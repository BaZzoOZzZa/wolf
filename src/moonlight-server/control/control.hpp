#pragma once

#include <chrono>
#include <deque>
#include <enet/enet.h>
#include <events/events.hpp>
#include <functional>
#include <helpers/logger.hpp>
#include <moonlight/control.hpp>
#include <mutex>
#include <range/v3/view.hpp>
#include <state/data-structures.hpp>
#include <thread>

namespace control {

using namespace std::chrono_literals;
using namespace wolf::core;

// Strong in-process scope identifies a generation; transport secrets only correlate it. A stable
// paired-client session_id alone must never authorize an old ENet connection.
inline bool same_control_generation(const events::StreamSession &a, const events::StreamSession &b) {
  return a.connection_scope.matches(b.connection_scope) && a.run_scope.matches(b.run_scope) &&
         a.control_crypto == b.control_crypto && a.session_id == b.session_id &&
         a.enet_secret_payload == b.enet_secret_payload && a.rtp_secret_payload == b.rtp_secret_payload &&
         a.aes_key == b.aes_key && a.aes_iv == b.aes_iv;
}

std::optional<events::StreamSession> match_control_session(const state::SessionsAtoms &running_sessions,
                                                           std::string_view client_ip,
                                                           std::uint32_t connect_token);

// Device callbacks may outlive an ENet peer. They retain only this revocable
// sender, never an ENet address/slot. All encryption and ENet calls stay on the
// control thread.
struct ControlPeer {
  std::function<bool(std::string_view)> send;
};

bool queue_feedback(std::string_view payload, immer::box<std::shared_ptr<ControlPeer>> connected_client);

bool init();

} // namespace control