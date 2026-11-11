#pragma once
#include <chrono>
#include <cstdlib>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace wolf::core::docker {

enum ContainerStatus {
  CREATED,
  RUNNING,
  PAUSED,
  RESTARTING,
  REMOVING,
  EXITED,
  DEAD,
  UNKNOWN
};

enum PortType {
  TCP,
  UDP
};

struct Port {
  int private_port;
  int public_port;
  PortType type;
};

struct MountPoint {
  std::string source;
  std::string destination;
  std::string mode;
};

struct Device {
  std::string path_on_host;
  std::string path_in_container;
  std::string cgroup_permission;
};

struct Container {
  std::string id;
  std::string name;
  std::string hostname;

  std::string image;

  ContainerStatus status;

  std::vector<Port> ports;
  std::vector<MountPoint> mounts;
  std::vector<Device> devices;
  std::vector<std::string> env;
};

/**
 * CURL needs to be initialised once
 */
void init();

/**
 * Container engines do not all use the same status code for name conflicts.
 */
bool is_container_name_conflict_response(long status_code, std::string_view response_body);

enum class CreateState {
  created,
  rejected,
  conflict,
  unknown
};

struct CreateResult {
  CreateState state = CreateState::unknown;
  std::optional<Container> container;
};

// Explicitly distinguishes an ambiguous external operation from a config or
// programmer exception. The identity is retained for operator reconciliation.
struct DockerOutcomeUnknown : std::runtime_error {
  const std::string identity;
  explicit DockerOutcomeUnknown(std::string identity)
      : std::runtime_error("Docker outcome unknown: " + identity), identity(std::move(identity)) {}
};

enum class InspectState {
  present,
  missing,
  unknown
};

struct InspectResult {
  InspectState state = InspectState::unknown;
  std::optional<Container> container;

  // Only confirmed absence or a terminal container state ends supervision.
  [[nodiscard]] bool terminal() const {
    return state == InspectState::missing ||
           (state == InspectState::present && container && (container->status == EXITED || container->status == DEAD));
  }
};

// A timed-out start may still commit after an early Exited inspect. Until an
// active state has been observed, that Exited result is not a teardown barrier.
class ContainerMonitor {
public:
  using Clock = std::chrono::steady_clock;
  enum class Decision {
    live,
    reconcile,
    terminal,
    quarantine
  };

  explicit ContainerMonitor(bool start_confirmed) : start_observed_(start_confirmed) {}

  Decision observe(const InspectResult &result, Clock::time_point now) {
    if (result.state == InspectState::missing) {
      return Decision::terminal; // An immutable ID cannot be started after removal.
    }
    if (result.state == InspectState::present && result.container) {
      auto status = result.container->status;
      if (status == RUNNING || status == PAUSED || status == RESTARTING) {
        start_observed_ = true;
      }
      if (result.terminal() && start_observed_) {
        return Decision::terminal;
      }
      if (status != UNKNOWN && status != CREATED && !result.terminal()) {
        uncertain_since_.reset();
        return Decision::live;
      }
    }
    if (!uncertain_since_) {
      uncertain_since_ = now;
    }
    return now - *uncertain_since_ >= std::chrono::seconds(30) ? Decision::quarantine : Decision::reconcile;
  }

private:
  bool start_observed_;
  std::optional<Clock::time_point> uncertain_since_;
};

class DockerAPI {
private:
  std::string socket_path; // TODO: add B64 registry_auth
  std::string docker_api_version;

public:
  explicit DockerAPI(std::string socket_path = "/var/run/docker.sock") : socket_path(std::move(socket_path)) {
    docker_api_version = get_api_version();
  }

  /**
   * Get a list of all containers
   *
   * https://docs.docker.com/engine/api/v1.30/#tag/Container/operation/ContainerList
   * @param all: Return all containers. If false, only running containers are shown
   */
  [[nodiscard]] std::vector<Container> get_containers(bool all = true) const;

  /**
   * Get a container
   *
   * https://docs.docker.com/engine/api/v1.30/#tag/Container/operation/ContainerInspect
   */
  [[nodiscard]] std::optional<Container> get_by_id(std::string_view id) const;

  // Unlike get_by_id(), preserves the distinction between 404 and an uncertain result.
  [[nodiscard]] InspectResult inspect_by_id(std::string_view id) const;

  /**
   * On success, returns the newly created docker container
   * this will differ from the input container, for example:
   *  - `id` will be added based on the returned ID
   *  - `env` will be the merged with the original container ENV variables
   *
   *  https://docs.docker.com/engine/api/v1.30/#tag/Container/operation/ContainerCreate
   *
   *  @param registry_auth: optional, base64 encoded registry auth in case the image is missing
   *  @see https://docs.docker.com/engine/api/v1.30/#section/Authentication
   *
   *  @param force_recreate_if_present: if a container with the same name is already present it will be removed
   */
  std::optional<Container> create(const Container &container,
                                  std::string_view custom_params = "{}",
                                  std::string_view registry_auth = {},
                                  bool force_recreate_if_present = false) const;

  [[nodiscard]] CreateResult create_with_outcome(const Container &container,
                                                 std::string_view custom_params = "{}",
                                                 std::string_view registry_auth = {},
                                                 bool force_recreate_if_present = false) const;

  /**
   * Starts the container
   *
   * https://docs.docker.com/engine/api/v1.30/#tag/Container/operation/ContainerStart
   */
  bool start_by_id(std::string_view id) const;

  /**
   * Stops the container
   *
   * https://docs.docker.com/engine/api/v1.30/#tag/Container/operation/ContainerStop
   * @param timeout_seconds: Number of seconds to wait before killing the container
   */
  bool stop_by_id(std::string_view id, int timeout_seconds = 2) const;

  /**
   * Removes the container
   *
   * https://docs.docker.com/engine/api/v1.30/#tag/Container/operation/ContainerDelete
   *
   * @param remove_volumes: Remove anonymous volumes associated with the container.
   * @param force: If the container is running, kill it before removing it.
   * @param link: Remove the specified link associated with the container.
   */
  bool remove_by_id(std::string_view id, bool remove_volumes = false, bool force = false, bool link = false) const;

  /**
   * Searches for a container with the given name and then removes it if present.
   */
  bool remove_by_name(std::string_view name, bool remove_volumes = false, bool force = false, bool link = false) const;

  /**
   * Returns the full json response as is from /images/{image_name}/json
   * Optional because the image might be missing locally
   */
  std::optional<std::string> inspect_image(std::string_view image_name) const;

  /**
   * Downloads a Docker image
   */
  bool pull_image(std::string_view image_name, std::string_view registry_auth = {}) const;

  struct DockerProgressEvent {
    std::string layer_id;
    long current_progress;
    long total;
  };

  /**
   * Download a docker image with a callback for progress
   */
  bool pull_image(std::string_view image_name,
                  std::string_view registry_auth,
                  const std::function<void(const DockerProgressEvent &)> &progress_fn) const;

  bool exec(std::string_view id, const std::vector<std::string_view> &command, std::string_view user = "root") const;

  /**
   * Get the container logs
   * https://docs.docker.com/engine/api/v1.40/#tag/Container/operation/ContainerLogs
   */
  std::string get_logs(std::string_view id,
                       bool get_stdout = true,
                       bool get_stderr = true,
                       int since = 0,
                       int until = 0,
                       bool timestamps = false);

  std::string get_api_version();
};

} // namespace wolf::core::docker
