#include <catch2/catch_test_macros.hpp>
#include <events/reflectors.hpp>
#include <sessions/handlers.hpp>
#include <sessions/lifecycle.hpp>
#include <state/sessions.hpp>

using namespace wolf::core;

namespace {
events::StreamSession scoped_session(std::size_t id) {
  return {.session_id = id,
          .connection_scope = sessions::ConnectionScope::fresh(id),
          .run_scope = sessions::RunScope::fresh(std::to_string(id))};
}

immer::box<state::AppState> isolated_state() {
  return state::AppState{.event_bus = std::make_shared<events::EventBusType>(),
                         .lobbies = std::make_shared<immer::atom<immer::vector<events::Lobby>>>(),
                         .running_sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>()};
}
} // namespace

TEST_CASE("Connection scopes retain identity through ABA and reject null", "[scope][isolated]") {
  const auto retained = sessions::ConnectionScope::fresh(42);
  auto copy = retained;
  REQUIRE(copy.matches(retained));
  REQUIRE_FALSE(sessions::ConnectionScope{}.matches({}));
  REQUIRE_FALSE(sessions::targets(retained, {}, std::size_t{42}));
  REQUIRE_FALSE(sessions::targets(retained, retained, std::size_t{43}));
  for (int i = 0; i < 4096; ++i) {
    auto replacement = sessions::ConnectionScope::fresh(42);
    REQUIRE_FALSE(replacement.matches(retained));
    REQUIRE(replacement.resource_key() != retained.resource_key());
  }
}

TEST_CASE("Scoped session removal never erases a replacement with the same stable ID", "[scope][isolated]") {
  auto old = scoped_session(42);
  auto replacement = scoped_session(42);
  auto snapshot = immer::vector<events::StreamSession>{replacement};
  REQUIRE(state::remove_session(snapshot, old).size() == 1);
  REQUIRE_FALSE(state::get_session_by_scope(snapshot, old.connection_scope));
  REQUIRE(state::get_session_by_scope(snapshot, replacement.connection_scope));
  REQUIRE(state::get_session_by_scope(immer::vector<events::StreamSession>{old, replacement},
                                      replacement.connection_scope));
  REQUIRE(state::remove_session(snapshot, replacement).empty());
  REQUIRE(state::remove_session(snapshot, events::StreamSession{.session_id = 42}).size() == 1);
}

TEST_CASE("Production run lifecycle adapters distinguish stop request and exact completion", "[scope][isolated]") {
  auto app = isolated_state();
  auto old = scoped_session(42);
  auto replacement = scoped_session(42);
  app->running_sessions->store(immer::vector<events::StreamSession>{replacement});
  // Real registrations; no StreamSession/start/ping events, device or GStreamer startup.
  auto handlers = sessions::setup_moonlight_handlers(app, "", {});
  std::vector<sessions::ConnectionScope> stopped;
  auto observer = app->event_bus->register_handler<immer::box<events::StopStreamEvent>>(
      [&](const immer::box<events::StopStreamEvent> &ev) { stopped.push_back(ev->connection_scope.get()); });
  auto fire = [&](auto event) { app->event_bus->fire_event(immer::box<decltype(event)>{event}); };

  fire(events::GameStopRequest{.session_id = "42"});
  fire(events::RunEnded{.session_id = "42"});
  fire(events::GameStopRequest{.session_id = "42", .run_scope = old.run_scope});
  fire(events::RunEnded{.session_id = "42", .run_scope = old.run_scope});
  REQUIRE(stopped.empty());
  REQUIRE(app->running_sessions->load()->size() == 1);

  fire(events::StopStreamEvent{.session_id = 42, .connection_scope = replacement.connection_scope});
  REQUIRE(app->running_sessions->load()->size() == 1); // Connection stop does not destroy the game.
  stopped.clear();
  auto resumed = replacement;
  resumed.connection_scope = sessions::ConnectionScope::fresh(42);
  app->running_sessions->store(immer::vector<events::StreamSession>{resumed});
  fire(events::GameStopRequest{.session_id = "42", .run_scope = replacement.run_scope});
  REQUIRE(stopped.size() == 1);
  REQUIRE(stopped.back().matches(resumed.connection_scope));
  REQUIRE(app->running_sessions->load()->size() == 1); // Accepted != completed.
  fire(events::RunEnded{.session_id = "42", .run_scope = replacement.run_scope});
  REQUIRE(app->running_sessions->load()->empty());
}

