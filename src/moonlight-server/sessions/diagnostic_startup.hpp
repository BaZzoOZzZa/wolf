#pragma once
#include <events/events.hpp>
#include <state/sessions.hpp>
#include <streaming/streaming.hpp>

namespace wolf::core::sessions::diagnostic {
inline void validate(const config::BaseApp &app) {
  if (app.legacy_direct_touch_trial.value_or(false)) {
    if (!app.legacy_pad_only_trial.value_or(false) || !app.direct_window_target)
      throw std::invalid_argument("direct touch requires PadOnly and an exact target");
    const auto &t = *app.direct_window_target;
    if (!t.xid || !t.pid || !t.root || !t.process_start_time || t.screen > 63 ||
        (app.video && (app.video->source || app.video->video_params)))
      throw std::invalid_argument("direct touch requires a valid target and canonical full-frame video source/params");
  } else if (app.direct_window_target) {
    throw std::invalid_argument("direct target requires explicit direct touch opt-in");
  }
  const int slot = app.legacy_pad_slot.value_or(0);
  if (slot < 0 || slot >= moonlight::control::CONTROL_SLOT_COUNT)
    throw std::invalid_argument("legacy_pad_slot must be in 0..15");
  if (app.legacy_pad_only_trial.value_or(false) &&
      (app.device_free_diagnostic.value_or(false) || app.start_virtual_compositor.value_or(true) ||
       app.start_audio_server.value_or(true) || !rfl::holds_alternative<config::AppCMD>(app.runner.variant())))
    throw std::invalid_argument("legacy_pad_only_trial requires no diagnostic/compositor/audio and a process runner");
  if (app.device_free_diagnostic.value_or(false) &&
      (app.start_virtual_compositor.value_or(true) || app.start_audio_server.value_or(true) ||
       !rfl::holds_alternative<config::AppCMD>(app.runner.variant())))
    throw std::invalid_argument("device_free_diagnostic requires no compositor/audio and a process runner");
}
inline bool disabled(const events::StreamSession &session) {
  return session.app && events::device_free(*session.app);
}
inline bool pad_only(const events::StreamSession &session) {
  return session.app && events::pad_only_trial(*session.app);
}
// Both opt-ins forbid non-pad input, startup devices and device/lobby aliasing.
// Only device_free forbids the selected trial Xbox input as well.
inline bool isolated(const events::StreamSession &session) {
  return disabled(session) || pad_only(session);
}
inline bool isolated_target(const events::Lobby &lobby, const immer::vector<events::StreamSession> &running) {
  for (const auto &s : running)
    if (s.run_scope.matches(lobby.run_scope) && isolated(s))
      return true;
  auto members = lobby.members->load();
  for (const auto &[id, member] : *members)
    if (member && isolated(*member))
      return true;
  return false;
}
inline bool queue_device(const immer::box<events::PlugDeviceEvent> &event,
                         const immer::vector<events::StreamSession> &running,
                         const immer::vector<events::Lobby> &lobbies,
                         const RunScope &run,
                         const std::shared_ptr<events::devices_atom_queue> &queue) {
  auto source = state::get_session_by_scope(running, event->connection_scope.get());
  if (!source || isolated(*source))
    return false;
  for (const auto &target : running)
    if (target.run_scope.matches(run) && isolated(target))
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
  if (app.legacy_direct_touch_trial.get() && (!events::pad_only_trial(app) || !app.direct_window_input.get())) {
    ops.failed();
    return {};
  }
  const bool restricted = events::device_free(app) || events::pad_only_trial(app);
  if ((restricted && (app.start_virtual_compositor || app.start_audio_server)) ||
      (events::pad_only_trial(app) && (events::device_free(app) || app.legacy_pad_slot.get() < 0 ||
                                       app.legacy_pad_slot.get() >= moonlight::control::CONTROL_SLOT_COUNT))) {
    ops.failed();
    return {};
  }
  boost::future<streaming::WaylandDisplayReady> ready;
  if (app.start_virtual_compositor) {
    ready = ops.compositor();
  } else {
    if (!restricted) {
      ops.mouse();
      ops.keyboard();
    }
  }
  if (app.start_audio_server)
    ops.audio();
  if (!app.start_virtual_compositor) {
    ops.runner();
    return {};
  }
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
