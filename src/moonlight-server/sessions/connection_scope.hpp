#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace wolf::core::sessions {

// In-process capabilities, never reconstructed from wire secrets or stable IDs.
// Every delayed operation retains a strong reference: an allocator cannot reuse
// its identity while any event/worker from that incarnation still exists.
template <typename Id, typename Tag> class Scope {
  struct Epoch {};
  Id id_{};
  std::shared_ptr<const Epoch> epoch_;

  Scope(Id id, std::shared_ptr<const Epoch> epoch) : id_(std::move(id)), epoch_(std::move(epoch)) {}

public:
  Scope() = default; // Deserialised/legacy requests have no authority.
  static Scope fresh(Id id) {
    return Scope(std::move(id), std::make_shared<const Epoch>());
  }
  const Id &id() const {
    return id_;
  }
  explicit operator bool() const {
    return static_cast<bool>(epoch_);
  }
  // Opaque process-local resource name, never put in client/API messages.
  // Retention provides the same anti-ABA lifetime as matches().
  std::string resource_key() const {
    return epoch_ ? "wolf_scope_" + std::to_string(reinterpret_cast<std::uintptr_t>(epoch_.get())) : std::string{};
  }
  bool matches(const Scope &other) const {
    return epoch_ && other.epoch_ && id_ == other.id_ && epoch_ == other.epoch_;
  }
};

struct RunTag;
struct ConnectionTag;
using RunScope = Scope<std::string, RunTag>;
using ConnectionScope = Scope<std::size_t, ConnectionTag>;

// Shared by the real event adapters and isolated regression fixtures. Even two
// null handles fail; an ID is only routing metadata, never wildcard authority.
template <typename ScopeType, typename Id>
bool targets(const ScopeType &owned, const ScopeType &requested, const Id &id) {
  return owned.matches(requested) && owned.id() == id;
}

// Resolve only the captured producer instance. The stable name is diagnostic;
// a delayed switch must never discover a new sink using that name alone.
inline std::optional<std::string> resolve_producer(const ConnectionScope &consumer,
                                                   const ConnectionScope &requested,
                                                   std::size_t session_id,
                                                   const RunScope &producer,
                                                   std::string_view producer_id) {
  if (!targets(consumer, requested, session_id) || !producer || producer.id() != producer_id)
    return {};
  return producer.resource_key();
}

// A listener must retain its selected producer even after the Switch event and
// producer worker disappear. Otherwise allocator reuse could alias its name.
// Only switching this binding is serialized here, not full pipeline lifecycle.
class ProducerBinding {
  std::mutex mutex_;
  RunScope producer_;

public:
  explicit ProducerBinding(RunScope producer) : producer_(std::move(producer)) {}
  template <typename Apply> bool select(const RunScope &producer, Apply &&apply) {
    if (!producer)
      return false;
    std::lock_guard lock(mutex_);
    // Copy before the side effect: a failed ID allocation cannot lose the new
    // identity after the property has already changed. Publication is a move.
    auto retained = producer;
    // Both old and requested scopes stay alive throughout the property change.
    if (!apply(retained.resource_key()))
      return false;
    producer_ = std::move(retained);
    return true;
  }
};

} // namespace wolf::core::sessions
