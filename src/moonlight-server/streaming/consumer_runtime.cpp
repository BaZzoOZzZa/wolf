#include "consumer_runtime.hpp"
#include <algorithm>
#include <stdexcept>

namespace streaming {
bool ConsumerCorrelation::matches(const ConsumerPing &ping) const {
  return ping.payload ? *ping.payload == secret : ping.ip == ip && ping.port == port;
}
ConsumerJob::ConsumerJob(ConnectionScope connection, RunScope run, ConsumerKind kind, ConsumerCorrelation correlation)
    : connection(std::move(connection)), run(std::move(run)), kind(kind), correlation(std::move(correlation)) {}
bool ConsumerJob::cancelled() const {
  std::lock_guard lock(mutex_);
  return cancelled_;
}
void ConsumerJob::begin_drain() {
  cancel(ConsumerReason::Completed);
}
ConsumerCommands ConsumerJob::take_commands() {
  std::lock_guard lock(mutex_);
  return std::exchange(commands_, {});
}
void ConsumerJob::cancel(ConsumerReason reason) {
  std::lock_guard lock(mutex_);
  if (!cancelled_) {
    cancelled_ = true;
    reason_ = reason;
    stop_at_ = std::chrono::steady_clock::now();
    if (state_ != ConsumerState::Stopped && state_ != ConsumerState::Failed)
      state_ = ConsumerState::Stopping;
    commands_ = {};
  }
  wake_.notify_all();
}
ConsumerRuntime::ConsumerRuntime(Current current, ConsumerPolicy policy)
    : policy_(policy), current_(std::move(current)) {
  if (!current_ || policy_.poll_interval.count() <= 0 || policy_.ping_timeout.count() <= 0 ||
      policy_.drain_timeout.count() <= 0 || !policy_.max_jobs || !policy_.max_records || !policy_.max_unknown)
    throw std::invalid_argument("Invalid consumer policy");
  supervisor_ = std::thread([this] { supervise(); });
}
ConsumerRuntime::~ConsumerRuntime() {
  {
    std::lock_guard lock(mutex_);
    closing_ = true;
    for (auto &record : records_)
      for (auto &worker : record.workers)
        worker->job->cancel(ConsumerReason::Cancelled);
  }
  wake_.notify_all();
  supervisor_.join();
  // External Gst/driver calls may not return. Keep ownership rather than claim
  // a bounded physical shutdown or detach a worker with borrowed resources.
  for (auto &record : records_)
    for (auto &worker : record.workers)
      if (worker->thread.joinable())
        worker->thread.join();
}
void ConsumerRuntime::prune() {
  std::erase_if(records_, [&](const Record &record) {
    return !current_(record.scope) &&
           std::all_of(record.workers.begin(), record.workers.end(), [](auto &w) { return w->joined; });
  });
}
ConsumerAdmission ConsumerRuntime::admit(
    ConnectionScope scope, RunScope run, ConsumerKind kind, ConsumerCorrelation correlation, Work work) {
  std::lock_guard lock(mutex_);
  if (!scope || !run || run.id() != std::to_string(scope.id()) || !current_(scope))
    return ConsumerAdmission::Stale;
  if (closing_ || saturated_revocations_)
    return ConsumerAdmission::Capacity;
  prune();
  Record *record = nullptr;
  std::size_t active = 0, unknown = 0;
  for (auto &entry : records_) {
    if (entry.scope.matches(scope)) {
      record = &entry;
      if (entry.revoked)
        return ConsumerAdmission::Revoked;
      for (auto &worker : entry.workers)
        if (worker->job->kind == kind)
          return ConsumerAdmission::Duplicate;
    }
    for (auto &worker : entry.workers) {
      if (!worker->joined) {
        ++active;
        if (entry.scope.id() == scope.id() && !entry.scope.matches(scope))
          return ConsumerAdmission::Busy;
      }
      std::lock_guard job_lock(worker->job->mutex_);
      unknown += worker->job->state_ == ConsumerState::Unknown;
    }
  }
  if (active >= policy_.max_jobs || unknown >= policy_.max_unknown ||
      (!record && records_.size() >= policy_.max_records))
    return ConsumerAdmission::Capacity;
  if (!record) {
    records_.push_back(Record{scope, false, {}});
    record = &records_.back();
  }
  auto worker = std::make_shared<Worker>();
  worker->job = std::make_shared<ConsumerJob>(scope, run, kind, std::move(correlation));
  record->workers.push_back(worker); // reservation precedes thread launch
  try {
    worker->thread =
        std::thread([job = worker->job, work = std::move(work), current = current_, policy = policy_]() mutable {
          try {
            std::optional<ConsumerPing> ping;
            {
              std::unique_lock lock(job->mutex_);
              if (!job->wake_.wait_for(lock, policy.ping_timeout, [&] {
                    return job->cancelled_ || job->ping_.has_value();
                  })) {
                job->cancelled_ = true;
                job->reason_ = ConsumerReason::PingTimeout;
              }
              if (!job->cancelled_ && current(job->connection)) {
                // This is the activation linearization point. A later revoke sees
                // Running and requests drain; a prior revoke can never start work.
                job->state_ = ConsumerState::Running;
                ping = std::move(job->ping_);
              }
            }
            if (ping)
              job->succeeded_ = work(*job, *ping);
          } catch (...) {
            job->succeeded_ = false;
          }
          // Destroy captures (including scoped pipeline resources) BEFORE the
          // reaper is told it can join. No callback may retain a borrowed job.
          work = {};
          current = {};
          job->returned_.store(true, std::memory_order_release);
        });
  } catch (...) {
    record->workers.pop_back();
    throw;
  }
  wake_.notify_all();
  return ConsumerAdmission::Accepted;
}
void ConsumerRuntime::revoke(const ConnectionScope &scope) {
  if (!scope)
    return;
  std::lock_guard lock(mutex_);
  for (auto &record : records_) {
    if (!record.scope.matches(scope))
      continue;
    record.revoked = true;
    for (auto &worker : record.workers)
      worker->job->cancel(ConsumerReason::Cancelled);
    wake_.notify_all();
    return;
  }
  if (!current_(scope))
    return;
  prune();
  if (records_.size() >= policy_.max_records) {
    // Losing a current-scope tombstone would reopen stop-before-register.
    // Permanently fail closed until the owner runtime is replaced at shutdown.
    saturated_revocations_ = true;
    return;
  }
  records_.push_back(Record{scope, true, {}});
}
void ConsumerRuntime::ping(ConsumerKind kind, const ConsumerPing &ping) {
  std::lock_guard lock(mutex_);
  for (auto &record : records_)
    for (auto &worker : record.workers) {
      auto &job = *worker->job;
      if (record.revoked || job.kind != kind || !current_(job.connection) || !job.correlation.matches(ping))
        continue;
      std::lock_guard job_lock(job.mutex_);
      if (job.state_ == ConsumerState::Waiting && !job.cancelled_ && !job.ping_) {
        job.ping_ = ping;
        job.wake_.notify_all();
      }
    }
}
void ConsumerRuntime::command(const ConnectionScope &scope, ConsumerKind kind, ConsumerCommands command) {
  if (!scope)
    return;
  std::lock_guard lock(mutex_);
  for (auto &record : records_)
    if (!record.revoked && record.scope.matches(scope))
      for (auto &worker : record.workers) {
        auto &job = *worker->job;
        std::lock_guard job_lock(job.mutex_);
        if (job.kind != kind || job.cancelled_ || worker->joined)
          continue;
        job.commands_.idr |= command.idr;
        if (command.producer && *command.producer)
          job.commands_.producer = std::move(command.producer);
      }
}
std::optional<ConsumerStatus> ConsumerRuntime::status(const ConnectionScope &scope, ConsumerKind kind) const {
  std::lock_guard lock(mutex_);
  for (auto &record : records_)
    if (record.scope.matches(scope))
      for (auto &worker : record.workers)
        if (worker->job->kind == kind) {
          std::lock_guard job_lock(worker->job->mutex_);
          return ConsumerStatus{worker->job->state_, worker->job->reason_, worker->joined};
        }
  return {};
}
void ConsumerRuntime::supervise() {
  std::unique_lock lock(mutex_);
  while (!closing_) {
    std::vector<std::shared_ptr<Worker>> ready;
    for (auto &record : records_)
      for (auto &worker : record.workers) {
        if (worker->joined)
          continue;
        auto &job = *worker->job;
        if (!current_(job.connection))
          job.cancel(ConsumerReason::Cancelled);
        if (job.returned_.load(std::memory_order_acquire)) {
          ready.push_back(worker);
        } else {
          std::lock_guard job_lock(job.mutex_);
          if (job.stop_at_ && std::chrono::steady_clock::now() - *job.stop_at_ >= policy_.drain_timeout)
            job.state_ = ConsumerState::Unknown;
        }
      }
    lock.unlock();
    for (auto &worker : ready)
      worker->thread.join();
    lock.lock();
    for (auto &worker : ready) {
      worker->joined = true;
      auto &job = *worker->job;
      std::lock_guard job_lock(job.mutex_);
      job.state_ = job.succeeded_ ? ConsumerState::Stopped : ConsumerState::Failed;
      if (!job.succeeded_)
        job.reason_ = ConsumerReason::Failure;
      else if (job.reason_ == ConsumerReason::None)
        job.reason_ = ConsumerReason::Completed;
      job.ping_.reset();
      job.commands_ = {};
    }
    wake_.wait_for(lock, policy_.poll_interval);
  }
}
} // namespace streaming
