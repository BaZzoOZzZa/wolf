#include <atomic>
#include <immer/array_transient.hpp>
#include <immer/map_transient.hpp>
#include <immer/vector_transient.hpp>
#include <sessions/common.hpp>
#include <sessions/handlers.hpp>
#include <state/sessions.hpp>
#include <streaming/consumer_events.hpp>
#include <streaming/streaming.hpp>

namespace wolf::core::sessions {

bool consumer_legacy_current(const state::SessionsAtoms &running, const ConnectionScope &scope) {
  return state::get_session_by_scope(running->load(), scope).has_value();
}

struct OwnedDeviceQueue {
  RunScope scope;
  std::shared_ptr<events::devices_atom_queue> queue;
};
using session_devices = immer::map<std::string, OwnedDeviceQueue>;

immer::vector<immer::box<events::EventBusHandlers>>
setup_moonlight_handlers(const immer::box<state::AppState> &app_state,
                         const std::string &runtime_dir,
                         const std::optional<AudioServer> &audio_server) {
  auto composition = make_consumer_composition(app_state->running_sessions);
  return setup_moonlight_handlers(app_state, runtime_dir, audio_server, consumer_runtime(composition));
}

immer::vector<immer::box<events::EventBusHandlers>>
setup_moonlight_handlers(const immer::box<state::AppState> &app_state,
                         const std::string &runtime_dir,
                         const std::optional<AudioServer> &audio_server,
                         std::shared_ptr<streaming::ConsumerRuntime> consumers) {
  if (!consumers)
    throw std::invalid_argument("Moonlight handlers require the shared consumer runtime");
  immer::vector_transient<immer::box<events::EventBusHandlers>> handlers;

  /*
   * A queue of devices that are waiting to be plugged, mapped by session_id
   * This way we can accumulate devices here until the docker container is up and running
   */
  auto plugged_devices_queue = std::make_shared<immer::atom<session_devices>>();

  // A connection stop never destroys a persistent game/runner or its devices.
  auto stop_connections = [app_state](const RunScope &run) {
    auto snapshot = app_state->running_sessions->load();
    for (const auto &session : *snapshot) {
      if (session.run_scope.matches(run)) {
        app_state->event_bus->fire_event(immer::box<events::StopStreamEvent>{
            events::StopStreamEvent{.session_id = session.session_id, .connection_scope = session.connection_scope}});
      }
    }
  };
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::GameStopRequest>>(
      [stop_connections](const immer::box<events::GameStopRequest> &ev) {
        if (ev->run_scope.get() && ev->run_scope.get().id() == ev->session_id)
          stop_connections(ev->run_scope.get());
      }));
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::RunEnded>>(
      [app_state, plugged_devices_queue, stop_connections](const immer::box<events::RunEnded> &ev) {
        const auto run = ev->run_scope.get();
        if (!run || run.id() != ev->session_id)
          return;
        stop_connections(run);
        app_state->running_sessions->update([run](const auto &all) {
          return all | ranges::views::filter([&](const auto &s) { return !s.run_scope.matches(run); }) |
                 ranges::to<immer::vector<events::StreamSession>>();
        });
        plugged_devices_queue->update([run](const auto &map) {
          auto entry = map.find(run.id());
          return entry && entry->scope.matches(run) ? map.erase(run.id()) : map;
        });
      }));

  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::PlugDeviceEvent>>(
      [plugged_devices_queue, app_state, lobbies = app_state->lobbies](
          const immer::box<events::PlugDeviceEvent> &hotplug_ev) {
        auto queues = plugged_devices_queue->load();
        if (auto entry = queues->find(hotplug_ev->session_id)) {
          state::queue_own_session_device(hotplug_ev,
                                          app_state->running_sessions->load(),
                                          lobbies->load(),
                                          entry->scope,
                                          entry->queue);
        }
      }));

  // Run process and our custom wayland as soon as a new StreamSession is created
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::StreamSession>>(
      [=](const immer::box<events::StreamSession> &session) {
        if (!session->run_scope ||
            !state::get_session_by_scope(app_state->running_sessions->load(), session->connection_scope))
          return;
        /* Initialise plugged device queue */
        auto devices_q = std::make_shared<events::devices_atom_queue>();
        plugged_devices_queue->update([=](const session_devices map) {
          return map.set(std::to_string(session->session_id), OwnedDeviceQueue{session->run_scope, devices_q});
        });

        std::shared_ptr<boost::promise<streaming::WaylandDisplayReady>> on_ready =
            std::make_shared<boost::promise<streaming::WaylandDisplayReady>>();

        if (session->app->start_virtual_compositor) {
          logs::log(logs::debug, "[STREAM_SESSION] Create wayland compositor");

          // Start Gstreamer producer pipeline
          std::thread([session, on_ready, gst_context = app_state->gst_context]() {
            streaming::start_video_producer(session->run_scope,
                                            session->app->video_producer_buffer_caps,
                                            session->app->render_node,
                                            {.width = session->display_mode.width,
                                             .height = session->display_mode.height,
                                             .refreshRate = session->display_mode.refreshRate},
                                            gst_context,
                                            on_ready,
                                            session->event_bus);
          }).detach();
        } else {
          // Create virtual devices
          auto mouse = input::Mouse::create();
          if (!mouse) {
            logs::log(logs::error, "Failed to create mouse: {}", mouse.getErrorMessage());
          } else {
            auto mouse_ptr = input::Mouse(std::move(*mouse));
            devices_q->push(immer::box<events::PlugDeviceEvent>(
                events::PlugDeviceEvent{.session_id = std::to_string(session->session_id),
                                        .udev_events = mouse_ptr.get_udev_events(),
                                        .udev_hw_db_entries = mouse_ptr.get_udev_hw_db_entries(),
                                        .run_scope = session->run_scope,
                                        .connection_scope = session->connection_scope}));
            session->mouse->emplace(std::move(mouse_ptr));
          }

          auto keyboard = input::Keyboard::create();
          if (!keyboard) {
            logs::log(logs::error, "Failed to create keyboard: {}", keyboard.getErrorMessage());
          } else {
            auto keyboard_ptr = input::Keyboard(std::move(*keyboard));
            devices_q->push(immer::box<events::PlugDeviceEvent>(
                events::PlugDeviceEvent{.session_id = std::to_string(session->session_id),
                                        .udev_events = keyboard_ptr.get_udev_events(),
                                        .udev_hw_db_entries = keyboard_ptr.get_udev_hw_db_entries(),
                                        .run_scope = session->run_scope,
                                        .connection_scope = session->connection_scope}));
            session->keyboard->emplace(std::move(keyboard_ptr));
          }
          on_ready->set_value({});
        }

        /* Create audio virtual sink */
        logs::log(logs::debug, "[STREAM_SESSION] Create virtual audio sink");
        auto pulse_sink_name = fmt::format("{}{}", VIRTUAL_SINK_PREFIX, session->session_id);
        std::shared_ptr<audio::VSink> v_device;
        if (session->app->start_audio_server && audio_server && audio_server->server) {
          v_device = audio::create_virtual_sink(
              audio_server->server,
              audio::AudioDevice{.sink_name = pulse_sink_name,
                                 .mode = state::get_audio_mode(session->audio_channel_count, true)});
          session->audio_sink->store(v_device);

          std::thread([session, audio_server = audio_server->server]() {
            auto sink_name = fmt::format("{}{}.monitor", VIRTUAL_SINK_PREFIX, session->session_id);
            streaming::start_audio_producer(session->run_scope,
                                            session->event_bus,
                                            session->audio_channel_count,
                                            sink_name,
                                            audio::get_server_name(audio_server));
          }).detach();
        }

        // TODO: timeout? What if the wayland display is never ready?
        auto w_display_ready = on_ready->get_future().then([session, runtime_dir](auto fut) {
          streaming::WaylandDisplayReady ready = fut.get();

          auto wl_state = virtual_display::create_wayland_display(ready.wayland_plugin, ready.wayland_socket_name);
          // Set the wayland display
          session->wayland_display->store(wl_state);

          // Set virtual devices
          session->mouse->emplace(virtual_display::WaylandMouse(wl_state));
          session->keyboard->emplace(virtual_display::WaylandKeyboard(wl_state));
          session->touch_screen->emplace(virtual_display::WaylandTouchScreen(wl_state));

          if (!wait_for_wayland_socket(runtime_dir, ready.wayland_socket_name)) {
            logs::log(logs::error,
                      "[STREAM_SESSION] Wayland socket {} was not ready, aborting runner startup",
                      ready.wayland_socket_name);
            session->event_bus->fire_event(immer::box<events::RunEnded>(
                events::RunEnded{.session_id = std::to_string(session->session_id), .run_scope = session->run_scope}));
            return;
          }

          logs::log(logs::debug, "[STREAM_SESSION] Start runner");
          session->event_bus->fire_event(immer::box<events::StartRunner>(
              events::StartRunner{.stop_stream_when_over = true,
                                  .runner = session->app->runner,
                                  .stream_session = std::make_shared<events::StreamSession>(*session)}));
        });
      }));

  /* Start runner */
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::StartRunner>>(
      [=](const immer::box<events::StartRunner> &run_session) {
        if (!run_session->stream_session || !run_session->stream_session->run_scope)
          return;
        auto session_id = std::to_string(run_session->stream_session->session_id);
        auto queues = plugged_devices_queue->load();
        auto entry = queues->find(session_id);
        if (!entry || !entry->scope.matches(run_session->stream_session->run_scope)) {
          logs::log(logs::warning, "No devices queue found for session {}", session_id);
          return;
        }

        auto devices_q = entry->queue; // Own it across worker launch and map replacement.
        std::thread([=]() {
          auto outcome = start_runner(
              run_session->runner,
              devices_q,
              immer::box<RunnerArgs>{RunnerArgs{
                  .session_id = session_id,
                  .video_settings =
                      {
                          .width = run_session->stream_session->display_mode.width,
                          .height = run_session->stream_session->display_mode.height,
                          .refresh_rate = run_session->stream_session->display_mode.refreshRate,
                          .wayland_render_node = run_session->stream_session->app->render_node,
                          .runner_render_node = run_session->stream_session->app->render_node,
                          .video_producer_buffer_caps = run_session->stream_session->app->video_producer_buffer_caps,
                      },
                  .wayland_display = run_session->stream_session->wayland_display->load(),
                  .audio_server = audio_server,
                  .audio_sink = run_session->stream_session->audio_sink->load(),
                  .host = app_state->host,
                  .app_local_state_folder = run_session->stream_session->app_local_state_folder,
                  .app_host_state_folder = run_session->stream_session->app_host_state_folder,
                  .xdg_runtime_dir = runtime_dir,
                  .client_settings = run_session->stream_session->client_settings,
                  .run_scope = run_session->stream_session->run_scope}});

          if (outcome.state == RunnerOutcome::State::unknown) {
            logs::log(logs::error, "[STREAM_SESSION] Runner {} is unknown; session retained", outcome.identity);
            return;
          }
          // Runner process ended
          if (run_session->stop_stream_when_over) {
            run_session->stream_session->wayland_display->store(nullptr);

            app_state->event_bus->fire_event(immer::box<events::RunEnded>(
                events::RunEnded{.session_id = session_id, .run_scope = run_session->stream_session->run_scope}));
          }
        }).detach();
      }));

  // The registry outlives all owned workers. Captures deliberately exclude
  // AppState/EventBus from work functions to avoid a bus -> runtime -> bus cycle.
  auto running = app_state->running_sessions;
  auto lobbies = app_state->lobbies;
  for (const auto &handler : streaming::consumer_event_handlers(app_state->event_bus, consumers))
    handlers.push_back(handler);

  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::VideoSession>>(
      [consumers, running, lobbies, gst_context = app_state->gst_context](
          const immer::box<events::VideoSession> &sess) {
        if (!state::prepare_stream_consumer(*sess, running->load(), lobbies->load()))
          return;
        auto admission = streaming::admit_consumer(
            *consumers,
            *sess,
            streaming::ConsumerKind::Video,
            [running, lobbies, sess, gst_context](streaming::ConsumerJob &job, const streaming::ConsumerPing &ping) {
              auto prepared = state::prepare_stream_consumer(*sess, running->load(), lobbies->load());
              if (!prepared || job.cancelled())
                return true;
              return streaming::start_streaming_video(
                  immer::box<events::VideoSession>{*prepared},
                  job,
                  ping.ip,
                  ping.port,
                  gst_context,
                  std::static_pointer_cast<boost::asio::ip::udp::socket>(ping.socket));
            });
        if (admission != streaming::ConsumerAdmission::Accepted && admission != streaming::ConsumerAdmission::Duplicate)
          logs::log(logs::warning, "[CONSUMER] Video admission denied: {}", static_cast<int>(admission));
      }));
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::AudioSession>>(
      [consumers, running, lobbies, audio_server](const immer::box<events::AudioSession> &sess) {
        if (!state::prepare_stream_consumer(*sess, running->load(), lobbies->load()))
          return;
        auto admission = streaming::admit_consumer(
            *consumers,
            *sess,
            streaming::ConsumerKind::Audio,
            [running, lobbies, sess, audio_server](streaming::ConsumerJob &job, const streaming::ConsumerPing &ping) {
              auto prepared = state::prepare_stream_consumer(*sess, running->load(), lobbies->load());
              if (!prepared || job.cancelled())
                return true;
              auto server = audio_server ? audio::get_server_name(audio_server->server) : std::optional<std::string>();
              auto sink = fmt::format("{}{}.monitor", VIRTUAL_SINK_PREFIX, sess->session_id);
              return streaming::start_streaming_audio(
                  immer::box<events::AudioSession>{*prepared},
                  job,
                  ping.ip,
                  ping.port,
                  std::static_pointer_cast<boost::asio::ip::udp::socket>(ping.socket),
                  sink,
                  server.value_or(""));
            });
        if (admission != streaming::ConsumerAdmission::Accepted && admission != streaming::ConsumerAdmission::Duplicate)
          logs::log(logs::warning, "[CONSUMER] Audio admission denied: {}", static_cast<int>(admission));
      }));

  return handlers.persistent();
}

} // namespace wolf::core::sessions