TEST_CASE("Production lobby adapters reject stale join and preserve membership on connection loss",
          "[scope][isolated]") {
  auto app = isolated_state();
  auto current = scoped_session(42);
  app->running_sessions->store(immer::vector<events::StreamSession>{current});
  events::Lobby lobby{.id = "lobby",
                      .name = "test",
                      .started_by_profile_id = "test",
                      .multi_user = false,
                      .stop_when_everyone_leaves = true,
                      .run_scope = sessions::RunScope::fresh("lobby")};
  lobby.connected_sessions->store(immer::vector<immer::box<std::string>>{immer::box<std::string>{"42"}});
  lobby.members->store(immer::map<std::size_t, std::shared_ptr<events::StreamSession>>{}.set(
      42,
      std::make_shared<events::StreamSession>(current)));
  app->lobbies->store(immer::vector<events::Lobby>{lobby});
  auto handlers = sessions::setup_lobbies_handlers(app, "", {});
  int leaves = 0;
  auto observer = app->event_bus->register_handler<immer::box<events::LeaveLobbyEvent>>(
      [&](const immer::box<events::LeaveLobbyEvent> &) { ++leaves; });
  app->event_bus->fire_event(immer::box<events::PauseStreamEvent>{
      events::PauseStreamEvent{.session_id = 42, .connection_scope = current.connection_scope}});
  app->event_bus->fire_event(immer::box<events::StopStreamEvent>{
      events::StopStreamEvent{.session_id = 42, .connection_scope = current.connection_scope}});
  REQUIRE(leaves == 0);
  REQUIRE(lobby.connected_sessions->load()->size() == 1);
  auto old = sessions::ConnectionScope::fresh(42);
  events::JoinLobbyEvent join{.lobby_id = "lobby",
                              .moonlight_session_id = 42,
                              .lobby_scope = lobby.run_scope,
                              .connection_scope = old};
  auto result = join.error_message.get()->get_future();
  app->event_bus->fire_event(immer::box<events::JoinLobbyEvent>{join});
  REQUIRE_FALSE(result.get().empty());
  app->event_bus->fire_event(immer::box<events::LeaveLobbyEvent>{events::LeaveLobbyEvent{.lobby_id = "lobby",
                                                                                         .moonlight_session_id = 42,
                                                                                         .lobby_scope = lobby.run_scope,
                                                                                         .connection_scope = old}});
  REQUIRE(lobby.connected_sessions->load()->size() == 1);
}

TEST_CASE("Public events serialize no runtime authority and cannot round trip it", "[scope][isolated]") {
  auto scope = sessions::ConnectionScope::fresh(42);
  const auto json = rfl::json::write(events::IDRRequestEvent{.session_id = 42, .connection_scope = scope});
  REQUIRE(json == R"({"session_id":"42"})");
  auto parsed = rfl::json::read<events::IDRRequestEvent>(json);
  REQUIRE(parsed);
  REQUIRE_FALSE(parsed->connection_scope.get());
  auto injected = rfl::json::read<events::IDRRequestEvent>(R"({"session_id":"42","connection_scope":42})");
  // Depending on reflection policy, unknown fields may be ignored or rejected;
  // neither choice may deserialize in-process authority.
  REQUIRE((!injected || !injected->connection_scope.get()));
}

