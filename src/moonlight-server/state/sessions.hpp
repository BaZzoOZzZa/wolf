#pragma once

#include <algorithm>
#include <events/events.hpp>
#include <helpers/logger.hpp>
#include <helpers/utils.hpp>
#include <immer/vector.hpp>
#include <optional>
#include <range/v3/view.hpp>
#include <state/config.hpp>
#include <state/serialised_config.hpp>

namespace state {

using namespace wolf::core;

inline std::optional<events::StreamSession> get_session_by_id(const immer::vector<events::StreamSession> &sessions,
                                                              const std::size_t id) {
  auto results =
      sessions |                                                                                             //
      ranges::views::filter([id](const events::StreamSession &session) { return session.session_id == id; }) //
      | ranges::views::take(1)                                                                               //
      | ranges::to_vector;                                                                                   //
  if (results.size() == 1) {
    return results[0];
  } else if (results.empty()) {
    return {};
  } else {
    logs::log(logs::warning, "Found multiple sessions for a given ID: {}", id);
    return {};
  }
}

inline std::optional<events::StreamSession> get_session_by_scope(const immer::vector<events::StreamSession> &all,
                                                                 const sessions::ConnectionScope &scope) {
  if (!scope)
    return {};
  for (const auto &session : all)
    if (sessions::targets(session.connection_scope, scope, session.session_id))
      return session;
  return {};
}

// Run-scoped administrative cleanup may deliberately address its newest
// connection. It must never reinterpret a stable ID as another run.
inline std::optional<events::StreamSession> get_session_by_run(const immer::vector<events::StreamSession> &all,
                                                               const sessions::RunScope &scope) {
  for (const auto &session : all)
    if (session.run_scope.matches(scope))
      return session;
  return {};
}

inline std::optional<events::StreamSession> get_session_by_client(const immer::vector<events::StreamSession> &sessions,
                                                                  const wolf::config::PairedClient &client) {
  auto client_id = get_client_id(client);
  return get_session_by_id(sessions, client_id);
}

inline std::optional<events::Lobby> get_lobby_by_id(const immer::vector<events::Lobby> &lobbies,
                                                    std::string_view lobby_id) {
  auto results = lobbies |                                                                                      //
                 ranges::views::filter([lobby_id](const events::Lobby &lobby) { return lobby.id == lobby_id; }) //
                 | ranges::views::take(1)                                                                       //
                 | ranges::to_vector;                                                                           //
  if (results.size() == 1) {
    return results[0];
  } else if (results.empty()) {
    return {};
  } else {
    logs::log(logs::warning, "Found multiple lobbies for a given ID: {}", lobby_id);
    return {};
  }
}

// Membership authority is the retained run, not its numeric display index.
// A resumed connection of that run is permitted; a replacement run is not.
// Require the committed projection too: join publishes members before the index,
// and leave removes the index before deleting the retained snapshot.
inline std::optional<events::Lobby> get_lobby_for_session(const immer::vector<events::Lobby> &lobbies,
                                                          const events::StreamSession &source) {
  if (!source.connection_scope || source.connection_scope.id() != source.session_id || !source.run_scope)
    return {};
  const auto session_id = std::to_string(source.session_id);
  for (const auto &lobby : lobbies) {
    if (!lobby.run_scope || lobby.run_scope.id() != lobby.id)
      continue;
    auto members = lobby.members->load();
    auto member = members->find(source.session_id);
    if (!member || !*member || (*member)->session_id != source.session_id ||
        !sessions::targets(source.run_scope, (*member)->run_scope, session_id))
      continue;
    auto connected = lobby.connected_sessions->load();
    if (std::any_of(connected->begin(), connected->end(), [&](const auto &id) { return *id == session_id; }))
      return lobby;
  }
  return {};
}

// Called after RTP ping, on the captured connection, for BOTH audio and video.
// Resolve membership for the exact captured current run. Keep connection/run
// identity separate from the selected producer; no caller-provided producer wins.
template <typename Consumer>
std::optional<Consumer> prepare_stream_consumer(const Consumer &request,
                                                const immer::vector<events::StreamSession> &all,
                                                const immer::vector<events::Lobby> &lobbies) {
  auto source = get_session_by_scope(all, request.connection_scope.get());
  if (!source || source->session_id != request.session_id || !source->run_scope.matches(request.run_scope.get()))
    return {};
  auto prepared = request;
  auto lobby = get_lobby_for_session(lobbies, *source);
  prepared.producer_scope = lobby ? lobby->run_scope : source->run_scope;
  return prepared;
}

// The real own-queue adapter is usable without startup, a runner or any device.
inline bool queue_own_session_device(const immer::box<events::PlugDeviceEvent> &event,
                                     const immer::vector<events::StreamSession> &all,
                                     const immer::vector<events::Lobby> &lobbies,
                                     const sessions::RunScope &queue_scope,
                                     const std::shared_ptr<events::devices_atom_queue> &queue) {
  auto source = get_session_by_scope(all, event->connection_scope.get());
  if (!source || !sessions::targets(source->run_scope, event->run_scope.get(), event->session_id) ||
      !queue_scope.matches(source->run_scope) || !queue || get_lobby_for_session(lobbies, *source))
    return false;
  queue->push(event);
  return true;
}

inline std::shared_ptr<events::StreamSession> create_stream_session(immer::box<state::AppState> state,
                                                                    const events::App &run_app,
                                                                    const wolf::config::PairedClient &current_client,
                                                                    const moonlight::DisplayMode &display_mode,
                                                                    int audio_channel_count,
                                                                    const std::string &aes_key,
                                                                    const std::string &aes_iv) {
  auto full_path = std::filesystem::path(state->host->local_base_state_folder) / current_client.app_state_folder /
                   run_app.base.title;
  logs::log(logs::debug, "Host app state folder: {}, creating paths", full_path.string());
  std::filesystem::create_directories(full_path);

  std::random_device rd;
  std::mt19937 generator(rd());

  std::uniform_int_distribution<> chars(33, 126); // ASCII values for printable character
  std::array<char, 16> rtp_secret_payload;
  for (auto &c : rtp_secret_payload) {
    c = static_cast<char>(chars(generator));
  }

  std::uniform_int_distribution<u_int32_t> uints(0, UINT32_MAX);

  std::uniform_int_distribution<> ints(0, 255);
  auto rtsp_fake_ip = fmt::format("{}.{}.{}.{}", ints(generator), ints(generator), ints(generator), ints(generator));

  auto session = events::StreamSession{
      .display_mode = display_mode,
      .audio_channel_count = audio_channel_count,
      .event_bus = state->event_bus,
      .client_settings = current_client.settings,
      .app = std::make_shared<events::App>(run_app),
      .app_local_state_folder = full_path.string(),
      .app_host_state_folder = std::filesystem::path(state->host->host_base_state_folder) /
                               current_client.app_state_folder / run_app.base.title,

      .aes_key = aes_key,
      .aes_iv = aes_iv,

      // Moonlight protocol extension to support IP-less connections
      .rtp_secret_payload = rtp_secret_payload,
      .enet_secret_payload = uints(generator),
      .rtsp_fake_ip = rtsp_fake_ip,

      // client info
      .session_id = get_client_id(current_client),
      .video_stream_port = static_cast<unsigned short>(get_port(VIDEO_PING_PORT)),
      .audio_stream_port = static_cast<unsigned short>(get_port(AUDIO_PING_PORT)),
      .control_stream_port = static_cast<unsigned short>(get_port(CONTROL_PORT))};

  session.connection_scope = sessions::ConnectionScope::fresh(session.session_id);
  session.run_scope = sessions::RunScope::fresh(std::to_string(session.session_id));
  return std::make_shared<events::StreamSession>(session);
}

inline immer::vector<events::StreamSession> remove_session(const immer::vector<events::StreamSession> &sessions,
                                                           const events::StreamSession &session) {
  return sessions                                                                                           //
         | ranges::views::filter([scope = session.connection_scope](const events::StreamSession &cur_ses) { //
             return !cur_ses.connection_scope.matches(scope);                                               //
           })                                                                                               //
         | ranges::to<immer::vector<events::StreamSession>>();                                              //
}
} // namespace state