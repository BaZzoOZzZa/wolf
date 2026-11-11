#include "control_loop.hpp"
#include "control.hpp"
#include "input_handler.hpp"
#include <array>
#include <map>
#include <sys/socket.h>

namespace control {
std::pair<std::string, int> get_ip(const sockaddr *);
}
namespace control::loop {
using namespace moonlight::control;
using namespace wolf::core;
Operations Operations::defaults() {
  return {std::make_shared<int>(0),
          [](void *, const char *ip, uint16_t port, size_t peers) {
            ENetAddress address{};
            if (enet_address_set_host(&address, ip) < 0)
              return static_cast<ENetHost *>(nullptr);
            enet_address_set_port(&address, port);
            return enet_host_create(AF_INET, &address, peers, 0, 0, 0);
          },
          [](void *, ENetHost *h, ENetEvent *e, uint32_t timeout) { return enet_host_service(h, e, timeout); },
          [](void *, const void *p, size_t n, uint32_t flags) { return enet_packet_create(p, n, flags); },
          [](void *, ENetPeer *p, uint8_t ch, ENetPacket *packet) { return enet_peer_send(p, ch, packet); },
          [](void *, ENetPacket *p) { enet_packet_destroy(p); },
          [](void *, ENetPeer *p) { enet_peer_reset(p); },
          [](void *, ENetPeer *p) { enet_peer_disconnect_now(p, 0); },
          [](void *, ENetHost *h) { enet_host_flush(h); },
          [](void *, ENetHost *h) { enet_host_destroy(h); }};
}
namespace {
struct Transport {
  Operations ops;
  ENetHost *host = nullptr; // execution owner only
  explicit Transport(Operations value) : ops(std::move(value)) {}
};
struct SenderLease {
  std::atomic_bool closed{false};
};
struct Outgoing {
  std::weak_ptr<ControlPeer> sender;
  std::string plaintext;
  bool charged = true;
};
struct Outbox {
  std::mutex mutex;
  bool closed = false;
  size_t retained = 0; // queue + popped batch + actual ENet-owned packets
  std::deque<Outgoing> pending;
  void release(size_t count) {
    std::lock_guard lock(mutex);
    retained -= count;
  }
  void close() {
    std::lock_guard lock(mutex);
    closed = true;
  }
};
struct PacketCell {
  ENetPacket *packet = nullptr;
  std::atomic_bool released{false};
  bool charged = false;
  static void marker(ENetPacket *packet) noexcept {
    static_cast<PacketCell *>(packet->userData)->released = true;
  }
  void take(ENetPacket *p) {
    packet = p;
    released = false;
    p->userData = this;
    p->freeCallback = marker;
  }
};
} // namespace
struct State : std::enable_shared_from_this<State> {
  const Config config;
  const state::SessionsAtoms sessions;
  const std::shared_ptr<events::EventBusType> bus;
  const std::shared_ptr<Transport> transport;
  const std::shared_ptr<ControlKeyRegistry> keys = std::make_shared<ControlKeyRegistry>();
  const std::shared_ptr<Outbox> outbox = std::make_shared<Outbox>();
  std::atomic_bool ready{false}, stop{false}, failed{false}, unknown{false};
  std::atomic_size_t calls{0};
  mutable std::mutex snapshot_mutex;
  std::vector<PeerLease> snapshots;
  std::optional<Clock::time_point> stop_at;
  const Clock::time_point started;
  bool attach_reserved = false;
  std::shared_ptr<InputSeat> pending_seat;
  // Owner-only below. State has NO thread/Owner backreference.
  std::thread::id execution;
  struct PeerState {
    events::StreamSession session;
    bool authenticated = false;
    std::shared_ptr<ControlPeer> sender;
    ControlKeyState *key_state = nullptr;
    PeerToken token;
    std::shared_ptr<SenderLease> lease;
  };
  std::map<ENetPeer *, PeerState> clients;
  std::map<size_t, ENetPeer *> owners;
  std::array<PacketCell, 256> packets;
  PacketCell received;
  bool release_uncertain = false;
  std::shared_ptr<InputSeat> seat;
  std::unique_ptr<protected_feedback::FeedbackDomain> feedback_domain;
  std::optional<protected_feedback::ProtectedFeedbackExecutor> feedback;
  State(Config c, state::SessionsAtoms s, std::shared_ptr<events::EventBusType> b, Operations o)
      : config(std::move(c)), sessions(std::move(s)), bus(std::move(b)),
        transport(std::make_shared<Transport>(std::move(o))), started(config.now()) {}
  void request_stop() noexcept {
    stop = true;
    try {
      auto now = config.now();
      {
        std::lock_guard lock(snapshot_mutex);
        if (!stop_at)
          stop_at = now;
      }
      outbox->close();
    } catch (...) {
      unknown = true;
    }
  }
  void deadline() {
    Clock::time_point now;
    try {
      now = config.now();
    } catch (...) {
      unknown = true;
      request_stop();
      return;
    }
    bool expired;
    {
      std::lock_guard lock(snapshot_mutex);
      expired = now - (stop_at ? *stop_at : started) >= config.decision_timeout && (stop_at || !ready);
    }
    if (expired) {
      unknown = true;
      request_stop();
    }
  }
  bool current(const events::StreamSession &session) {
    auto live = state::get_session_by_id(*sessions->load(), session.session_id);
    return live && same_control_generation(session, *live);
  }
  void publish() {
    std::vector<PeerLease> next;
    next.reserve(clients.size());
    for (const auto &[raw, peer] : clients)
      next.push_back(
          {peer.token, peer.session.connection_scope, peer.session.run_scope, peer.authenticated, peer.sender});
    {
      std::lock_guard lock(snapshot_mutex);
      next.swap(snapshots);
    }
    // Old sender/callback owners release OUTSIDE mutex.
  }
  void forget(ENetPeer *peer) {
    auto it = clients.find(peer);
    if (it == clients.end())
      return;
    it->second.lease->closed = true;
    auto owner = owners.find(it->second.session.session_id);
    if (owner != owners.end() && owner->second == peer)
      owners.erase(owner);
    clients.erase(it); // execution owner, no external mutex
    publish();
  }
  void retire_packets() {
    size_t credits = 0;
    for (auto &cell : packets)
      if (cell.packet && cell.released) {
        cell.packet = nullptr;
        if (cell.charged) {
          cell.charged = false;
          ++credits;
        }
      }
    if (received.packet && received.released)
      received.packet = nullptr;
    if (credits)
      outbox->release(credits);
  }
  bool packets_pending() const {
    if (received.packet)
      return true;
    for (const auto &cell : packets)
      if (cell.packet)
        return true;
    return false;
  }
  // One PRIVATE envelope includes operation + dispatch + all handler/payload
  // destructors. No public manual checkpoint; protected retirement runs last.
  bool frame(void (*body)(State &)) {
    struct Call {
      State *self;
      void (*body)(State &);
    } call{this, body};
    ++calls;
    bool ok = true;
    if (feedback) {
      ok = feedback->owner_call(&call, [](void *p) {
        auto &c = *static_cast<Call *>(p);
        c.body(*c.self);
      });
    } else {
      try {
        body(*this);
      } catch (...) {
        ok = false;
      }
    }
    retire_packets();
    --calls;
    if (!ok) {
      failed = true;
      request_stop();
    }
    return ok;
  }
  void send_packet(std::string_view bytes, ENetPeer *peer, Outgoing &request) {
    auto cell = std::find_if(packets.begin(), packets.end(), [](auto &c) { return !c.packet; });
    if (cell == packets.end())
      throw std::runtime_error("Control packet capacity exhausted");
    auto &ops = transport->ops;
    auto *p = ops.packet(ops.owner.get(), bytes.data(), bytes.size(), ENET_PACKET_FLAG_RELIABLE);
    if (!p)
      return;
    // Immediate nonallocating owner before any throwing work.
    cell->take(p);
    cell->charged = true;
    request.charged = false;
    bool caller = true;
    try {
      caller = false; // uncertain until send returns; never guessed double free
      if (ops.send(ops.owner.get(), peer, 0, p) < 0)
        caller = true;
    } catch (...) {
      unknown = true;
      throw;
    }
    if (caller) {
      try {
        ops.destroy_packet(ops.owner.get(), p);
      } catch (...) {
        release_uncertain = true;
        unknown = true;
        throw;
      }
    }
  }
  void drain_outbox() {
    struct Batch {
      std::shared_ptr<Outbox> queue;
      std::deque<Outgoing> items;
      ~Batch() {
        size_t credits = 0;
        for (auto &item : items)
          credits += item.charged;
        items.clear(); // actual plaintext/weak owners die outside queue lock
        if (credits)
          queue->release(credits);
      }
    } pending{outbox, {}};
    {
      std::lock_guard lock(outbox->mutex);
      pending.items.swap(outbox->pending);
    }
    if (stop)
      return;
    for (auto &request : pending.items) {
      auto sender = request.sender.lock();
      if (!sender)
        continue;
      for (auto &[peer, connection] : clients) {
        auto owner = owners.find(connection.session.session_id);
        if (connection.sender != sender || !connection.authenticated || !current(connection.session) ||
            owner == owners.end() || owner->second != peer || !connection.key_state ||
            connection.session.control_crypto->mode.load() != EncryptionMode::v2)
          continue;
        auto seq = connection.key_state->reserve_sequence();
        if (!seq)
          break;
        try {
          auto encrypted =
              encrypt_packet(connection.session.aes_key, *seq, request.plaintext, EncryptionMode::v2, Direction::host);
          send_packet({reinterpret_cast<const char *>(encrypted.get()), encrypted->full_size()}, peer, request);
        } catch (const std::runtime_error &e) {
          // Crypto failure is per packet; uncertain transport failure must stop.
          if (unknown)
            throw;
          logs::log(logs::warning, "[ENET] Outgoing encryption failed: {}", e.what());
        }
        break;
      }
    }
  }
  void reset(ENetPeer *peer) {
    try {
      transport->ops.reset(transport->ops.owner.get(), peer);
    } catch (...) {
      release_uncertain = true;
      unknown = true;
      throw;
    }
  }
  void disconnect(ENetPeer *peer) {
    try {
      transport->ops.disconnect(transport->ops.owner.get(), peer);
    } catch (...) {
      release_uncertain = true;
      unknown = true;
      throw;
    }
  }
  void dispatch(ENetEvent &event) {
    auto &ops = transport->ops;
    auto [client_ip, client_port] = get_ip((sockaddr *)&event.peer->address.address);
    if (event.type == ENET_EVENT_TYPE_CONNECT) {
      auto token = PeerToken::fresh(0); // EACH actual CONNECT allocation, not numeric authority
      if (clients.contains(event.peer)) {
        reset(event.peer); // actual old queue release before reused slot
        forget(event.peer);
      }
      auto session = match_control_session(sessions, client_ip, event.data);
      if (!session || clients.size() >= config.peers) {
        disconnect(event.peer);
        return;
      }
      auto sender = std::make_shared<ControlPeer>();
      auto lease = std::make_shared<SenderLease>();
      sender->send = [weak_outbox = std::weak_ptr<Outbox>(outbox),
                      weak_sender = std::weak_ptr<ControlPeer>(sender),
                      weak_crypto = std::weak_ptr<ControlCryptoState>(session->control_crypto),
                      lease](std::string_view payload) {
        auto crypto = weak_crypto.lock();
        if (lease->closed || !crypto || crypto->mode.load() != EncryptionMode::v2 || payload.size() > MAX_PAYLOAD_SIZE)
          return false;
        auto queue = weak_outbox.lock();
        if (!queue)
          return false;
        // Allocate the owned payload before locking, release on refusal outside.
        Outgoing request{weak_sender, std::string(payload), true};
        std::lock_guard lock(queue->mutex);
        if (queue->closed || lease->closed || queue->retained >= 256)
          return false;
        queue->pending.push_back(std::move(request));
        ++queue->retained;
        return true;
      };
      clients.emplace(event.peer, PeerState{*session, false, sender, nullptr, token, lease});
      publish();
      return; // CONNECT is NOT key-possession proof
    }
    auto it = clients.find(event.peer);
    if (it == clients.end()) {
      disconnect(event.peer);
      return;
    }
    auto &peer_state = it->second;
    auto &session = peer_state.session;
    if (!current(session)) {
      forget(event.peer);
      disconnect(event.peer);
      return;
    }
    auto owner = owners.find(session.session_id);
    bool is_owner = owner != owners.end() && owner->second == event.peer;
    if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
      forget(event.peer);
      return;
    }
    if (event.type != ENET_EVENT_TYPE_RECEIVE)
      return;
    try {
      auto mode = session.control_crypto->mode.load();
      auto bytes = std::string_view{reinterpret_cast<const char *>(event.packet->data), event.packet->dataLength};
      auto decrypted = decrypt_packet(bytes, session.aes_key, mode, Direction::client);
      if (!valid_control_packet(decrypted))
        return;
      auto type = read_packet<ControlPacket>(decrypted).type;
      if (type == INPUT_DATA && !valid_input_packet(decrypted))
        return;
      if (type == TERMINATION && decrypted.size() != sizeof(ControlTerminatePacket))
        return;
      if (type == IDR_FRAME && decrypted.size() != sizeof(ControlPacket) + 2)
        return;
      if (!is_owner && owner != owners.end()) {
        auto old = clients.find(owner->second);
        if (old != clients.end() && current(old->second.session))
          return;
      }
      if (mode == EncryptionMode::v2) {
        auto key_state = keys->find_or_insert(crypto::hex_to_str(session.aes_key, true));
        if (!key_state || !key_state->received.accept(boost::endian::little_to_native(
                              read_packet<uint32_t>(bytes.substr(sizeof(ControlPacket))))))
          return;
        peer_state.key_state = key_state;
      }
      if (!peer_state.authenticated) {
        peer_state.authenticated = true;
        owners[session.session_id] = event.peer;
        publish();
        bus->fire_event(immer::box<events::ResumeStreamEvent>(
            events::ResumeStreamEvent{.session_id = session.session_id, .connection_scope = session.connection_scope}));
      }
      if (!current(session))
        return;
      if (type == TERMINATION) {
        bus->fire_event(immer::box<events::PauseStreamEvent>(
            events::PauseStreamEvent{.session_id = session.session_id, .connection_scope = session.connection_scope}));
        forget(event.peer);
        disconnect(event.peer);
      } else if (type == INPUT_DATA)
        handle_input(session, {peer_state.sender}, decrypted);
      else if (type == IDR_FRAME)
        bus->fire_event(immer::box<events::IDRRequestEvent>(
            events::IDRRequestEvent{.session_id = session.session_id, .connection_scope = session.connection_scope}));
    } catch (const std::runtime_error &e) {
      if (release_uncertain)
        throw;
      logs::log(logs::warning, "[ENET] Invalid encrypted control packet: {}", e.what());
    }
  }
  void step() {
    drain_outbox();
    if (stop)
      return;
    ENetEvent event{};
    struct ReceiveOwner {
      State &s;
      ENetEvent &event;
      ~ReceiveOwner() {
        if (event.type != ENET_EVENT_TYPE_RECEIVE || !event.packet)
          return;
        s.received.take(event.packet); // transfer-full, no allocation/throwing work
        try {
          s.transport->ops.destroy_packet(s.transport->ops.owner.get(), event.packet);
        } catch (...) {
          s.release_uncertain = true;
          s.unknown = true;
          s.request_stop();
        }
      }
    } packet{*this, event}; // precedes actual service, covers valid output even on throw
    int count = transport->ops.service(transport->ops.owner.get(), transport->host, &event, 10);
    if (count < 0)
      throw std::runtime_error("ENet service failed");
    if (!count)
      return;
    dispatch(event);
  }
  void attach() {
    std::shared_ptr<InputSeat> next;
    {
      std::lock_guard lock(snapshot_mutex);
      next.swap(pending_seat);
    }
    if (!next)
      return;
    seat = std::move(next);
    feedback_domain = std::make_unique<protected_feedback::FeedbackDomain>();
    auto t = transport;
    protected_feedback::EnetOperations ops{t,
                                           [](void *p, const void *b, size_t n, uint32_t f) {
                                             auto &t = *static_cast<Transport *>(p);
                                             return t.ops.packet(t.ops.owner.get(), b, n, f);
                                           },
                                           [](void *p, ENetPeer *r, uint8_t c, ENetPacket *b) {
                                             auto &t = *static_cast<Transport *>(p);
                                             return t.ops.send(t.ops.owner.get(), r, c, b);
                                           },
                                           [](void *p, ENetPacket *b) {
                                             auto &t = *static_cast<Transport *>(p);
                                             t.ops.destroy_packet(t.ops.owner.get(), b);
                                           },
                                           [](void *p, ENetPeer *r) {
                                             auto &t = *static_cast<Transport *>(p);
                                             t.ops.reset(t.ops.owner.get(), r);
                                           },
                                           nullptr};
    feedback = feedback_domain->create(seat, std::move(ops));
    if (!feedback || !feedback->bind_current_thread())
      throw std::runtime_error("Feedback owner setup failed");
  }
  void pump_feedback() {
    if (!feedback || stop)
      return;
    auto grant = seat->current();
    if (!grant)
      return;
    for (auto &[raw, peer] : clients) {
      auto owner = owners.find(peer.session.session_id);
      if (!peer.authenticated || !peer.token.matches(grant->owner.peer) || !peer.key_state ||
          !peer.session.connection_scope.matches(grant->owner.connection) ||
          !peer.session.run_scope.matches(grant->owner.source) || owner == owners.end() || owner->second != raw ||
          !current(peer.session))
        continue;
      auto live = state::get_session_by_id(*sessions->load(), peer.session.session_id);
      if (!live)
        return;
      protected_feedback::CurrentOwnerSource source{
          shared_from_this(),
          [](void *p,
             ENetPeer &raw,
             const PeerToken &token,
             const std::shared_ptr<ControlPeer> &sender,
             const events::StreamSession &session,
             ControlKeyState &key) {
            auto &s = *static_cast<State *>(p);
            auto it = s.clients.find(&raw);
            if (it == s.clients.end())
              return false;
            auto own = s.owners.find(session.session_id);
            return it->second.authenticated && it->second.token.matches(token) && it->second.sender == sender &&
                   it->second.key_state == &key && own != s.owners.end() && own->second == &raw &&
                   same_control_generation(it->second.session, session) && s.current(session);
          }};
      protected_feedback::OwnerPeerView view(*raw,
                                             peer.session,
                                             *live,
                                             peer.token,
                                             peer.sender,
                                             peer.sender,
                                             keys,
                                             *peer.key_state,
                                             grant,
                                             seat->generation(),
                                             std::move(source));
      feedback->pump_one(view);
      return;
    }
  }
  void shutdown() {
    outbox->close();
    for (auto &[raw, peer] : clients)
      peer.lease->closed = true;
    drain_outbox();
    if (feedback)
      feedback->request_close(); // explicit WHOLE loop shutdown, not stale scope error
    if (transport->host) {
      for (size_t i = 0; i < transport->host->peerCount; ++i) {
        auto *peer = &transport->host->peers[i];
        reset(peer);
        disconnect(peer);
      }
      transport->ops.flush(transport->ops.owner.get(), transport->host);
      auto *host = std::exchange(transport->host, nullptr); // uncertain destroy is never retried
      transport->ops.destroy_host(transport->ops.owner.get(), host);
    }
    clients.clear();
    owners.clear();
    publish();
  }
  void run() noexcept {
    execution = std::this_thread::get_id();
    try {
      if (!stop) {
        transport->host =
            transport->ops.create(transport->ops.owner.get(), config.host.c_str(), config.port, config.peers);
        if (!transport->host)
          throw std::runtime_error("ENet host creation failed");
        if (!stop)
          ready = true;
      }
      while (!stop) {
        attach();
        if (!frame([](State &s) { s.step(); }))
          break;
        pump_feedback();
      }
    } catch (...) {
      failed = true;
      request_stop();
    }
    bool cleaned = !release_uncertain && frame([](State &s) { s.shutdown(); });
    if (!cleaned || packets_pending() || (feedback && feedback->status().packet_live)) {
      // Uncertain free/callback is NOT exit. Retain State/host/cell forever if no
      // completion can be established; no detach/retry/cross-thread guessed free.
      unknown = true;
      for (;;)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // Actual owner frames already drained; no active callback/packet remains.
    feedback.reset();
    feedback_domain.reset();
  }
};
ControlLoopOwner::ControlLoopOwner(std::shared_ptr<State> s, std::shared_future<void> exit)
    : state_(std::move(s)), exited_(std::move(exit)) {}
void ControlLoopOwner::request_stop() noexcept {
  state_->request_stop();
}
Status ControlLoopOwner::status() const {
  state_->deadline();
  size_t count;
  {
    std::lock_guard lock(state_->snapshot_mutex);
    count = state_->snapshots.size();
  }
  return {state_->ready,
          state_->stop,
          exited_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready,
          joined_,
          state_->failed,
          state_->unknown,
          count,
          state_->calls};
}
std::vector<PeerLease> ControlLoopOwner::peers() const {
  std::lock_guard lock(state_->snapshot_mutex);
  return state_->snapshots;
}
bool ControlLoopOwner::attach_feedback(std::shared_ptr<InputSeat> seat) {
  if (!seat || state_->stop)
    return false;
  std::lock_guard lock(state_->snapshot_mutex);
  if (state_->attach_reserved || state_->stop)
    return false;
  state_->attach_reserved = true;
  state_->pending_seat.swap(seat);
  return true;
}
JoinResult ControlLoopOwner::join_when_exited() {
  if (exited_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
    return status().unknown ? JoinResult::Unknown : JoinResult::Refused;
  std::lock_guard lock(join_mutex_);
  if (worker_.joinable()) {
    if (worker_.get_id() == std::this_thread::get_id())
      return JoinResult::Refused;
    worker_.join();
  }
  joined_ = true;
  return JoinResult::Joined;
}
ControlLoopOwner::~ControlLoopOwner() {
  request_stop();
  // Lifecycle owner only; never on execution stack. Actual join includes TLS and
  // callable destruction. Unknown may wait indefinitely; passive wait is not IO.
  if (worker_.joinable())
    worker_.join();
}
std::shared_ptr<ControlLoopOwner> ControlLoopDomain::start(Config config,
                                                           state::SessionsAtoms sessions,
                                                           std::shared_ptr<events::EventBusType> bus,
                                                           Operations ops,
                                                           ThreadFactory factory) {
  {
    std::lock_guard lock(mutex_);
    if (reserved_)
      return {};
    reserved_ = true;
  }
  if (!sessions || !bus || !factory || !config.now || !config.peers || config.peers > 20 ||
      config.decision_timeout.count() <= 0 || !ops.owner || !ops.create || !ops.service || !ops.packet || !ops.send ||
      !ops.destroy_packet || !ops.reset || !ops.disconnect || !ops.flush || !ops.destroy_host)
    return {};
  auto state = std::make_shared<State>(std::move(config), std::move(sessions), std::move(bus), std::move(ops));
  auto promise = std::make_shared<std::promise<void>>();
  auto result = std::shared_ptr<ControlLoopOwner>(new ControlLoopOwner(state, promise->get_future().share()));
  {
    std::lock_guard lock(mutex_);
    owner_ = result;
  }
  try {
    result->worker_ = factory([state, promise] {
      state->run();
      promise->set_value_at_thread_exit();
    });
    if (!result->worker_.joinable())
      throw std::runtime_error("Thread factory returned no worker");
  } catch (...) {
    state->failed = true;
    state->request_stop();
    // Trusted factory: no escaped worker on throw/nonjoinable, parameter captures
    // already retired outside all locks. No resource was published by a worker.
    promise->set_value();
  }
  return result;
}
ControlLoopDomain::~ControlLoopDomain() {
  auto owner = std::move(owner_);
  if (!owner)
    return;
  owner->request_stop();
  while (owner->join_when_exited() != JoinResult::Joined)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
}
} // namespace control::loop