TEST_CASE("Deferred lobby leave retains the requesting connection", "[scope][isolated]") {
  sessions::LobbyLifecycle lifecycle;
  auto entry = lifecycle.create("lobby", "resource", "config").ticket;
  REQUIRE(lifecycle.launch(entry));
  REQUIRE(lifecycle.ready(entry));
  REQUIRE(lifecycle.join(entry, 42, false) == sessions::LobbyLifecycle::Join::admitted);
  const auto old = sessions::ConnectionScope::fresh(42);
  REQUIRE_FALSE(lifecycle.leave(entry, {}));
  REQUIRE_FALSE(lifecycle.leave(entry, old));
  auto deferred = lifecycle.finish(entry);
  REQUIRE(deferred.leaves.size() == 1);
  REQUIRE(deferred.leaves.front().matches(old));
  REQUIRE_FALSE(deferred.leaves.front().matches(sessions::ConnectionScope::fresh(42)));
}

TEST_CASE("Delayed producer switches never resolve a replacement from reused IDs", "[scope][isolated]") {
  const auto old_connection = sessions::ConnectionScope::fresh(42);
  const auto new_connection = sessions::ConnectionScope::fresh(42);
  const auto old_producer = sessions::RunScope::fresh("42");
  const auto new_producer = sessions::RunScope::fresh("42");
  events::SwitchStreamProducerEvents delayed{.session_id = 42,
                                             .interpipe_src_id = "42",
                                             .connection_scope = old_connection,
                                             .producer_scope = old_producer};
  REQUIRE_FALSE(sessions::resolve_producer(new_connection,
                                           delayed.connection_scope.get(),
                                           delayed.session_id,
                                           delayed.producer_scope.get(),
                                           delayed.interpipe_src_id));
  auto resolved = sessions::resolve_producer(old_connection,
                                             delayed.connection_scope.get(),
                                             delayed.session_id,
                                             delayed.producer_scope.get(),
                                             delayed.interpipe_src_id);
  REQUIRE(resolved);
  REQUIRE(*resolved == old_producer.resource_key());
  REQUIRE(*resolved != new_producer.resource_key());
  REQUIRE_FALSE(sessions::resolve_producer(old_connection, old_connection, 42, {}, "42"));
  REQUIRE_FALSE(sessions::resolve_producer(old_connection, old_connection, 42, old_producer, "wrong"));
}

TEST_CASE("Consumer binding retains the selected producer beyond the switch event", "[scope][isolated]") {
  sessions::ProducerBinding binding(sessions::RunScope::fresh("42"));
  std::string selected_key;
  {
    auto requested = sessions::RunScope::fresh("42");
    REQUIRE(binding.select(requested, [&](const std::string &key) {
      selected_key = key;
      return true;
    }));
  }
  for (int i = 0; i < 4096; ++i) {
    REQUIRE(sessions::RunScope::fresh("42").resource_key() != selected_key);
  }
  REQUIRE_FALSE(binding.select({}, [](const auto &) {
    FAIL("Null producer reached property setter");
    return true;
  }));
  REQUIRE_FALSE(binding.select(sessions::RunScope::fresh("42"), [](const auto &) { return false; }));
  REQUIRE(sessions::RunScope::fresh("42").resource_key() != selected_key);
}

namespace {
events::Lobby retained_lobby(const events::StreamSession &member) {
  events::Lobby lobby{.id = "joined-lobby",
                      .name = "joined",
                      .started_by_profile_id = "profile",
                      .multi_user = false,
                      .stop_when_everyone_leaves = true,
                      .run_scope = sessions::RunScope::fresh("joined-lobby")};
  // The committed membership representation written by the production join path.
  lobby.members->store(immer::map<std::size_t, std::shared_ptr<events::StreamSession>>{}.set(
      member.session_id,
      std::make_shared<events::StreamSession>(member)));
  lobby.connected_sessions->store(
      immer::vector<immer::box<std::string>>{immer::box<std::string>{std::to_string(member.session_id)}});
  return lobby;
}
} // namespace

