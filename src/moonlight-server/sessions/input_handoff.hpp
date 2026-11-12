#pragma once
#ifdef __linux__
#include <control/input_seat.hpp>
#include <streaming/consumer_runtime.hpp>
namespace sessions {
struct TransferIdentity;
using TransferToken = wolf::core::sessions::Scope<size_t, TransferIdentity>;
enum class TransferStatus {
  Pending,
  Granted,
  Quarantined
};
// Payload-free historical observation: this token retains only its empty scope
// allocation identity, never TransferHandle, validator, facts, Work or ticket.
struct TransferObservation {
  TransferToken identity;
  TransferStatus status;
};
enum class TransferAdmission {
  Accepted,
  Duplicate,
  Busy,
  Invalid,
  Quarantined
};
// Constructible only by trusted internal orchestration. This is not evidence of
// pairing/PIN/first-input permission; Stage3 must supply real GameACL validation.
struct AuthorizedSelection {
  control::GrantHandle expected_old;
  control::SeatCandidate candidate;
  control::SeatValidation validate;
  std::shared_ptr<const protected_authority::Permit> authority;
};
class TransferHandle {
  friend class InputHandoff;
  std::shared_ptr<control::seat_detail::Gate> gate_;
  std::optional<streaming::ConsumerDrainTicket> ticket_;
  std::atomic<TransferStatus> status_{TransferStatus::Pending};
  control::SeatValidation validate_;
  std::shared_ptr<const protected_authority::Permit> authority_;
  TransferHandle(std::shared_ptr<control::seat_detail::Gate>,
                 TransferToken,
                 AuthorizedSelection,
                 std::chrono::steady_clock::time_point);

public:
  const TransferToken identity;
  const control::GrantHandle old_grant, prepared_grant;
  const std::chrono::steady_clock::time_point started;
  TransferStatus status() const {
    return status_.load();
  }
  std::optional<streaming::ConsumerDrainTicket> saved_ticket() const;
};
struct TransferResult {
  TransferAdmission admission;
  std::shared_ptr<const TransferHandle> handle;
};
class InputHandoff {
  std::shared_ptr<control::InputSeat> seat_;
  std::shared_ptr<streaming::ConsumerRuntime> consumers_;
  std::function<std::chrono::steady_clock::time_point()> now_;
  std::shared_ptr<TransferHandle> pending_;
  std::optional<streaming::ConsumerDrainTicket> quarantine_ticket_;
  bool quarantine_revoked_ = false;
  std::atomic_bool closing_{false};
  std::thread supervisor_;
  void quarantine_locked();

public:
  using Clock = std::chrono::steady_clock;
  InputHandoff(
      std::shared_ptr<control::InputSeat>,
      std::shared_ptr<streaming::ConsumerRuntime>,
      std::function<Clock::time_point()> now = [] { return Clock::now(); },
      control::SeatThreadFactory start = control::start_seat_thread);
  ~InputHandoff(); // owner only, never event/transport callback
  TransferResult request(TransferToken, AuthorizedSelection);
  void poll(); // May invoke the trusted nonblocking validator outside gate; blocked validators delay this caller.
  // Deadline-only decision, no validator/Runtime/IO. The existing clock must be
  // nonblocking and nonreentrant; clock exceptions quarantine the exact Pending.
  // Progress requires a caller/tick; this is not an autonomous timer guarantee.
  void poll_deadline();
  std::optional<TransferObservation> observe() const; // same Gate; no callbacks/IO/authority effect
  bool candidate_gone(const control::ConnectionScope &, const control::PeerToken &);
  // Inspection only; execution must use admit_prepared under the serial gate.
  bool prepared_admission(const std::shared_ptr<const TransferHandle> &,
                          const control::ConnectionScope &,
                          const control::PeerToken &) const;
  // before_cleanup is an optional INTERNAL test synchronization hook: called
  // after actual Runtime result + gate unlock, before temporary owners retire.
  // Throwing quarantines/refuses. The hook is not a completion acknowledgement.
  streaming::ConsumerAdmission admit_prepared(const std::shared_ptr<const TransferHandle> &,
                                              const control::ConnectionScope &,
                                              const control::PeerToken &,
                                              streaming::ConsumerKind,
                                              streaming::ConsumerCorrelation,
                                              streaming::ConsumerRuntime::Work,
                                              std::function<void()> before_cleanup = {});
  // Actual commit predicate; invalid/broken future never constitutes completion.
  static bool drain_complete(const std::optional<streaming::ConsumerDrainTicket> &, const control::ConnectionScope &);
};
} // namespace sessions
#endif
