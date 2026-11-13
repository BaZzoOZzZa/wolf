#include <immer/vector_transient.hpp>
#include <rfl/json.hpp>
#include <sessions/common.hpp>
#include <sessions/diagnostic_startup.hpp>
#include <sessions/handlers.hpp>
#include <sessions/lifecycle.hpp>
#include <state/config.hpp>
#include <state/data-structures.hpp>
#include <state/sessions.hpp>
#include <streaming/streaming.hpp>

namespace wolf::core::sessions {

/**
 * @brief Removes the StreamSession from the input Lobby and switches everything to the original session
 *
 * @note Leaving a lobby may have side effects,
 * like terminating the lobby if it becomes empty or triggering additional events.
 */
void leave_lobby(const std::shared_ptr<events::EventBusType> &ev_bus,
                 const events::Lobby &lobby,
                 const events::StreamSession &session,
                 const std::function<void()> &stop_when_empty) {
  logs::log(logs::info, "[LOBBY] Session {} leaving lobby {}", session.session_id, lobby.id);
  // Remove the current session from the lobby list
  lobby.connected_sessions->update([session](const immer::vector<immer::box<std::string>> &connected_sessions) {
    return connected_sessions | //
           ranges::views::filter([session](const immer::box<std::string> &session_id) {
             return *session_id != std::to_string(session.session_id);
           }) | //
           ranges::to<immer::vector<immer::box<std::string>>>();
  });

  // Switch over mouse and keyboard to use the original session wayland server
  auto wl_state = session.wayland_display->load();
  session.mouse->emplace(virtual_display::WaylandMouse(wl_state));
  session.keyboard->emplace(virtual_display::WaylandKeyboard(wl_state));
  session.touch_screen->emplace(virtual_display::WaylandTouchScreen(wl_state));

  // Switch over all joypads present in the lobby back into the original session
  events::JoypadList joypads = session.joypads->load();
  for (auto [_joypad_nr, joypad] : joypads) {
    // Plug them into original session
    events::PlugDeviceEvent plug_ev{.session_id = std::to_string(session.session_id),
                                    .run_scope = session.run_scope,
                                    .connection_scope = session.connection_scope};
    std::visit(
        [&plug_ev](auto &pad) {
          plug_ev.udev_events = pad.get_udev_events();
          plug_ev.udev_hw_db_entries = pad.get_udev_hw_db_entries();
        },
        *joypad);
    ev_bus->fire_event(immer::box<events::PlugDeviceEvent>(plug_ev));
    // Unplug them from the current lobby
    ev_bus->fire_event(immer::box<events::UnplugDeviceEvent>{
        events::UnplugDeviceEvent{.session_id = lobby.id,
                                  .udev_events = plug_ev.udev_events,
                                  .udev_hw_db_entries = plug_ev.udev_hw_db_entries,
                                  .run_scope = lobby.run_scope,
                                  .connection_scope = session.connection_scope}});
  }
  // TODO: hotplug pen_tablet and touch_screen

  // Switch audio/video gstreamer stream producers
  ev_bus->fire_event(immer::box<events::SwitchStreamProducerEvents>{
      events::SwitchStreamProducerEvents{.session_id = session.session_id,
                                         .interpipe_src_id = std::to_string(session.session_id),
                                         .connection_scope = session.connection_scope,
                                         .producer_scope = session.run_scope}});

  if (lobby.stop_when_everyone_leaves && lobby.connected_sessions->load()->size() == 0) {
    // Nobody left in the lobby, and it's set to stop when everyone leaves
    stop_when_empty();
  }
}

immer::vector<immer::box<events::EventBusHandlers>>
setup_lobbies_handlers(const immer::box<state::AppState> &app_state,
                       const std::string &runtime_dir,
                       const std::optional<AudioServer> &audio_server) {
  immer::vector_transient<immer::box<events::EventBusHandlers>> handlers;
  auto lifecycle = std::make_shared<LobbyLifecycle>();
  auto settle_setup = [](const auto &settings, bool ready) {
    try {
      settings->on_setup_over.get()->set_value(ready);
    } catch (const std::future_error &) {
      // Stop and asynchronous setup completion can both resolve the same request.
    }
  };
  auto settle_join = [](const auto &request, std::string message) {
    try {
      request->error_message.get()->set_value(std::move(message));
    } catch (const std::future_error &) {
      // A repeated event or a failure after completion must not resolve twice.
    }
  };
  auto cleanup_lobby = [=](const RunScope &scope) {
    const auto &id = scope.id();
    auto ticket = lifecycle->find(id);
    if (!ticket || !ticket->run_scope.matches(scope)) {
      return;
    }
    handle_lobby_stop(
        *lifecycle,
        ticket,
        [&] {
          auto lobbies = app_state->lobbies->load();
          auto lobby = state::get_lobby_by_id(lobbies.get(), id);
          if (!lobby || !lobby->run_scope.matches(scope)) {
            return lifecycle->setup_complete(ticket);
          }
          auto connected = lobby->members->load();
          for (const auto &[session_id, member] : *connected) {
            auto current = state::get_session_by_run(app_state->running_sessions->load(), member->run_scope);
            const auto &session = current ? *current : *member;
            app_state->event_bus->fire_event(immer::box<events::LeaveLobbyEvent>{
                events::LeaveLobbyEvent{.lobby_id = id,
                                        .moonlight_session_id = session_id,
                                        .lobby_scope = scope,
                                        .connection_scope = session.connection_scope}});
          }
          return lobby->connected_sessions->load()->empty();
        },
        [&] {
          // Only confirmed terminal workers and completed membership cleanup can
          // remove this addressable record. Unknown/failed stop keeps the ID/PIN.
          auto lobbies = app_state->lobbies->load();
          if (auto lobby = state::get_lobby_by_id(lobbies.get(), id); lobby && lobby->run_scope.matches(scope)) {
            if (audio_server && audio_server->server) {
              std::shared_ptr<audio::VSink> sink = lobby->audio_sink->load();
              if (sink) {
                lobby->audio_sink->store(nullptr);
                audio::delete_virtual_sink(audio_server->server, sink);
              }
            }
          }
          app_state->lobbies->update([scope](const immer::vector<events::Lobby> &lobbies) {
            return lobbies |
                   ranges::views::filter([&](const events::Lobby &lobby) { return !lobby.run_scope.matches(scope); }) |
                   ranges::to<immer::vector<events::Lobby>>();
          });
        });
  };
  auto signal_stop = [lifecycle, cleanup_lobby, ev_bus = app_state->event_bus](const auto &ticket) {
    notify_lobby_completion(
        *lifecycle,
        ticket,
        [&] {
          ev_bus->fire_event(immer::box<events::StopLobbyEvent>{
              events::StopLobbyEvent{.lobby_id = ticket->id, .run_scope = ticket->run_scope}});
        },
        [&] { cleanup_lobby(ticket->run_scope); });
  };
  auto dispatch_deferred = [cleanup_lobby, ev_bus = app_state->event_bus](const auto &ticket, const auto &deferred) {
    for (const auto &scope : deferred.leaves) {
      ev_bus->fire_event(immer::box<events::LeaveLobbyEvent>{events::LeaveLobbyEvent{.lobby_id = ticket->id,
                                                                                     .moonlight_session_id = scope.id(),
                                                                                     .lobby_scope = ticket->run_scope,
                                                                                     .connection_scope = scope}});
    }
    if (deferred.stop) {
      cleanup_lobby(ticket->run_scope);
    }
  };

  // On create lobby event
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::CreateLobbyEvent>>(
      [=](const immer::box<events::CreateLobbyEvent> &lobby_settings) {
        auto ev_bus = app_state->event_bus;
        std::string resource;
        std::string fingerprint;
        try {
          // All lobbies sharing the canonical runner state directory compete for
          // one launch slot, including legacy requests without an idempotency key.
          resource = std::filesystem::weakly_canonical(std::filesystem::path(app_state->host->host_base_state_folder) /
                                                       lobby_settings->runner_state_folder)
                         .string();
          fingerprint = rfl::json::write(lobby_settings->runner->serialize()) + "\n" +
                        rfl::json::write(lobby_settings->profile_id) + "\n" + rfl::json::write(lobby_settings->name) +
                        "\n" + rfl::json::write(lobby_settings->icon_png_path) + "\n" +
                        rfl::json::write(lobby_settings->pin) + "\n" + rfl::json::write(lobby_settings->multi_user) +
                        "\n" + rfl::json::write(lobby_settings->stop_when_everyone_leaves) + "\n" +
                        rfl::json::write(lobby_settings->video_settings) + "\n" +
                        rfl::json::write(lobby_settings->audio_settings) + "\n" +
                        rfl::json::write(lobby_settings->client_settings);
        } catch (const std::exception &e) {
          logs::log(logs::error, "[LOBBY] Invalid launch resource: {}", e.what());
          settle_setup(lobby_settings, false);
          return;
        }
        auto admission = lifecycle->create(lobby_settings->id, resource, fingerprint);
        if (admission.result != LobbyLifecycle::Create::admitted) {
          settle_setup(lobby_settings, admission.result == LobbyLifecycle::Create::replay_ready);
          return;
        }
        auto ticket = admission.ticket;
        logs::log(logs::info, "[LOBBY] Creating new lobby {}", ticket->id);

        try {
          run_lobby_startup(
              *lifecycle,
              ticket,
              [&](auto &handoffs) {
                auto lobby = std::make_shared<events::Lobby>(
                    events::Lobby{.id = lobby_settings->id,
                                  .name = lobby_settings->name,
                                  .started_by_profile_id = lobby_settings->profile_id,
                                  .icon_png_path = lobby_settings->icon_png_path,
                                  .multi_user = lobby_settings->multi_user,
                                  .pin = lobby_settings->pin,
                                  .stop_when_everyone_leaves = lobby_settings->stop_when_everyone_leaves,
                                  .runner = lobby_settings->runner,
                                  .run_scope = ticket->run_scope});
                app_state->lobbies->update(
                    [lobby](const immer::vector<events::Lobby> &lobbies) { return lobbies.push_back(*lobby); });
                if (lifecycle->stopping(ticket)) {
                  settle_setup(lobby_settings, false);
                  signal_stop(ticket);
                  return;
                }

                { // Create audio virtual sink
                  logs::log(logs::debug, "[LOBBY] Create audio virtual sink");
                  auto pulse_sink_name = fmt::format("{}{}", VIRTUAL_SINK_PREFIX, lobby->id);
                  if (audio_server && audio_server->server) {
                    auto channel_count = lobby_settings->audio_settings.channel_count;
                    handoffs.audio(
                        [&](auto work) {
                          auto v_device = audio::create_virtual_sink(
                              audio_server->server,
                              audio::AudioDevice{.sink_name = pulse_sink_name,
                                                 .mode = state::get_audio_mode(channel_count, true)});
                          lobby->audio_sink->store(v_device);
                          return std::thread(std::move(work));
                        },
                        [=, server = audio_server->server] {
                          try {
                            if (!lifecycle->stopping(ticket)) {
                              auto sink_name = fmt::format("{}{}.monitor", VIRTUAL_SINK_PREFIX, lobby->id);
                              streaming::start_audio_producer(lobby->run_scope,
                                                              ev_bus,
                                                              channel_count,
                                                              sink_name,
                                                              audio::get_server_name(server));
                            }
                          } catch (const std::exception &e) {
                            logs::log(logs::error, "[LOBBY] Audio producer failed: {}", e.what());
                          }
                        },
                        [=] { signal_stop(ticket); });
                  }
                }
                { // Start Wayland compositor and Gstreamer producer pipeline
                  logs::log(logs::debug, "[LOBBY] Create wayland compositor");

                  std::shared_ptr<boost::promise<streaming::WaylandDisplayReady>> on_ready =
                      std::make_shared<boost::promise<streaming::WaylandDisplayReady>>();

                  if (!handoffs.video([](auto work) { return std::thread(std::move(work)); },
                                      [=, gst_context = app_state->gst_context] {
                                        try {
                                          if (!lifecycle->stopping(ticket)) {
                                            streaming::start_video_producer(
                                                lobby->run_scope,
                                                lobby_settings->video_settings.video_producer_buffer_caps,
                                                lobby_settings->video_settings.wayland_render_node,
                                                {.width = lobby_settings->video_settings.width,
                                                 .height = lobby_settings->video_settings.height,
                                                 .refreshRate = lobby_settings->video_settings.refresh_rate},
                                                gst_context,
                                                on_ready,
                                                ev_bus);
                                          }
                                        } catch (const std::exception &e) {
                                          logs::log(logs::error, "[LOBBY] Video producer failed: {}", e.what());
                                        }
                                        try {
                                          on_ready->set_exception(
                                              boost::copy_exception(std::runtime_error("Video producer ended")));
                                        } catch (const boost::promise_already_satisfied &) {
                                        }
                                      },
                                      [=] {
                                        settle_setup(lobby_settings, false);
                                        signal_stop(ticket);
                                      })) {
                    settle_setup(lobby_settings, false);
                    signal_stop(ticket);
                    return;
                  }

                  auto w_display_ready = on_ready->get_future().then([=, host = app_state->host](auto fut) {
                    try {
                      streaming::WaylandDisplayReady ready = fut.get();
                      if (lifecycle->stopping(ticket)) {
                        settle_setup(lobby_settings, false);
                        // The first stop can predate the producer's EOS handler.
                        gst_element_send_event(ready.wayland_plugin.get(), gst_event_new_eos());
                        return;
                      }

                      auto wl_state =
                          virtual_display::create_wayland_display(ready.wayland_plugin, ready.wayland_socket_name);
                      // Set the wayland display
                      lobby->wayland_display->store(wl_state);

                      if (!wait_for_wayland_socket(runtime_dir, ready.wayland_socket_name)) {
                        logs::log(logs::error,
                                  "[LOBBY] Wayland socket {} was not ready, aborting runner startup",
                                  ready.wayland_socket_name);
                        signal_stop(ticket);
                        return;
                      }

                      { // Start runner
                        logs::log(logs::debug, "[LOBBY] Start runner");
                        auto full_path = std::filesystem::path(host->local_base_state_folder) /
                                         lobby_settings->runner_state_folder;
                        logs::log(logs::debug, "Host app state folder: {}, creating paths", full_path.string());
                        std::filesystem::create_directories(full_path);

                        std::thread([=]() {
                          if (!lifecycle->launch(ticket)) {
                            settle_setup(lobby_settings, false);
                            signal_stop(ticket);
                            return;
                          }
                          if (!lifecycle->ready(ticket)) {
                            lifecycle->runner_finished(ticket); // Cancelled before invoking the runner.
                            settle_setup(lobby_settings, false);
                            signal_stop(ticket);
                            return;
                          }
                          settle_setup(lobby_settings, true);
                          try {
                            auto outcome = start_runner(
                                lobby->runner,
                                lobby->plugged_devices_queue,
                                immer::box<RunnerArgs>{RunnerArgs{
                                    .session_id = lobby->id,
                                    .video_settings = lobby_settings->video_settings,
                                    .wayland_display = lobby->wayland_display->load(),
                                    .audio_server = audio_server,
                                    .audio_sink = lobby->audio_sink->load(),
                                    .host = host,
                                    .app_local_state_folder = full_path.string(),
                                    .app_host_state_folder = std::filesystem::path(host->host_base_state_folder) /
                                                             lobby_settings->runner_state_folder,
                                    .xdg_runtime_dir = runtime_dir,
                                    .client_settings = lobby_settings->client_settings,
                                    .cleanup_audio_sink = false,
                                    .run_scope = lobby->run_scope}});
                            if (outcome.state == RunnerOutcome::State::unknown) {
                              lifecycle->quarantine(ticket);
                              logs::log(logs::error,
                                        "[LOBBY] Runner {} unknown; lobby {} resource retained",
                                        outcome.identity,
                                        lobby->id);
                              return;
                            }
                          } catch (const std::exception &e) {
                            // Unknown runner outcome retains the resource. There is no
                            // safe automatic retry after arbitrary startup side effects.
                            logs::log(logs::error, "[LOBBY] Runner outcome unknown, resource retained: {}", e.what());
                            lifecycle->quarantine(ticket);
                            return;
                          }
                          lifecycle->runner_finished(ticket);
                          // Runner process ended, stop the lobby
                          lobby->wayland_display->store(nullptr);

                          signal_stop(ticket);
                        }).detach();
                      }

                    } catch (const std::exception &e) {
                      logs::log(logs::error, "[LOBBY] Setup failed: {}", e.what());
                      settle_setup(lobby_settings, false);
                      signal_stop(ticket);
                    }
                  });
                }
              },
              [=] {
                if (lifecycle->stopping(ticket)) {
                  cleanup_lobby(ticket->run_scope);
                }
              });
        } catch (const std::exception &e) {
          logs::log(logs::error, "[LOBBY] Setup failed; unstarted workers cancelled: {}", e.what());
          settle_setup(lobby_settings, false);
          signal_stop(ticket);
        }
      }));