TEST_CASE("Resumed audio and video retain the admitted lobby producer", "[scope][routing][isolated]") {
  auto app = isolated_state();
  auto source = scoped_session(42);
  sessions::LobbyLifecycle admission;
  auto ticket = admission.create("joined-lobby", "resource", "config").ticket;
  REQUIRE(admission.launch(ticket));
  REQUIRE(admission.ready(ticket));
  REQUIRE(admission.join(ticket, source.session_id, false) == sessions::LobbyLifecycle::Join::admitted);
  admission.finish(ticket);
  auto lobby = retained_lobby(source);
  lobby.run_scope = ticket->run_scope;
  app->lobbies->store(immer::vector<events::Lobby>{lobby});
  app->running_sessions->store(immer::vector<events::StreamSession>{source});
  auto handlers = sessions::setup_lobbies_handlers(app, "", {});
  // Input objects represent the already joined seat. No device/display is created.
  const auto joined_mouse = source.mouse;
  const auto joined_keyboard = source.keyboard;
  const auto joined_pads = source.joypads;
  int leaves = 0;
  auto observer = app->event_bus->register_handler<immer::box<events::LeaveLobbyEvent>>(
      [&](const immer::box<events::LeaveLobbyEvent> &) { ++leaves; });
  app->event_bus->fire_event(immer::box<events::PauseStreamEvent>{
      events::PauseStreamEvent{.session_id = source.session_id, .connection_scope = source.connection_scope}});
  auto resumed = source;
  resumed.connection_scope = sessions::ConnectionScope::fresh(source.session_id);
  app->running_sessions->store(immer::vector<events::StreamSession>{resumed});
  // Repeated join is already_joined, so correctness cannot rely on a fresh Switch event.
  CHECK(admission.join(ticket, resumed.session_id, false) == sessions::LobbyLifecycle::Join::already_joined);
  events::VideoSession video{.session_id = resumed.session_id,
                             .connection_scope = resumed.connection_scope,
                             .run_scope = resumed.run_scope};
  events::AudioSession audio{.session_id = resumed.session_id,
                             .connection_scope = resumed.connection_scope,
                             .run_scope = resumed.run_scope};
  auto v = state::prepare_stream_consumer(video, app->running_sessions->load(), app->lobbies->load());
  auto a = state::prepare_stream_consumer(audio, app->running_sessions->load(), app->lobbies->load());
  REQUIRE(v);
  REQUIRE(a);
  CHECK(v->producer_scope.get().matches(lobby.run_scope));
  CHECK(a->producer_scope.get().matches(lobby.run_scope));
  CHECK(v->run_scope.get().matches(source.run_scope));
  CHECK(a->run_scope.get().matches(source.run_scope));
  CHECK(v->connection_scope.get().matches(resumed.connection_scope));
  CHECK(a->connection_scope.get().matches(resumed.connection_scope));
  CHECK(leaves == 0);
  CHECK(lobby.connected_sessions->load()->size() == 1);
  CHECK(resumed.mouse == joined_mouse);
  CHECK(resumed.keyboard == joined_keyboard);
  CHECK(resumed.joypads == joined_pads);
  CHECK(lobby.members->load()->find(42)->get()->mouse == joined_mouse);
  // Prepared consumers own the selected producer even after the live list changes.
  app->lobbies->store({});
  CHECK(v->producer_scope.get().matches(lobby.run_scope));
  CHECK(a->producer_scope.get().matches(lobby.run_scope));
}

