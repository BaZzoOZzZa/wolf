#pragma once
#include <array>
#include <atomic>
#include <state/data-structures.hpp>
#include <streaming/consumer_runtime.hpp>

namespace sessions {
class ProtectedRuntimeOwner;
}
namespace wolf::core::sessions {
// Transport lifetime only, not permission. Snapshots own identities, never callbacks.
class ConsumerComposition {
  friend class ::sessions::ProtectedRuntimeOwner;
  struct Lifetimes {
    std::array<streaming::ConnectionScope, 2> scopes;
  };
  struct View {
    std::atomic<std::shared_ptr<const Lifetimes>> snapshot;
    std::atomic_bool reserved{false};
  };
  std::shared_ptr<View> view_;
  std::shared_ptr<streaming::ConsumerRuntime> runtime_;

public:
  explicit ConsumerComposition(state::SessionsAtoms, streaming::ConsumerPolicy = {});
  const std::shared_ptr<streaming::ConsumerRuntime> &runtime() const {
    return runtime_;
  }
  bool contains(const streaming::ConnectionScope &) const;
};
} // namespace wolf::core::sessions

#ifdef __linux__
#include <sessions/input_handoff.hpp>
#include <sessions/protected_access.hpp>
#include <sessions/protected_authority.hpp>
namespace sessions {
using ConsumerComposition = wolf::core::sessions::ConsumerComposition;
struct RuntimeJobTag;
using RuntimeJobIdentity = wolf::core::sessions::Scope<unsigned, RuntimeJobTag>;
enum class RuntimeCell {
  Preparation,
  Audio,
  Video,
  Retirement
};
enum class RuntimeSubmission {
  Accepted,
  Busy,
  Capacity,
  Closed,
  Invalid
};
enum class RuntimeJobState {
  Empty,
  Queued,
  Executing,
  Complete,
  Refused,
  Unknown
};
enum class RuntimeJoin {
  Refused,
  Unknown,
  Joined
};
struct RuntimeReceipt {
  RuntimeJobIdentity identity;
  std::atomic<RuntimeJobState> state{RuntimeJobState::Queued};
  std::atomic<streaming::ConsumerAdmission> media{streaming::ConsumerAdmission::Stale};
};
// Owned trusted data, created off ingress. No opaque callback copies on submission.
struct RuntimeJob {
  RuntimeCell cell = RuntimeCell::Preparation;
  std::vector<std::byte> payload;
  TransferToken transfer;
  AuthorizedSelection selection;
  std::shared_ptr<const TransferHandle> handle;
  control::RunScope producer;
  streaming::ConsumerCorrelation correlation;
  streaming::ConsumerRuntime::Work work;
  std::function<void()> retire;
  std::function<void()> before_cleanup; // internal deterministic F12 test seam
};
struct RuntimeSubmitResult {
  RuntimeSubmission result;
  std::shared_ptr<const RuntimeReceipt> receipt;
};
struct RuntimeOwnerStatus {
  bool started, stop, unknown, joined;
  size_t occupied, bytes;
};
struct RuntimeBootstrap {
  protected_authority::PreparedFacts facts;
  protected_authority::OperatorCapability op;
  control::SeatCandidate initial;
  protected_access::Principal principal;
  control::ProtectedInventory inventory;
  control::InputSeat::Factory backend;
  control::SeatValidation validate;
  std::function<InputHandoff::Clock::time_point()> now = [] { return InputHandoff::Clock::now(); };
};
// Internal deterministic scheduling only. Retained once until lifecycle teardown;
// no per-call ownership/capture release, no strong owner back-reference.
struct RuntimeInspectionProbe {
  virtual ~RuntimeInspectionProbe() = default;
  virtual void after_submit_publish() const noexcept {}
  virtual void after_status_inspection() const noexcept {}
};
class ProtectedRuntimeOwner {
  struct State;
  std::unique_ptr<State> state_;
  std::thread worker_;
  std::shared_future<void> exited_;
  bool attempted_ = false; // lifecycle owner only
  std::atomic_bool joined_{false};
  void run();

public:
  explicit ProtectedRuntimeOwner(std::shared_ptr<ConsumerComposition>,
                                 std::shared_ptr<const RuntimeInspectionProbe> = {});
  ~ProtectedRuntimeOwner(); // lifecycle only, never worker/ingress; may wait indefinitely
  ProtectedRuntimeOwner(const ProtectedRuntimeOwner &) = delete;
  // Lifecycle-only start once, no fallback legacy factory. Factory must not escape on throw.
  bool start(RuntimeBootstrap, control::SeatThreadFactory = control::start_seat_thread);
  RuntimeSubmitResult submit(std::unique_ptr<RuntimeJob>);
  RuntimeOwnerStatus status() const;
  std::shared_ptr<const TransferHandle> transfer() const; // historical diagnostic, not authority
  control::GrantHandle current() const;
  protected_authority::PermitHandle capture(const control::SeatCandidate &, const protected_access::Principal &) const;
  bool close_exact(const control::ConnectionScope &, const control::PeerToken &);
  bool close_exact(const protected_authority::OperatorCapability &, const protected_authority::FactHandle &);
  void request_stop();
  RuntimeJoin join_when_exited(); // lifecycle owner, actual exit/TLS then actual join
  const std::shared_ptr<streaming::ConsumerRuntime> &runtime() const;
};
} // namespace sessions
#endif
