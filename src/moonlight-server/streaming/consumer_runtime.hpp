#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <sessions/connection_scope.hpp>
#include <thread>
#include <vector>

namespace streaming {
using wolf::core::sessions::ConnectionScope;
using wolf::core::sessions::RunScope;

enum class ConsumerKind {
  Video,
  Audio
};
enum class ConsumerState {
  Waiting,
  Running,
  Stopping,
  Unknown,
  Stopped,
  Failed
};
enum class ConsumerAdmission {
  Accepted,
  Duplicate,
  Stale,
  Revoked,
  Busy,
  Capacity
};
enum class ConsumerReason {
  None,
  Cancelled,
  PingTimeout,
  Completed,
  Failure
};

struct ConsumerPolicy {
  std::chrono::milliseconds ping_timeout{5000};
  std::chrono::milliseconds drain_timeout{5000};
  std::chrono::milliseconds poll_interval{10};
  std::size_t max_jobs = 16;
  std::size_t max_records = 64;
  std::size_t max_unknown = 4;
};
struct ConsumerPing {
  std::string ip;
  unsigned short port = 0;
  std::optional<std::array<char, 16>> payload;
  // Retained transport; only the protocol adapter knows its concrete type.
  std::shared_ptr<void> socket;
};
struct ConsumerCorrelation {
  std::string ip;
  unsigned short port = 0;
  std::array<char, 16> secret;
  bool matches(const ConsumerPing &ping) const;
};
struct ConsumerCommands {
  bool idr = false;
  std::optional<RunScope> producer;
};
struct ConsumerStatus {
  ConsumerState state;
  ConsumerReason reason;
  bool joined;
};

// Retain this ticket across registry pruning/replacement. Only a successful
// completion after ALL reserved workers join is positive release evidence.
struct ConsumerDrainTicket {
  ConnectionScope connection;
  std::shared_future<void> released;
  bool complete() const {
    if (!released.valid() || released.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
      return false;
    try {
      released.get();
      return true;
    } catch (...) {
      return false; // broken promise/owner loss is never completion
    }
  }
};

// No pipeline pointer escapes to event-bus callers. These methods only touch
// bounded in-memory state; Gst operations belong exclusively to the worker.
class ConsumerJob {
  friend class ConsumerRuntime;
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  bool cancelled_ = false;
  std::optional<ConsumerPing> ping_;
  ConsumerCommands commands_;
  ConsumerState state_ = ConsumerState::Waiting;
  ConsumerReason reason_ = ConsumerReason::None;
  std::optional<std::chrono::steady_clock::time_point> stop_at_;
  std::atomic_bool returned_{false};
  bool succeeded_ = true;
  void cancel(ConsumerReason reason);

public:
  const ConnectionScope connection;
  const RunScope run;
  const ConsumerKind kind;
  const ConsumerCorrelation correlation;
  ConsumerJob(ConnectionScope connection, RunScope run, ConsumerKind kind, ConsumerCorrelation correlation);
  bool cancelled() const;
  void begin_drain(); // EOS/error starts a deadline, never publishes completion
  ConsumerCommands take_commands();
};

class ConsumerRuntime {
public:
  using Current = std::function<bool(const ConnectionScope &)>;
  // Returning means ALL work/callback/pipeline resources have been released.
  // A function unable to prove this must keep ownership and not return.
  using Work = std::function<bool(ConsumerJob &, const ConsumerPing &)>;

private:
  struct Worker {
    std::shared_ptr<ConsumerJob> job;
    std::thread thread;
    bool joined = false;
  };
  struct Record {
    ConnectionScope scope;
    bool revoked = false;
    std::vector<std::shared_ptr<Worker>> workers;
    std::shared_ptr<std::promise<void>> completion = std::make_shared<std::promise<void>>();
    std::shared_future<void> released = completion->get_future().share();
    bool published = false;
    void finish_if_drained();
  };
  ConsumerPolicy policy_;
  Current current_;
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::vector<Record> records_;
  bool closing_ = false;
  bool saturated_revocations_ = false;
  std::thread supervisor_;
  void supervise();
  void prune(); // registry lock held

public:
  explicit ConsumerRuntime(Current current, ConsumerPolicy policy = {});
  ~ConsumerRuntime(); // main/owner thread only: actual join, never detach
  ConsumerRuntime(const ConsumerRuntime &) = delete;
  ConsumerRuntime &operator=(const ConsumerRuntime &) = delete;
  ConsumerAdmission
  admit(ConnectionScope scope, RunScope run, ConsumerKind kind, ConsumerCorrelation correlation, Work work);
  std::optional<ConsumerDrainTicket> revoke(const ConnectionScope &scope);
  void ping(ConsumerKind kind, const ConsumerPing &ping);
  void command(const ConnectionScope &scope, ConsumerKind kind, ConsumerCommands command);
  std::optional<ConsumerStatus> status(const ConnectionScope &scope, ConsumerKind kind) const;
};
} // namespace streaming