TEST_CASE("Consumer preparation rejects stale connections and replacement membership", "[scope][routing][isolated]") {
  const auto original = scoped_session(42);
  auto lobby = retained_lobby(original);
  auto replacement = scoped_session(42);
  auto all = immer::vector<events::StreamSession>{replacement};
  auto lobbies = immer::vector<events::Lobby>{lobby};
  events::VideoSession video{.session_id = 42,
                             .connection_scope = replacement.connection_scope,
                             .run_scope = replacement.run_scope,
                             .producer_scope = lobby.run_scope};
  events::AudioSession audio{.session_id = 42,
                             .connection_scope = replacement.connection_scope,
                             .run_scope = replacement.run_scope,
                             .producer_scope = lobby.run_scope};
  auto v = state::prepare_stream_consumer(video, all, lobbies);
  auto a = state::prepare_stream_consumer(audio, all, lobbies);
  REQUIRE(v);
  REQUIRE(a);
  CHECK(v->producer_scope.get().matches(replacement.run_scope));
  CHECK(a->producer_scope.get().matches(replacement.run_scope));
  video.connection_scope = original.connection_scope;
  audio.connection_scope = original.connection_scope;
  CHECK_FALSE(state::prepare_stream_consumer(video, all, lobbies));
  CHECK_FALSE(state::prepare_stream_consumer(audio, all, lobbies));
  video.connection_scope = replacement.connection_scope;
  video.run_scope = original.run_scope;
  CHECK_FALSE(state::prepare_stream_consumer(video, all, lobbies));
  audio.connection_scope = sessions::ConnectionScope{};
  CHECK_FALSE(state::prepare_stream_consumer(audio, all, lobbies));
}

TEST_CASE("Device routing uses retained run membership for relay and own queue", "[scope][routing][isolated]") {
  auto app = isolated_state();
  const auto member = scoped_session(42);
  auto lobby = retained_lobby(member);
  auto source = member;
  bool belongs = true;
  SECTION("resumed connection of the admitted run") {
    source.connection_scope = sessions::ConnectionScope::fresh(42);
  }
  SECTION("replacement run reusing numeric ID") {
    source = scoped_session(42);
    belongs = false;
  }
  SECTION("numeric projection without retained member") {
    lobby.members->store({});
    belongs = false;
  }
  SECTION("retained member without committed projection") {
    lobby.connected_sessions->store({});
    belongs = false;
  }
  SECTION("null lobby scope") {
    lobby.run_scope = {};
    belongs = false;
  }
  app->running_sessions->store(immer::vector<events::StreamSession>{source});
  app->lobbies->store(immer::vector<events::Lobby>{lobby});
  auto handlers = sessions::setup_lobbies_handlers(app, "", {});
  auto own_queue = std::make_shared<events::devices_atom_queue>();
  auto plug = immer::box<events::PlugDeviceEvent>{events::PlugDeviceEvent{.session_id = "42",
                                                                          .run_scope = source.run_scope,
                                                                          .connection_scope = source.connection_scope}};
  // Execute the same own-queue adapter invoked by setup_moonlight_handlers.
  CHECK(state::queue_own_session_device(plug,
                                        app->running_sessions->load(),
                                        app->lobbies->load(),
                                        source.run_scope,
                                        own_queue) == !belongs);
  auto own = own_queue->pop(std::chrono::milliseconds(0));
  CHECK(own.has_value() == !belongs);
  if (own)
    CHECK((*own)->run_scope.get().matches(source.run_scope));
  // Execute actual lobby event handlers; no runner/Gst/device startup.
  int unplugged = 0;
  auto observer = app->event_bus->register_handler<immer::box<events::UnplugDeviceEvent>>(
      [&](const immer::box<events::UnplugDeviceEvent> &ev) {
        if (ev->session_id == lobby.id) {
          ++unplugged;
          CHECK(ev->run_scope.get().matches(lobby.run_scope));
        }
      });
  app->event_bus->fire_event(plug);
  auto relayed = lobby.plugged_devices_queue->pop(std::chrono::milliseconds(0));
  CHECK(relayed.has_value() == belongs);
  if (relayed)
    CHECK((*relayed)->run_scope.get().matches(lobby.run_scope));
  app->event_bus->fire_event(
      immer::box<events::UnplugDeviceEvent>{events::UnplugDeviceEvent{.session_id = "42",
                                                                      .run_scope = source.run_scope,
                                                                      .connection_scope = source.connection_scope}});
  CHECK(unplugged == (belongs ? 1 : 0));
}
