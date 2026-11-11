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