  // When a Moonlight client joins a lobby
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::JoinLobbyEvent>>(
      [=](const immer::box<events::JoinLobbyEvent> &join_lobby_event) {
        auto lobbies = app_state->lobbies->load();
        auto lobby = state::get_lobby_by_id(lobbies.get(), join_lobby_event->lobby_id);
        auto sessions = app_state->running_sessions->load();
        auto session = state::get_session_by_scope(sessions.get(), join_lobby_event->connection_scope.get());

        if (!lobby || !session || !lobby->run_scope.matches(join_lobby_event->lobby_scope.get()) ||
            session->session_id != join_lobby_event->moonlight_session_id) {
          logs::log(logs::error,
                    "[LOBBY] Failed to join lobby: lobby {} or session {} not found",
                    join_lobby_event->lobby_id,
                    join_lobby_event->moonlight_session_id);
          settle_join(join_lobby_event, "Lobby or session not found");
          return;
        }
        if (diagnostic::isolated(*session) || diagnostic::isolated_target(*lobby, *sessions)) {
          settle_join(join_lobby_event,
                      diagnostic::disabled(*session) ? "Diagnostic app cannot join lobbies"
                                                     : "Isolated app cannot join lobbies");
          return;
        }
        logs::log(logs::info, "[LOBBY] Session {} joining lobby {}", session->session_id, lobby->id);

        auto ticket = lifecycle->find(lobby->id);
        if (!ticket || lobby->pin != join_lobby_event->pin) {
          settle_join(join_lobby_event, "Lobby unavailable or invalid PIN");
          return;
        }
        auto admission = lifecycle->join(ticket, session->session_id, lobby->multi_user);
        if (admission == LobbyLifecycle::Join::already_joined) {
          settle_join(join_lobby_event, "");
          return;
        }
        if (admission != LobbyLifecycle::Join::admitted) {
          settle_join(join_lobby_event, "Lobby full, stopping, or session busy; retry later");
          return;
        }
        try {

          // Migrate joypads BEFORE adding the session to connected_sessions, or the relayed unplug races the queued
          // plug
          events::JoypadList joypads = session->joypads->load();
          for (auto [_joypad_nr, joypad] : joypads) {
            events::PlugDeviceEvent plug_ev{.session_id = lobby->id,
                                            .run_scope = lobby->run_scope,
                                            .connection_scope = session->connection_scope};
            std::visit(
                [&plug_ev](auto &pad) {
                  plug_ev.udev_events = pad.get_udev_events();
                  plug_ev.udev_hw_db_entries = pad.get_udev_hw_db_entries();
                },
                *joypad);
            // Unplug it from the current session's runner
            app_state->event_bus->fire_event(immer::box<events::UnplugDeviceEvent>{
                events::UnplugDeviceEvent{.session_id = std::to_string(session->session_id),
                                          .udev_events = plug_ev.udev_events,
                                          .udev_hw_db_entries = plug_ev.udev_hw_db_entries,
                                          .run_scope = session->run_scope,
                                          .connection_scope = session->connection_scope}});

            // Add it to the lobby runner's devices queue, addressed to the lobby id
            // (same routing the PlugDeviceEvent relay performs for connected sessions)
            lobby->plugged_devices_queue->push(immer::box<events::PlugDeviceEvent>{plug_ev});
          }
          // TODO: hotplug pen_tablet

          lobby->members->update([session](const auto &members) {
            return members.set(session->session_id, std::make_shared<events::StreamSession>(*session));
          });
          // Update the lobby with the new session
          lobby->connected_sessions->update(
              [session](const immer::vector<immer::box<std::string>> &connected_sessions) {
                return connected_sessions.push_back({std::to_string(session->session_id)});
              });

          // switch mouse and keyboard in session to use the lobby wayland server
          auto wl_state = lobby->wayland_display->load();
          session->mouse->emplace(virtual_display::WaylandMouse(wl_state));
          session->keyboard->emplace(virtual_display::WaylandKeyboard(wl_state));
          session->touch_screen->emplace(virtual_display::WaylandTouchScreen(wl_state));

          // Switch audio/video gstreamer stream producers
          app_state->event_bus->fire_event(immer::box<events::SwitchStreamProducerEvents>{
              events::SwitchStreamProducerEvents{.session_id = session->session_id,
                                                 .interpipe_src_id = lobby->id,
                                                 .connection_scope = session->connection_scope,
                                                 .producer_scope = lobby->run_scope}});
          settle_join(join_lobby_event, "");
          dispatch_deferred(ticket, lifecycle->finish(ticket));
        } catch (const std::exception &e) {
          // Keep the admission reserved after partially applied device changes.
          // Full input rollback/neutralisation belongs to the handoff work.
          lifecycle->quarantine(ticket);
          logs::log(logs::error, "[LOBBY] Join outcome unknown, admission retained: {}", e.what());
          settle_join(join_lobby_event, "Join failed; operator reconciliation required");
        }
      }));

  // When a Moonlight session leaves the lobby
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::LeaveLobbyEvent>>(
      [=](const immer::box<events::LeaveLobbyEvent> &leave_lobby_event) {
        auto lobbies = app_state->lobbies->load();
        auto lobby = state::get_lobby_by_id(lobbies.get(), leave_lobby_event->lobby_id);
        auto sessions = app_state->running_sessions->load();
        auto session = state::get_session_by_scope(sessions.get(), leave_lobby_event->connection_scope.get());
        // Run cleanup may follow removal from running_sessions. Retain the exact
        // member snapshot; never redirect a late leave to a replacement session.
        if (!session && lobby && !state::get_session_by_id(sessions.get(), leave_lobby_event->moonlight_session_id)) {
          auto members = lobby->members->load();
          if (auto member = members->find(leave_lobby_event->moonlight_session_id);
              member && (*member)->connection_scope.matches(leave_lobby_event->connection_scope.get()))
            session = **member;
        }

        if (!lobby || !session || !lobby->run_scope.matches(leave_lobby_event->lobby_scope.get()) ||
            session->session_id != leave_lobby_event->moonlight_session_id) {
          logs::log(logs::error,
                    "[LOBBY] Failed to leave lobby: lobby {} or session {} not found",
                    leave_lobby_event->lobby_id,
                    leave_lobby_event->moonlight_session_id);
        } else {
          auto members = lobby->members->load();
          auto member = members->find(session->session_id);
          if (!member || !(*member)->run_scope.matches(session->run_scope))
            return;
          auto ticket = lifecycle->find(lobby->id);
          if (!ticket || !lifecycle->leave(ticket, session->connection_scope)) {
            return;
          }
          try {
            leave_lobby(app_state->event_bus, lobby.value(), session.value(), [=] { signal_stop(ticket); });
            lobby->members->update([session](const auto &members) { return members.erase(session->session_id); });
            dispatch_deferred(ticket, lifecycle->finish(ticket, session->session_id));
          } catch (const std::exception &e) {
            lifecycle->quarantine(ticket);
            logs::log(logs::error, "[LOBBY] Leave outcome unknown, admission retained: {}", e.what());
          }
        }
      }));

  // Membership cleanup does not mean the runner stopped. Keep the record until
  // terminal completion, so the public stop API can address an explicit retry.
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::StopLobbyEvent>>(
      [=](const immer::box<events::StopLobbyEvent> &event) {
        if (event->run_scope.get().id() == event->lobby_id)
          cleanup_lobby(event->run_scope.get());
      }));

  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::DockerContainerCreated>>(
      [=](const immer::box<events::DockerContainerCreated> &created) {
        auto ticket = lifecycle->find(created->session_id);
        if (ticket && ticket->run_scope.matches(created->run_scope.get()) && lifecycle->stopping(ticket)) {
          app_state->event_bus->fire_event(immer::box<events::StopLobbyEvent>{
              events::StopLobbyEvent{.lobby_id = ticket->id, .run_scope = ticket->run_scope}});
        }
      }));

  // On a PlugDeviceEvent, we have to add the device to the lobby queue so that the runner will pick it up
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::PlugDeviceEvent>>(
      [=](const immer::box<events::PlugDeviceEvent> &plug_device_event) {
        auto source =
            state::get_session_by_scope(app_state->running_sessions->load(), plug_device_event->connection_scope.get());
        if (!source ||
            !sessions::targets(source->run_scope, plug_device_event->run_scope.get(), plug_device_event->session_id))
          return;
        if (diagnostic::isolated(*source))
          return;
        immer::vector<events::Lobby> lobbies = app_state->lobbies->load();
        if (auto lobby = state::get_lobby_for_session(lobbies, *source)) {
          if (diagnostic::isolated_target(*lobby, *app_state->running_sessions->load()))
            return;
          logs::log(logs::info,
                    "[LOBBY] adding device to session {} in lobby {}",
                    plug_device_event->session_id,
                    lobby->id);

          lobby->plugged_devices_queue->push(immer::box<events::PlugDeviceEvent>{
              events::PlugDeviceEvent{.session_id = lobby->id,
                                      .udev_events = plug_device_event->udev_events,
                                      .udev_hw_db_entries = plug_device_event->udev_hw_db_entries,
                                      .run_scope = lobby->run_scope,
                                      .connection_scope = plug_device_event->connection_scope}});
        }
      }));

  // When a device is unplugged from a Moonlight session, we have to re-fire the event on our lobby
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::UnplugDeviceEvent>>(
      [=](const immer::box<events::UnplugDeviceEvent> &unplug_device_event) {
        auto source = state::get_session_by_scope(app_state->running_sessions->load(),
                                                  unplug_device_event->connection_scope.get());
        if (!source || !sessions::targets(source->run_scope,
                                          unplug_device_event->run_scope.get(),
                                          unplug_device_event->session_id))
          return;
        if (diagnostic::isolated(*source))
          return;
        immer::vector<events::Lobby> lobbies = app_state->lobbies->load();
        if (auto lobby = state::get_lobby_for_session(lobbies, *source)) {
          if (diagnostic::isolated_target(*lobby, *app_state->running_sessions->load()))
            return;
          logs::log(logs::debug, "[LOBBY] Unplug device for session {}", unplug_device_event->session_id);
          app_state->event_bus->fire_event(immer::box<events::UnplugDeviceEvent>{
              events::UnplugDeviceEvent{.session_id = lobby->id,
                                        .udev_events = unplug_device_event->udev_events,
                                        .udev_hw_db_entries = unplug_device_event->udev_hw_db_entries,
                                        .run_scope = lobby->run_scope,
                                        .connection_scope = unplug_device_event->connection_scope}});
        }
      }));

  auto on_explicit_leave = [app_state](const ConnectionScope &scope) {
    auto session = state::get_session_by_scope(app_state->running_sessions->load(), scope);
    if (!session)
      return;
    auto lobbies = app_state->lobbies->load();
    if (auto lobby = state::get_lobby_for_session(*lobbies, *session)) {
      app_state->event_bus->fire_event(
          immer::box<events::LeaveLobbyEvent>{events::LeaveLobbyEvent{.lobby_id = lobby->id,
                                                                      .moonlight_session_id = scope.id(),
                                                                      .lobby_scope = lobby->run_scope,
                                                                      .connection_scope = scope}});
    }
  };
  // Network pause/stop ends only its consumers. Membership/device migration is
  // an explicit leave operation; input revocation/neutral is a later stage.

  auto on_run_over = [app_state](const RunScope &run) {
    if (!run)
      return;
    auto lobbies = app_state->lobbies->load();
    for (const auto &lobby : *lobbies) {
      auto members = lobby.members->load();
      for (const auto &[id, member] : *members) {
        if (!member->run_scope.matches(run))
          continue;
        auto current = state::get_session_by_run(app_state->running_sessions->load(), run);
        const auto &session = current ? *current : *member;
        app_state->event_bus->fire_event(
            immer::box<events::LeaveLobbyEvent>{events::LeaveLobbyEvent{.lobby_id = lobby.id,
                                                                        .moonlight_session_id = id,
                                                                        .lobby_scope = lobby.run_scope,
                                                                        .connection_scope = session.connection_scope}});
      }
    }
  };
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::GameStopRequest>>(
      [on_run_over](const immer::box<events::GameStopRequest> &ev) {
        if (ev->run_scope.get().id() == ev->session_id)
          on_run_over(ev->run_scope.get());
      }));
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::RunEnded>>(
      [on_run_over](const immer::box<events::RunEnded> &ev) {
        if (ev->run_scope.get().id() == ev->session_id)
          on_run_over(ev->run_scope.get());
      }));

  // When a client presses the WolfUI special combo, we get the user out of the lobby (and back to WolfUI)
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::ClientWolfUIComboEvent>>(
      [=](const immer::box<events::ClientWolfUIComboEvent> &event) {
        logs::log(logs::info, "Detected WolfUI combo for session {}", event->session_id);
        if (event->connection_scope.get().id() == event->session_id)
          on_explicit_leave(event->connection_scope.get());
      }));

  return handlers.persistent();
}

} // namespace wolf::core::sessions
