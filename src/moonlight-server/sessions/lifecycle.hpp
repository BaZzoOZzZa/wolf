#pragma once

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
    std::set<std::size_t> members;
    std::set<std::size_t> deferred_leaves;

  public:
    Entry(std::string id, std::string resource, std::string fingerprint)
        : id(std::move(id)), resource(std::move(resource)), fingerprint(std::move(fingerprint)) {}
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
    std::vector<std::size_t> leaves;
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

  bool leave(const Ticket &entry, std::size_t session) {
    std::lock_guard lock(mutex_);
    if (!entry->members.count(session)) {
      return false;
    }
    if (entry->operation) {
      entry->deferred_leaves.insert(session);
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
    Deferred deferred{.stop = entry->phase == Phase::stopping && !entry->cleaning && !entry->cleaned,
                      .leaves = {entry->deferred_leaves.begin(), entry->deferred_leaves.end()}};
    entry->deferred_leaves.clear();
    close_if_finished(entry);
    return deferred;
  }

private:
  void close_if_finished(const Ticket &entry) {
    if (entry->phase != Phase::closed && entry->producers == 0 && (!entry->launched || entry->runner_done) &&
        entry->cleaned && !entry->operation && entry->members.empty()) {
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

} // namespace wolf::core::sessions
