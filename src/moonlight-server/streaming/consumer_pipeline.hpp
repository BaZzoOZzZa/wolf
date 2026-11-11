#pragma once
#include "consumer_runtime.hpp"
#include <gst/gst.h>

namespace streaming {
// User data stays owned by the worker and by Gst's destroy-notify holder.
// After callbacks are disconnected, close_and_wait proves all entries exited.
class ConsumerCallbackBarrier {
  std::mutex mutex_;
  std::condition_variable wake_;
  std::size_t active_ = 0;
  std::size_t holders_ = 0;
  bool closed_ = false;

public:
  class Lease {
    ConsumerCallbackBarrier *owner_;

  public:
    explicit Lease(ConsumerCallbackBarrier *owner) : owner_(owner) {}
    Lease(const Lease &) = delete;
    Lease &operator=(const Lease &) = delete;
    explicit operator bool() const {
      return owner_ != nullptr;
    }
    ~Lease() {
      if (owner_) {
        std::lock_guard lock(owner_->mutex_);
        --owner_->active_;
        owner_->wake_.notify_all();
      }
    }
  };
  Lease enter() {
    std::lock_guard lock(mutex_);
    if (closed_)
      return Lease(nullptr);
    ++active_;
    return Lease(this);
  }
  void retain_holder() {
    std::lock_guard lock(mutex_);
    ++holders_;
  }
  void retire_holder() {
    std::lock_guard lock(mutex_);
    --holders_;
    wake_.notify_all();
  }
  void close_and_wait() {
    std::unique_lock lock(mutex_);
    closed_ = true;
    wake_.wait(lock, [&] { return active_ == 0; });
  }
};

struct ConsumerPipelineHooks {
  std::function<void(GstElement *)> setup;
  std::function<void(GstElement *, ConsumerCommands)> commands;
  // Runs after confirmed NULL. Disconnect callback holders and drain them here.
  // Must not throw; a non-returning drain stays owned and becomes Unknown.
  std::function<void(GstElement *)> drain;
};
// The only consumer Gst lifecycle executor; producer run_pipeline is unchanged.
// Called on an owned worker, never a control/event-bus callback.
bool run_consumer_pipeline(const std::string &description, ConsumerJob &job, const ConsumerPipelineHooks &hooks = {});
} // namespace streaming
