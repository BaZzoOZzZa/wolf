#pragma once

#include "connection_scope.hpp"
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace wolf::core::sessions {

// One instance belongs to setup_lobbies_handlers, never to a process-global
// singleton. No event, promise, I/O or caller code executes under this mutex.
class LobbyLifecycle {
public:
  enum class Phase {
    starting,
    ready,
    unknown,
    stopping,
    closed
  };
  struct Entry {
    const std::string id;
    const RunScope run_scope;

  private:
    std::string resource;
    std::string fingerprint;
    friend class LobbyLifecycle;
    Phase phase = Phase::starting;
    bool launched = false;
    bool runner_done = false;
    std::size_t producers = 1; // Video startup owns the initial reservation.
    bool video_done = false;
    bool audio_reserved = false;
    bool audio_done = false;
    bool operation = false;
    bool cleaning = false;
    bool cleaned = false;
    bool setup_complete = true;
    bool forget_claimed = false;
    bool stop_notified = false;
    std::set<std::size_t> members;
    std::map<std::size_t, ConnectionScope> deferred_leaves;

  public:
    Entry(std::string id, std::string resource, std::string fingerprint)
        : id(std::move(id)), run_scope(RunScope::fresh(this->id)), resource(std::move(resource)),
          fingerprint(std::move(fingerprint)) {}
  };
  using Ticket = std::shared_ptr<Entry>;
  enum class Producer {
    video,
    audio
  };
  enum class Create {
    admitted,
    replay_ready,
    pending,
    conflict,
    exhausted
  };
  struct Admission {
    Create result;
    Ticket ticket;
  };
  enum class Join {
    admitted,
    already_joined,
    unavailable,
    full,
    session_busy
  };
  struct Deferred {
    bool stop = false;
    std::vector<ConnectionScope> leaves;
  };

  explicit LobbyLifecycle(std::size_t capacity = 1024) : capacity_(capacity) {}

  Admission create(std::string id, std::string resource, std::string fingerprint) {
    std::lock_guard lock(mutex_);
    if (auto it = entries_.find(id); it != entries_.end()) {
      auto entry = it->second;
      if (entry->phase == Phase::closed || entry->resource != resource || entry->fingerprint != fingerprint) {
        return {Create::conflict, entry};
      }
      return {entry->phase == Phase::ready      ? Create::replay_ready
              : entry->phase == Phase::starting ? Create::pending
                                                : Create::conflict,
              entry};
    }
    if (resources_.count(resource)) {
      return {Create::conflict, {}};
    }
    // Tombstones are deliberately not evicted: old idempotency keys must never
    // become a new launch. Exhaustion fails closed rather than growing forever.
    if (entries_.size() >= capacity_ || id.size() > 128 || resource.size() > 4096 || fingerprint.size() > 65536) {
      return {Create::exhausted, {}};
    }
    auto entry = std::make_shared<Entry>(std::move(id), std::move(resource), std::move(fingerprint));
    entries_.emplace(entry->id, entry);
    resources_.emplace(entry->resource, entry->id);
    return {Create::admitted, entry};
  }

  void begin_setup(const Ticket &entry) {
    std::lock_guard lock(mutex_);
    entry->setup_complete = false;
  }

  void finish_setup(const Ticket &entry) {
    std::lock_guard lock(mutex_);
    entry->setup_complete = true;
    close_if_finished(entry);
  }

  bool setup_complete(const Ticket &entry) const {
    std::lock_guard lock(mutex_);
    return entry->setup_complete;
  }

  // Internal completion notifications must not reissue a mutating Docker stop.
  // Public StopLobby requests still reach the runner's request callback directly.
  bool request_stop_once(const Ticket &entry) {
    std::lock_guard lock(mutex_);
    if (entry->stop_notified || entry->phase == Phase::closed) {
      return false;
    }
    entry->stop_notified = true;
    entry->phase = Phase::stopping;
    return true;
  }

  bool claim_forget(const Ticket &entry) {
    std::lock_guard lock(mutex_);
    if (entry->phase != Phase::closed || entry->forget_claimed) {
      return false;
    }
    entry->forget_claimed = true;
    return true;
  }

  void retry_forget(const Ticket &entry) {
    std::lock_guard lock(mutex_);
    entry->forget_claimed = false;
  }

  bool launch(const Ticket &entry) {
    std::lock_guard lock(mutex_);
    if (entry->phase != Phase::starting || entry->launched || entry->video_done) {
      return false;
    }
    entry->launched = true;
    return true;
  }

  bool ready(const Ticket &entry) {
    std::lock_guard lock(mutex_);
    if (entry->phase != Phase::starting || !entry->launched) {
      return false;
    }
    entry->phase = Phase::ready;
    return true;
  }

  void quarantine(const Ticket &entry) {
    std::lock_guard lock(mutex_);
    if (entry->phase != Phase::closed) {
      entry->phase = Phase::unknown;
    }
  }

  bool stopping(const Ticket &entry) const {
    std::lock_guard lock(mutex_);
    return entry->phase == Phase::stopping || entry->phase == Phase::closed;
  }

  Ticket find(const std::string &id) const {
    std::lock_guard lock(mutex_);
    auto it = entries_.find(id);
    return it == entries_.end() ? Ticket{} : it->second;
  }

  bool add_audio_producer(const Ticket &entry) {
    std::lock_guard lock(mutex_);
    if (entry->phase != Phase::starting || entry->audio_reserved) {
      return false;
    }
    entry->audio_reserved = true;
    ++entry->producers;
    return true;
  }

  void producer_finished(const Ticket &entry, Producer producer = Producer::video) {
    std::lock_guard lock(mutex_);
    auto &done = producer == Producer::video ? entry->video_done : entry->audio_done;
    if (done || (producer == Producer::audio && !entry->audio_reserved)) {
      return;
    }
    done = true;
    --entry->producers;
    close_if_finished(entry);
  }

  void runner_finished(const Ticket &entry) {
    std::lock_guard lock(mutex_);
    entry->runner_done = true;
    close_if_finished(entry);
  }

  // Stop may arrive during an admitted operation. Revoke further admission now;
  // defer membership cleanup until that operation has left its critical phase.
  bool stop(const Ticket &entry) {
    std::lock_guard lock(mutex_);
    entry->stop_notified = true;
    if (entry->phase != Phase::closed) {
      entry->phase = Phase::stopping;
    }
    if (entry->operation || entry->cleaning || entry->cleaned) {
      return false;
    }
    entry->cleaning = true;
    return true;
  }

  void retry_stop(const Ticket &entry) {
    std::lock_guard lock(mutex_);
    entry->cleaning = false;
  }

  void stopped(const Ticket &entry) {
    std::lock_guard lock(mutex_);
    entry->cleaning = false;
    entry->cleaned = true;
    close_if_finished(entry);
  }

  Join join(const Ticket &entry, std::size_t session, bool multi_user) {
    std::lock_guard lock(mutex_);
    if (entry->phase != Phase::ready || entry->operation || entry->cleaning) {
      return Join::unavailable;
    }
    if (entry->members.count(session)) {
      return Join::already_joined;
    }
    if (session_owners_.count(session)) {
      return Join::session_busy;
    }
    if (!multi_user && !entry->members.empty()) {
      return Join::full;
    }
    entry->operation = true;
    entry->members.insert(session);
    session_owners_.emplace(session, entry->id);
    return Join::admitted;
  }

  bool leave(const Ticket &entry, const ConnectionScope &scope) {
    if (!scope)
      return false;
    const auto session = scope.id();
    std::lock_guard lock(mutex_);
    if (!entry->members.count(session)) {
      return false;
    }
    if (entry->operation) {
      entry->deferred_leaves.insert_or_assign(session, scope);
      return false;
    }
    entry->operation = true;
    return true;
  }

  Deferred finish(const Ticket &entry, std::optional<std::size_t> leaving = {}) {
    std::lock_guard lock(mutex_);
    if (leaving) {
      entry->members.erase(*leaving);
      session_owners_.erase(*leaving);
    }
    entry->operation = false;
    Deferred deferred{.stop = entry->phase == Phase::stopping && !entry->cleaning && !entry->cleaned};
    for (const auto &[id, scope] : entry->deferred_leaves)
      deferred.leaves.push_back(scope);
    entry->deferred_leaves.clear();
    close_if_finished(entry);
    return deferred;
  }

private:
  void close_if_finished(const Ticket &entry) {
    if (entry->phase != Phase::closed && entry->producers == 0 && (!entry->launched || entry->runner_done) &&
        entry->cleaned && entry->setup_complete && !entry->operation && entry->members.empty()) {
      entry->phase = Phase::closed;
      resources_.erase(entry->resource);
      // Closed keys remain as small tombstones; release request/config payloads.
      std::string{}.swap(entry->resource);
      std::string{}.swap(entry->fingerprint);
    }
  }

  const std::size_t capacity_;
  mutable std::mutex mutex_;
  std::map<std::string, Ticket> entries_;
  std::map<std::string, std::string> resources_;
  std::map<std::size_t, std::string> session_owners_;
};

// The actual StopLobby handler policy, shared with the callback fixtures. A
// cleaned lobby remains addressable until its producers AND runner are terminal.
template <typename Cleanup, typename Forget>
void handle_lobby_stop(LobbyLifecycle &lifecycle,
                       const LobbyLifecycle::Ticket &ticket,
                       Cleanup &&cleanup_membership,
                       Forget &&forget_record) {
  if (lifecycle.stop(ticket)) {
    if (cleanup_membership()) {
      lifecycle.stopped(ticket);
    } else {
      lifecycle.retry_stop(ticket);
    }
  }
  if (lifecycle.claim_forget(ticket)) {
    try {
      forget_record();
    } catch (...) {
      lifecycle.retry_forget(ticket);
      throw;
    }
  }
}

// Producer/deferred completion is distinct from a new explicit stop request.
template <typename EmitStop, typename Cleanup>
void notify_lobby_completion(LobbyLifecycle &lifecycle,
                             const LobbyLifecycle::Ticket &ticket,
                             EmitStop &&emit_stop,
                             Cleanup &&cleanup) {
  if (lifecycle.request_stop_once(ticket)) {
    emit_stop();
  } else {
    cleanup();
  }
}

// Factory must either return an actual worker or throw before creating one.
// Successful construction transfers the reservation BEFORE detach; a detach
// failure must never be treated as proof that the worker was not started.
class ProducerHandoffs {
public:
  ProducerHandoffs(LobbyLifecycle &lifecycle, LobbyLifecycle::Ticket ticket)
      : lifecycle_(lifecycle), ticket_(std::move(ticket)) {}
  ProducerHandoffs(const ProducerHandoffs &) = delete;
  ProducerHandoffs &operator=(const ProducerHandoffs &) = delete;

  template <typename Factory, typename Work, typename Completion>
  bool audio(Factory &&factory, Work work, Completion completion) {
    if (!lifecycle_.add_audio_producer(ticket_)) {
      return false;
    }
    start(LobbyLifecycle::Producer::audio,
          audio_handed_off_,
          std::forward<Factory>(factory),
          std::move(work),
          std::move(completion));
    return true;
  }

  template <typename Factory, typename Work, typename Completion>
  bool video(Factory &&factory, Work work, Completion completion) {
    if (lifecycle_.stopping(ticket_)) {
      return false;
    }
    start(LobbyLifecycle::Producer::video,
          video_handed_off_,
          std::forward<Factory>(factory),
          std::move(work),
          std::move(completion));
    return true;
  }

  void cancel_unstarted() {
    if (!audio_handed_off_) {
      lifecycle_.producer_finished(ticket_, LobbyLifecycle::Producer::audio);
    }
    if (!video_handed_off_) {
      lifecycle_.producer_finished(ticket_, LobbyLifecycle::Producer::video);
    }
  }

private:
  template <typename Factory, typename Work, typename Completion>
  void start(LobbyLifecycle::Producer producer, bool &handed_off, Factory &&factory, Work work, Completion completion) {
    auto worker = factory([lifecycle = &lifecycle_,
                           ticket = ticket_,
                           producer,
                           work = std::move(work),
                           completion = std::move(completion)]() mutable {
      try {
        work();
      } catch (...) {
        lifecycle->producer_finished(ticket, producer);
        completion();
        throw;
      }
      lifecycle->producer_finished(ticket, producer);
      completion();
    });
    handed_off = true;
    worker.detach();
  }

  LobbyLifecycle &lifecycle_;
  LobbyLifecycle::Ticket ticket_;
  bool audio_handed_off_ = false;
  bool video_handed_off_ = false;
};

// This seam is used by the production create handler, not a test copy of its
// counters. Both successful and failed setup settle exactly the uncreated workers.
template <typename Setup, typename Finalize>
void run_lobby_startup(LobbyLifecycle &lifecycle,
                       const LobbyLifecycle::Ticket &ticket,
                       Setup &&setup,
                       Finalize &&finalize) {
  lifecycle.begin_setup(ticket);
  ProducerHandoffs handoffs(lifecycle, ticket);
  try {
    setup(handoffs);
  } catch (...) {
    handoffs.cancel_unstarted();
    lifecycle.finish_setup(ticket);
    finalize();
    throw;
  }
  handoffs.cancel_unstarted();
  lifecycle.finish_setup(ticket);
  finalize();
}

} // namespace wolf::core::sessions
