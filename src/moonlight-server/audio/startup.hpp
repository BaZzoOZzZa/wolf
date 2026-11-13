#pragma once
#include <sessions/handlers.hpp>

namespace wolf::core::audio::startup {
struct Operations {
  std::function<std::shared_ptr<Server>()> connect;
  std::function<bool(const std::shared_ptr<Server> &)> connected;
  // Owns ALL cleanup/Docker operations; never invoked when fallback is disabled.
  std::function<std::optional<sessions::AudioServer>()> fallback;
};
Operations defaults(const std::string &host_runtime_dir, const std::string &runtime_dir);
std::optional<sessions::AudioServer> setup(std::string_view allow_container, const Operations &);
// Actual run() dispatch boundary, before router, handlers and every listener.
// Trusted IO observation, not a guarantee that Pulse stays connected afterwards.
bool dispatch(const std::optional<sessions::AudioServer> &, const Operations &, const std::function<void()> &);
} // namespace wolf::core::audio::startup
