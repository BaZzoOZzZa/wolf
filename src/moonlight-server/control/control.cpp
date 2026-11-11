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

bool init() {
  auto error_code = enet_initialize();
  if (error_code != 0) {
    logs::log(logs::error, "An error occurred while initializing Enet: {}.", error_code);
    return false;
  }
  return true;
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

bool queue_feedback(std::string_view payload, immer::box<std::shared_ptr<ControlPeer>> connected_client) {
  return *connected_client && (*connected_client)->send && (*connected_client)->send(payload);
}

std::optional<events::StreamSession> match_control_session(const state::SessionsAtoms &running_sessions,
                                                           std::string_view client_ip,
                                                           std::uint32_t connect_token) {
  std::optional<events::StreamSession> match;
  for (const auto &session : *running_sessions->load()) {
    if (!session.connection_scope || !session.run_scope)
      continue;
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

} // namespace control
