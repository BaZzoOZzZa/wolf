#pragma once
#include <events/events.hpp>
#include <state/sessions.hpp>
#include <streaming/streaming.hpp>

namespace wolf::core::sessions::diagnostic {
inline void validate(const config::BaseApp &app) {
  if (app.device_free_diagnostic.value_or(false) &&
      (app.start_virtual_compositor.value_or(true) || app.start_audio_server.value_or(true) ||
       !rfl::holds_alternative<config::AppCMD>(app.runner.variant())))
    throw std::invalid_argument("device_free_diagnostic requires no compositor/audio and a process runner");
}
inline bool disabled(const events::StreamSession &session) {
  return session.app && events::device_free(*session.app);
}
inline bool queue_device(const immer::box<events::PlugDeviceEvent> &event,
                         const immer::vector<events::StreamSession> &running,
                         const immer::vector<events::Lobby> &lobbies,
                         const RunScope &run,
                         const std::shared_ptr<events::devices_atom_queue> &queue) {
  auto source = state::get_session_by_scope(running, event->connection_scope.get());
  if (!source || disabled(*source))
    return false;
  return state::queue_own_session_device(event, running, lobbies, run, queue);
}
// Trusted owned operations; production passes actual factories and captured session.
// Tests substitute IO only. Invocations and last capture release occur outside locks.
struct StartupOps {
  std::function<boost::future<streaming::WaylandDisplayReady>()> compositor;
  std::function<void()> mouse, keyboard, audio;
  std::function<void(const streaming::WaylandDisplayReady &)> bind_wayland;
  std::function<bool(const streaming::WaylandDisplayReady &)> wait_wayland;
  std::function<void()> runner, failed;
};
inline boost::future<void> start(const events::App &app, StartupOps ops) {
  if (events::device_free(app) && (app.start_virtual_compositor || app.start_audio_server)) {
    ops.failed();
    return {};
  }
  boost::future<streaming::WaylandDisplayReady> ready;
  if (app.start_virtual_compositor) {
    ready = ops.compositor();
  } else {
    if (!events::device_free(app)) {
      ops.mouse();
      ops.keyboard();
    }
    // RED extraction: preserve the old empty readiness/Wayland continuation.
    boost::promise<streaming::WaylandDisplayReady> promise;
    ready = promise.get_future();
    promise.set_value({});
  }
  if (app.start_audio_server)
    ops.audio();
  return ready.then([ops = std::move(ops)](auto future) {
    auto display = future.get();
    ops.bind_wayland(display);
    if (!ops.wait_wayland(display)) {
      ops.failed();
      return;
    }
    ops.runner();
  });
}
// Exact FALSE is opt-out. This is worker dispatch, not an mDNS dependency fix.
template <class Launch> void dispatch_mdns(std::string_view setting, Launch &&launch) {
  if (setting != "FALSE")
    std::forward<Launch>(launch)();
}
} // namespace wolf::core::sessions::diagnostic
