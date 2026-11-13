#pragma once
#ifdef __linux__
#include "checked_xbox_backend.hpp"
#include <array>
#include <condition_variable>
#include <deque>
#include <sessions/connection_scope.hpp>
#include <vector>

namespace sessions {
class InputHandoff;
namespace protected_authority {
class Cell;
class ProtectedAuthorityDomain;
class Permit;
} // namespace protected_authority
} // namespace sessions
namespace control {
using wolf::core::sessions::ConnectionScope;
using wolf::core::sessions::RunScope;
struct PeerIdentity;
struct BindingIdentity;
struct GrantIdentity;
using PeerToken = wolf::core::sessions::Scope<size_t, PeerIdentity>;
using BindingRevision = wolf::core::sessions::Scope<size_t, BindingIdentity>;
using GrantToken = wolf::core::sessions::Scope<size_t, GrantIdentity>;
struct SeatCandidate {
  RunScope source, destination;
  ConnectionScope connection;
  PeerToken peer;
  BindingRevision binding;
  uint8_t slot = 0;
  bool valid() const;
};
struct SeatGrant {
  const SeatCandidate owner;
  const GrantToken token = GrantToken::fresh(0);
  explicit SeatGrant(SeatCandidate candidate) : owner(std::move(candidate)) {}
};
using GrantHandle = std::shared_ptr<const SeatGrant>;
// Trusted internal caller only: must revalidate exact retained identities and
// current auth/lifecycle/binding. Nonblocking, no reentry into the seat gate.
using SeatValidation = std::function<bool(const SeatCandidate &)>;
// Trusted all-or-nothing launch seam: throwing means no thread escaped.
using SeatThreadFactory = inputtino::checked::detail::ThreadFactory;
inline std::thread start_seat_thread(std::function<void()> work) {
  return std::thread(std::move(work));
}
struct ProtectedInventory {
  bool fresh_pad_only = false;
  bool exclusive_routes = false;
  bool no_nonpad_devices_or_pending_commands = false;
  bool valid() const {
    return fresh_pad_only && exclusive_routes && no_nonpad_devices_or_pending_commands;
  }
};
enum class InputKind {
  Pad,
  Keyboard,
  Mouse,
  Text,
  Touch,
  Pen,
  PadExtension
};
struct SeatCommand {
  GrantHandle grant;
  InputKind kind = InputKind::Pad;
  uint8_t slot = 0;
  inputtino::checked::Frame frame;
  std::vector<std::byte> packet; // owned ingress bytes, capacity charged to budget
};
enum class SeatAdmission {
  Accepted,
  Stale,
  Denied,
  Overflow,
  Quarantined
};
struct SeatPolicy {
  size_t packets = 256, bytes = 64 * 1024;
};
struct SeatFeedback {
  GrantHandle grant;
  DeviceGeneration device;
  int low = 0, high = 0;
};
namespace seat_detail {
struct Gate {
  std::mutex mutex;
  std::condition_variable wake;
  RunScope destination;
  DeviceGeneration device;
  GrantHandle current;
  SeatValidation initial_validation;
  std::shared_ptr<sessions::protected_authority::Cell> authority; // installed before protected backend/worker start
  SeatPolicy policy;
  bool coordinator_reserved = false;
  bool open = false, bootstrap = true, quarantined = false, shutdown = false;
  bool writing = false, neutral_requested = true, neutral_running = false, neutral_done = false;
  size_t sends = 0, count = 0, bytes = 0, inflight_bytes = 0, head = 0;
  size_t feedback_count = 0, feedback_head = 0;
  // One bounded admission-call cleanup obligation, independent of Runtime join.
  // close() must preserve it; only the actual outside-gate owner retirement acks.
  bool admission_cleanup_pending = false;
  GrantHandle admission_cleanup_owner;
  std::array<std::optional<SeatCommand>, 256> queue;
  std::array<std::optional<SeatFeedback>, 256> feedback;
  std::optional<inputtino::checked::WriteReceipt> neutral;
  void close(bool fault); // gate held, reserved lane never allocates
  bool matches(const GrantHandle &) const;
};
} // namespace seat_detail
// Executor must keep this handle until the real send/callback retires. Dropping
// an unacknowledged reservation leaves the fence closed (Unknown), never success.
class FeedbackReservation {
  std::shared_ptr<seat_detail::Gate> gate_;
  bool retired_ = false;
  friend class SeatFeedbackRouter;
  FeedbackReservation(std::shared_ptr<seat_detail::Gate> gate, SeatFeedback value);

public:
  const SeatFeedback feedback;
  void retire(bool success = true);
  FeedbackReservation(const FeedbackReservation &) = delete;
};
class SeatFeedbackRouter {
  std::shared_ptr<seat_detail::Gate> gate_;
  std::shared_ptr<FeedbackReservation> reserve_send_locked();
  friend class InputSeat;
  explicit SeatFeedbackRouter(std::shared_ptr<seat_detail::Gate> gate) : gate_(std::move(gate)) {}

public:
  std::optional<SeatFeedback> capture(int low, int high);
  bool enqueue(SeatFeedback);
  void intake(int low, int high) noexcept;
  std::shared_ptr<FeedbackReservation> reserve_send();
  // Exact retained grant checked under the same gate BEFORE consuming a record.
  // Null/stale expected never acts as wildcard or revokes the current grant.
  std::shared_ptr<FeedbackReservation> reserve_send(const GrantHandle &expected);
};
class InputSeat {
  friend class sessions::InputHandoff;
  friend class sessions::protected_authority::ProtectedAuthorityDomain;
  std::shared_ptr<seat_detail::Gate> gate_;
  std::shared_ptr<SeatFeedbackRouter> router_;
  std::shared_ptr<CheckedXboxBackend> backend_;
  std::thread worker_;
  static void work(const std::shared_ptr<seat_detail::Gate> &, const std::shared_ptr<CheckedXboxBackend> &);
  InputSeat() = default;

public:
  using Factory = std::function<std::shared_ptr<CheckedXboxBackend>(inputtino::checked::Xbox::Feedback)>;
  ~InputSeat(); // owner shutdown only, actual join may block; retain on Unknown
  InputSeat(const InputSeat &) = delete;

private:
  friend class InputSeatDomain;
  static std::shared_ptr<InputSeat> create(SeatCandidate,
                                           ProtectedInventory,
                                           SeatValidation,
                                           Factory,
                                           SeatPolicy,
                                           SeatThreadFactory,
                                           std::shared_ptr<seat_detail::Gate> prepared_gate = {});

public:
  SeatAdmission submit(SeatCommand);
  bool revoke(const GrantHandle &, const PeerToken &); // exact trusted notification, no raw disconnect
  GrantHandle current() const;
  bool quarantined() const;
  bool neutral_complete() const;
  std::shared_ptr<SeatFeedbackRouter> router() const {
    return router_;
  }
  DeviceGeneration generation() const {
    return backend_->generation();
  }
  const CheckedXboxBackend *backend_identity() const {
    return backend_.get();
  }
};
// One server-owned domain must be shared by all future protected ingress. No
// release/replacement API: Unknown and failed construction consume its sole slot.
class InputSeatDomain {
  std::mutex mutex_;
  bool reserved_ = false;
  std::shared_ptr<InputSeat> seat_;

public:
  std::shared_ptr<InputSeat> create(SeatCandidate,
                                    ProtectedInventory,
                                    SeatValidation,
                                    InputSeat::Factory,
                                    SeatPolicy = {},
                                    SeatThreadFactory = start_seat_thread);
};
} // namespace control
#endif
