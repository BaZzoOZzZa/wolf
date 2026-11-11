#include "core/input.hpp"
#include <control/control.hpp>
#include <control/input_handler.hpp>
#include <events/events.hpp>
#include <immer/box.hpp>
#include <map>
#include <state/sessions.hpp>
#include <sys/socket.h>

namespace control {

using namespace ranges;
using namespace moonlight::control;
using namespace wolf::core::events;

void free_host(ENetHost *host) {
  std::for_each(host->peers, host->peers + host->peerCount, [](ENetPeer &peer_ref) {
    ENetPeer *peer = &peer_ref;

    if (peer) {
      enet_peer_disconnect_now(peer, 0);
    }
  });

  enet_host_destroy(host);
}

using enet_host = std::unique_ptr<ENetHost, decltype(&free_host)>;
using enet_packet = std::unique_ptr<ENetPacket, decltype(&enet_packet_destroy)>;

bool init() {
  auto error_code = enet_initialize();
  if (error_code != 0) {
    logs::log(logs::error, "An error occurred while initializing Enet: {}.", error_code);
    return false;
  }
  return true;
}

enet_host create_host(std::string_view host, std::uint16_t port, std::size_t peers) {
  ENetAddress addr;
  enet_address_set_host(&addr, host.data());
  enet_address_set_port(&addr, port);

  auto enet_host = enet_host_create(AF_INET, &addr, peers, 0, 0, 0);
  if (enet_host == nullptr) {
    logs::log(logs::error, "An error occurred while trying to create an ENet server host.");
  }

  return {enet_host, free_host};
}

/**
 * The Moonlight fork of ENET doesn't include host and port as easily accessible parts of the struct,
 * we have to extract them manually
 */
std::pair<std::string /* ip */, int /* port */> get_ip(const sockaddr *const ip_addr) {
  char data[INET6_ADDRSTRLEN]{};

  auto family = ip_addr->sa_family;
  std::uint16_t port = 0;
  if (family == AF_INET6) {
    inet_ntop(AF_INET6, &((sockaddr_in6 *)ip_addr)->sin6_addr, data, INET6_ADDRSTRLEN);
    port = ((sockaddr_in6 *)ip_addr)->sin6_port;
  }

  if (family == AF_INET) {
    inet_ntop(AF_INET, &((sockaddr_in *)ip_addr)->sin_addr, data, INET_ADDRSTRLEN);
    port = ((sockaddr_in *)ip_addr)->sin_port;
  }

  return {std::string{data}, port};
}

bool send_packet(std::string_view payload, ENetPeer *peer) {
  logs::log(logs::trace, "[ENET] Sending packet");
  auto packet = enet_packet_create(payload.data(), payload.size(), ENET_PACKET_FLAG_RELIABLE);
  if (!packet)
    return false;
  if (enet_peer_send(peer, 0, packet) < 0) {
    logs::log(logs::warning, "[ENET] Failed to send packet");
    enet_packet_destroy(packet);
    return false;
  }
  return true;
}

bool queue_feedback(std::string_view payload, immer::box<std::shared_ptr<ControlPeer>> connected_client) {
  return *connected_client && (*connected_client)->send && (*connected_client)->send(payload);
}

std::optional<events::StreamSession> match_control_session(const state::SessionsAtoms &running_sessions,
                                                           std::string_view client_ip,
                                                           std::uint32_t connect_token) {
  std::optional<events::StreamSession> match;
  for (const auto &session : *running_sessions->load()) {
    // Legacy clients send zero. Only that explicit legacy case may select by
    // IP, and only when unambiguous. Selection is NOT authentication.
    bool candidate = connect_token != 0
                         ? session.enet_secret_payload == connect_token
                         : session.control_crypto->mode.load() == EncryptionMode::legacy && session.ip == client_ip;
    if (candidate) {
      if (match)
        return std::nullopt;
      match = session;
    }
  }
  return match;
}

void run_control(int port,
                 const state::SessionsAtoms &running_sessions,
                 const std::shared_ptr<events::EventBusType> &event_bus,
                 int peers,
                 std::chrono::milliseconds timeout,
                 const std::string &host_ip) {

  enet_host host = create_host(host_ip, port, peers);
  logs::log(logs::info, "Control server started on port: {}", port);

  if (!host)
    return;

  struct PeerState {
    events::StreamSession session;
    bool authenticated = false;
    std::shared_ptr<ControlPeer> sender;
    ControlKeyState *key_state = nullptr;
  };
  // ENet peers, association and ownership are touched only by this thread.
  std::map<ENetPeer *, PeerState> clients;
  ControlKeyRegistry key_registry;
  struct Outgoing {
    std::weak_ptr<ControlPeer> sender;
    std::string plaintext;
  };
  struct Outbox {
    std::mutex mutex;
    std::deque<Outgoing> pending;
  };
  auto outbox = std::make_shared<Outbox>();
  std::map<std::size_t, ENetPeer *> owners;

  auto current = [&](const events::StreamSession &session) {
    auto sessions = running_sessions->load();
    auto live = state::get_session_by_id(*sessions, session.session_id);
    return live && same_control_generation(session, *live);
  };
  auto forget = [&](ENetPeer *peer) {
    if (auto it = clients.find(peer); it != clients.end()) {
      auto owner = owners.find(it->second.session.session_id);
      if (owner != owners.end() && owner->second == peer)
        owners.erase(owner);
      clients.erase(it);
    }
  };

  ENetEvent event{};
  while (true) {
    std::deque<Outgoing> pending;
    {
      std::lock_guard lock(outbox->mutex);
      pending.swap(outbox->pending);
    }
    for (const auto &request : pending) {
      auto sender = request.sender.lock();
      if (!sender)
        continue;
      for (auto &[peer, connection] : clients) {
        auto owner = owners.find(connection.session.session_id);
        if (connection.sender != sender || !connection.authenticated || !current(connection.session) ||
            owner == owners.end() || owner->second != peer || !connection.key_state ||
            connection.session.control_crypto->mode.load() != EncryptionMode::v2)
          continue;
        // Reserve before encryption/send; failures consume a nonce too.
        auto seq = connection.key_state->reserve_sequence();
        if (!seq)
          break; // rekey required, never wrap
        try {
          auto encrypted =
              encrypt_packet(connection.session.aes_key, *seq, request.plaintext, EncryptionMode::v2, Direction::host);
          send_packet({reinterpret_cast<const char *>(encrypted.get()), encrypted->full_size()}, peer);
        } catch (const std::runtime_error &e) {
          logs::log(logs::warning, "[ENET] Outgoing encryption failed: {}", e.what());
        }
        break;
      }
    }
    if (enet_host_service(host.get(), &event, std::min<std::int64_t>(timeout.count(), 10)) <= 0)
      continue;
    // Own every received packet, including rejected/unassociated traffic.
    enet_packet packet{event.type == ENET_EVENT_TYPE_RECEIVE ? event.packet : nullptr, enet_packet_destroy};
    auto [client_ip, client_port] = get_ip((sockaddr *)&event.peer->address.address);
    if (event.type == ENET_EVENT_TYPE_CONNECT) {
      forget(event.peer); // a recycled ENet slot is a new, unauthenticated peer
      auto session = match_control_session(running_sessions, client_ip, event.data);
      if (!session) {
        enet_peer_disconnect_now(event.peer, 0);
        continue;
      }
      auto sender = std::make_shared<ControlPeer>();
      sender->send =
          [weak_outbox = std::weak_ptr<Outbox>(outbox),
           weak_sender = std::weak_ptr<ControlPeer>(sender),
           weak_crypto = std::weak_ptr<ControlCryptoState>(session->control_crypto)](std::string_view payload) {
            auto crypto = weak_crypto.lock();
            if (!crypto || crypto->mode.load() != EncryptionMode::v2)
              return false;
            auto queue = weak_outbox.lock();
            if (!queue || payload.size() > MAX_PAYLOAD_SIZE)
              return false;
            std::lock_guard lock(queue->mutex);
            if (queue->pending.size() >= 256)
              return false;
            queue->pending.push_back({weak_sender, std::string(payload)});
            return true;
          };
      clients.emplace(event.peer, PeerState{.session = *session, .sender = sender});
      // CONNECT (including possession of the 32-bit token) is not proof of key possession.
      continue;
    }

    auto it = clients.find(event.peer);
    if (it == clients.end()) {
      enet_peer_disconnect_now(event.peer, 0);
      continue;
    }
    auto &peer_state = it->second;
    auto &session = peer_state.session;
    if (!current(session)) {
      forget(event.peer);
      enet_peer_disconnect_now(event.peer, 0);
      continue;
    }
    auto owner = owners.find(session.session_id);
    bool is_owner = owner != owners.end() && owner->second == event.peer;
    if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
      // Transport disconnects are unauthenticated. In particular they must not
      // pause a newer generation through the session-id-only event bus.
      forget(event.peer);
      continue;
    }
    if (event.type != ENET_EVENT_TYPE_RECEIVE)
      continue;

    try {
      auto mode = session.control_crypto->mode.load();
      auto bytes = std::string_view{reinterpret_cast<const char *>(packet->data), packet->dataLength};
      auto decrypted = decrypt_packet(bytes, session.aes_key, mode, Direction::client);
      if (!valid_control_packet(decrypted))
        continue;
      auto type = read_packet<ControlPacket>(decrypted).type;
      if (type == INPUT_DATA && !valid_input_packet(decrypted))
        continue;
      if (type == TERMINATION && decrypted.size() != sizeof(ControlTerminatePacket))
        continue;
      if (type == IDR_FRAME && decrypted.size() != sizeof(ControlPacket) + 2)
        continue;
      // No takeover of a currently authenticated generation by a second peer.
      if (!is_owner && owner != owners.end()) {
        auto old = clients.find(owner->second);
        if (old != clients.end() && current(old->second.session))
          continue;
      }
      if (mode == EncryptionMode::v2) {
        auto key_state = key_registry.find_or_insert(crypto::hex_to_str(session.aes_key, true));
        if (!key_state || !key_state->received.accept(boost::endian::little_to_native(
                              read_packet<std::uint32_t>(bytes.substr(sizeof(ControlPacket))))))
          continue;
        peer_state.key_state = key_state;
      }
      if (!peer_state.authenticated) {
        peer_state.authenticated = true;
        owners[session.session_id] = event.peer;
        event_bus->fire_event(immer::box<ResumeStreamEvent>(ResumeStreamEvent{.session_id = session.session_id}));
      }
      // Recheck after synchronous event handlers; a resume may have revoked us.
      if (!current(session))
        continue;
      if (type == TERMINATION) {
        event_bus->fire_event(immer::box<PauseStreamEvent>(PauseStreamEvent{.session_id = session.session_id}));
        forget(event.peer);
        enet_peer_disconnect_now(event.peer, 0);
      } else if (type == INPUT_DATA) {
        handle_input(session, {peer_state.sender}, decrypted);
      } else if (type == IDR_FRAME) {
        event_bus->fire_event(immer::box<IDRRequestEvent>(IDRRequestEvent{.session_id = session.session_id}));
      }
    } catch (const std::runtime_error &e) {
      logs::log(logs::warning, "[ENET] Invalid encrypted control packet: {}", e.what());
    }
  }
}

} // namespace control
