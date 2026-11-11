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
};
class TransferHandle {
  friend class InputHandoff;
  std::shared_ptr<control::seat_detail::Gate> gate_;
  std::optional<streaming::ConsumerDrainTicket> ticket_;
  std::atomic<TransferStatus> status_{TransferStatus::Pending};
  control::SeatValidation validate_;
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
  void poll(); // nonblocking timer decision; also deterministic clock seam
  bool candidate_gone(const control::ConnectionScope &, const control::PeerToken &);
  // Inspection only; execution must use admit_prepared under the serial gate.
  bool prepared_admission(const std::shared_ptr<const TransferHandle> &,
                          const control::ConnectionScope &,
                          const control::PeerToken &) const;
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
