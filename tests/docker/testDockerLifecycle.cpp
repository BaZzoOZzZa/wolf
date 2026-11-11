// Uses only a scripted temporary Unix HTTP socket, never a Docker daemon.
#include <core/docker.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <exception>
#include <iostream>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

using namespace wolf::core::docker;
using namespace std::chrono_literals;

namespace {
void check(bool condition, const char *expression, int line) {
  if (!condition) {
    throw std::runtime_error(std::string(expression) + " at line " + std::to_string(line));
  }
}
#define CHECK(...) check((__VA_ARGS__), #__VA_ARGS__, __LINE__)

struct Step {
  std::string request;
  int status;
  std::string body;
  bool disconnect = false;
  std::chrono::milliseconds delay = 0ms;
};

class MockDocker {
public:
  explicit MockDocker(std::vector<Step> steps) {
    char directory[] = "/tmp/wolf-docker-test-XXXXXX";
    auto created = mkdtemp(directory);
    if (!created) {
      throw std::runtime_error("mkdtemp failed");
    }
    directory_ = created;
    path = directory_ + "/engine.sock";
    listener_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (listener_ < 0 || bind(listener_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) ||
        listen(listener_, 8)) {
      cleanup();
      throw std::runtime_error("Unable to bind mock Docker socket");
    }
    steps.insert(steps.begin(), {"GET /version ", 200, R"({"ApiVersion":"1.40"})"});
    worker_ = std::thread([this, steps = std::move(steps)] {
      try {
        for (const auto &step : steps) {
          pollfd available{listener_, POLLIN, 0};
          if (poll(&available, 1, 2000) != 1) {
            throw std::runtime_error("Expected Docker request was not received");
          }
          const int client = accept(listener_, nullptr, nullptr);
          if (client < 0) {
            throw std::runtime_error("accept failed");
          }
          try {
            auto request = read_request(client);
            CHECK(request.starts_with(step.request));
            std::this_thread::sleep_for(step.delay);
            if (!step.disconnect) {
              auto response = "HTTP/1.1 " + std::to_string(step.status) +
                              " Test\r\nContent-Length: " + std::to_string(step.body.size()) +
                              "\r\nConnection: close\r\n\r\n" + step.body;
              std::size_t sent = 0;
              while (sent < response.size()) {
                auto count = send(client, response.data() + sent, response.size() - sent, MSG_NOSIGNAL);
                if (count <= 0) {
                  break; // Expected when testing the client's request deadline.
                }
                sent += count;
              }
            }
          } catch (...) {
            close(client);
            throw;
          }
          close(client);
        }
      } catch (...) {
        failure_ = std::current_exception();
      }
      // Any unexpected retry now fails immediately rather than contacting a daemon.
      shutdown(listener_, SHUT_RDWR);
    });
  }

  ~MockDocker() {
    if (worker_.joinable()) {
      shutdown(listener_, SHUT_RDWR);
      worker_.join();
    }
    cleanup();
  }

  void finish() {
    worker_.join();
    if (failure_) {
      std::rethrow_exception(failure_);
    }
  }

  std::string path;

private:
  static std::string read_request(int client) {
    timeval timeout{2, 0};
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    std::string request;
    char buffer[4096];
    for (;;) {
      auto count = recv(client, buffer, sizeof(buffer), 0);
      if (count <= 0 || request.size() > 1024 * 1024) {
        throw std::runtime_error("Incomplete mock HTTP request");
      }
      request.append(buffer, count);
      auto header_end = request.find("\r\n\r\n");
      if (header_end == std::string::npos) {
        continue;
      }
      if (request.substr(0, header_end).find("Transfer-Encoding: chunked") != std::string::npos) {
        if (request.find("\r\n0\r\n\r\n", header_end + 2) != std::string::npos) {
          return request;
        }
      } else if (auto length = request.find("Content-Length: "); length != std::string::npos && length < header_end) {
        if (request.size() >= header_end + 4 + std::stoul(request.substr(length + 16))) {
          return request;
        }
      } else {
        return request;
      }
    }
  }

  void cleanup() {
    if (listener_ >= 0) {
      close(listener_);
    }
    if (!path.empty()) {
      unlink(path.c_str());
    }
    if (!directory_.empty()) {
      rmdir(directory_.c_str());
    }
  }

  int listener_ = -1;
  std::string directory_;
  std::thread worker_;
  std::exception_ptr failure_;
};

std::string inspect_body(const std::string &status) {
  return R"({"Id":"known-id","Name":"/test","State":{"Status":")" + status +
         R"("},"HostConfig":{"PortBindings":null,"Devices":null},"Mounts":null,)"
         R"("Config":{"Image":"test","Env":null,"Hostname":"test-host"}})";
}

