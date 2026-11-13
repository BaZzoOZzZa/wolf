#include "startup.hpp"
#include <filesystem>
#include <helpers/logger.hpp>
#include <helpers/utils.hpp>

namespace wolf::core::audio::startup {
std::optional<sessions::AudioServer> setup(std::string_view allow_container, const Operations &ops) {
  auto server = ops.connect();
  if (server && ops.connected(server))
    return sessions::AudioServer{.server = std::move(server)};
  if (allow_container == "FALSE") {
    logs::log(logs::error, "Pulse connection failed; container fallback disabled");
    return {};
  }
  return ops.fallback();
}
bool dispatch(const std::optional<sessions::AudioServer> &server,
              const Operations &ops,
              const std::function<void()> &start) {
  if (!server || !server->server || !ops.connected(server->server)) {
    logs::log(logs::error, "Pulse startup unavailable; ingress not started");
    return false;
  }
  start();
  return true;
}
Operations defaults(const std::string &host_runtime_dir, const std::string &runtime_dir) {
  return {
      [] { return audio::connect(); },
      [](const auto &server) { return audio::connected(server); },
      [host_runtime_dir, runtime_dir]() -> std::optional<sessions::AudioServer> {
        logs::log(logs::info, "Starting PulseAudio docker container");
        docker::DockerAPI docker_api(utils::get_env("WOLF_DOCKER_SOCKET", "/var/run/docker.sock"));
        auto pulse_socket = fmt::format("{}/pulse-socket", runtime_dir);

        /* Cleanup old leftovers, Pulse will fail to start otherwise */
        try {
          std::filesystem::remove(pulse_socket);
          std::filesystem::remove_all(fmt::format("{}/pulse", runtime_dir));
        } catch (const std::filesystem::filesystem_error &e) {
          logs::log(logs::warning, "Failed to remove old PulseAudio socket: {}", e.what());
        }

        auto container = docker_api.create(
            docker::Container{
                .id = "",
                .name = "WolfPulseAudio",
                .image = utils::get_env("WOLF_PULSE_IMAGE", "ghcr.io/games-on-whales/pulseaudio:master"),
                .status = docker::CREATED,
                .ports = {},
                .mounts = {docker::MountPoint{.source = host_runtime_dir, .destination = "/tmp/pulse/", .mode = "rw"}},
                .env = {"XDG_RUNTIME_DIR=/tmp/pulse/", "UNAME=retro", "UID=1000", "GID=1000", "HOME=/home/retro"}},
            // The following is needed when using podman (or any container that uses SELINUX). This way we can access
            // the socket that is created by PulseAudio from other containers (including this one).
            R"({
                  "HostConfig" : {
                    "SecurityOpt" : ["label=disable"]
                  }
            })");
        if (container && docker_api.start_by_id(container.value().id)) {
          auto ms = std::stoi(utils::get_env("WOLF_PULSE_CONTAINER_TIMEOUT_MS", "2000"));
          std::this_thread::sleep_for(std::chrono::milliseconds(ms)); // TODO: Better way of knowing when ready?
          return {{.server = audio::connect(fmt::format("{}/pulse-socket", runtime_dir)), .container = container}};
        }
        logs::log(logs::warning, "Failed to connect to any PulseAudio server, audio will not be available!");
        return {};
      }};
}
} // namespace wolf::core::audio::startup