void inspect_errors_are_not_absence() {
  MockDocker mock({{"GET /v1.40/containers/known-id/json ", 200, inspect_body("running")},
                   {"GET /v1.40/containers/known-id/json ", 500, "daemon unavailable"},
                   {"GET /v1.40/containers/known-id/json ", 404, "missing"},
                   {"GET /v1.40/containers/known-id/json ", 200, "{}"},
                   {"GET /v1.40/containers/known-id/json ", 200, inspect_body("future-state")}});
  DockerAPI api(mock.path);
  CHECK(api.inspect_by_id("known-id").state == InspectState::present);
  CHECK(api.inspect_by_id("known-id").state == InspectState::unknown);
  CHECK(api.inspect_by_id("known-id").state == InspectState::missing);
  CHECK(api.inspect_by_id("known-id").state == InspectState::unknown);
  auto unknown_status = api.inspect_by_id("known-id");
  CHECK(unknown_status.state == InspectState::present);
  CHECK(unknown_status.container->status == UNKNOWN);
  CHECK(!unknown_status.terminal());
  mock.finish();
}

void conflict_is_not_recreation() {
  MockDocker mock({{"POST /v1.40/containers/create?name=test ", 409, R"({"message":"name already in use"})"}});
  DockerAPI api(mock.path);
  auto result = api.create_with_outcome(Container{.name = "test", .image = "test", .status = CREATED});
  CHECK(result.state == CreateState::conflict);
  CHECK(!result.container);
  mock.finish();
}

void created_id_survives_inspect_failure() {
  MockDocker mock({{"POST /v1.40/containers/create?name=test ", 201, R"({"Id":"known-id"})"},
                   {"GET /v1.40/containers/known-id/json ", 503, "temporarily unavailable"}});
  DockerAPI api(mock.path);
  auto result = api.create_with_outcome(Container{.name = "test", .image = "test", .status = CREATED});
  CHECK(result.state == CreateState::created);
  CHECK(result.container->id == "known-id");
  CHECK(result.container->status == UNKNOWN);
  mock.finish();
}

void lost_response_is_unknown_and_not_retried() {
  MockDocker mock({{"POST /v1.40/containers/create?name=test ", 0, "", true}});
  DockerAPI api(mock.path);
  auto result = api.create_with_outcome(Container{.name = "test", .image = "test", .status = CREATED});
  CHECK(result.state == CreateState::unknown);
  mock.finish();
}

void rejected_create_and_bounded_pull_retry() {
  {
    MockDocker mock({{"POST /v1.40/containers/create?name=test ", 400, "invalid config"}});
    DockerAPI api(mock.path);
    CHECK(api.create_with_outcome(Container{.name = "test", .image = "test", .status = CREATED}).state ==
          CreateState::rejected);
    mock.finish();
  }
  {
    MockDocker mock({{"POST /v1.40/containers/create?name=test ", 404, "image missing"},
                     {"POST /v1.40/images/create?fromImage=test ", 200, ""},
                     {"POST /v1.40/containers/create?name=test ", 404, "still missing"}});
    DockerAPI api(mock.path);
    CHECK(api.create_with_outcome(Container{.name = "test", .image = "test", .status = CREATED}).state ==
          CreateState::rejected);
    mock.finish();
  }
}

void malformed_create_response_retains_uncertainty() {
  MockDocker mock({{"POST /v1.40/containers/create?name=test ", 201, "{}"}});
  DockerAPI api(mock.path);
  CHECK(api.create_with_outcome(Container{.name = "test", .image = "test", .status = CREATED}).state ==
        CreateState::unknown);
  mock.finish();
}

void exec_reuses_a_handle_without_reusing_post_state() {
  MockDocker mock({{"POST /v1.40/containers/known-id/exec ", 201, R"({"Id":"exec-id"})"},
                   {"POST /v1.40/exec/exec-id/start ", 200, "output"},
                   {"GET /v1.40/exec/exec-id/json ", 200, R"({"ExitCode":0})"}});
  DockerAPI api(mock.path);
  CHECK(api.exec("known-id", {"test"}));
  mock.finish();
}

void slow_engine_has_a_request_deadline() {
  MockDocker mock({{"GET /v1.40/containers/known-id/json ", 200, inspect_body("running"), false, 11s}});
  DockerAPI api(mock.path);
  auto started = std::chrono::steady_clock::now();
  CHECK(api.inspect_by_id("known-id").state == InspectState::unknown);
  auto elapsed = std::chrono::steady_clock::now() - started;
  CHECK(elapsed >= 9s);
  CHECK(elapsed < 13s);
  mock.finish();
}
} // namespace

int main() {
  try {
    init();
    inspect_errors_are_not_absence();
    conflict_is_not_recreation();
    created_id_survives_inspect_failure();
    lost_response_is_unknown_and_not_retried();
    rejected_create_and_bounded_pull_retry();
    malformed_create_response_retains_uncertainty();
    exec_reuses_a_handle_without_reusing_post_state();
    slow_engine_has_a_request_deadline();
    std::cout << "8 Docker lifecycle mock scenarios passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
