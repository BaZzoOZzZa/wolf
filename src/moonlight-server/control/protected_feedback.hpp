#pragma once
#ifdef __linux__
#include "input_seat.hpp"
#include <atomic>
#include <condition_variable>
#include <enet/enet.h>
#include <moonlight/control_crypto.hpp>
#include <sessions/input_handoff.hpp>

namespace wolf::core::events {
struct StreamSession;
}
namespace control {
struct ControlPeer;
}
namespace control::protected_feedback {
// Internal trusted owner view, not a TLS/GameACL proof. Referents must be stable
// through the owner call; no copy/raw peer export into an asynchronous queue.
struct CurrentOwnerSource {
  std::shared_ptr<void> owner;
  // Fresh owner-map/session observation, not historical snapshots or wire bool.
  // Trusted nonblocking owner-only predicate; no reentry/transport operations.
  bool (*matches)(void *,
                  ENetPeer &,
                  const PeerToken &,
                  const std::shared_ptr<ControlPeer> &,
                  const wolf::core::events::StreamSession &,
                  moonlight::control::ControlKeyState &) = nullptr;
};
class OwnerPeerView {
  friend struct State;
  friend class ProtectedFeedbackExecutor;
  ENetPeer &raw_;
  const std::thread::id thread_ = std::this_thread::get_id();
  const wolf::core::events::StreamSession &associated_, &current_;
  const PeerToken token_;
  const std::shared_ptr<ControlPeer> sender_, current_sender_;
  const std::shared_ptr<moonlight::control::ControlKeyRegistry> registry_;
  moonlight::control::ControlKeyState &key_;
  const GrantHandle grant_;
  const DeviceGeneration device_;
  const CurrentOwnerSource current_source_;

public:
  OwnerPeerView(ENetPeer &,
                const wolf::core::events::StreamSession &associated,
                const wolf::core::events::StreamSession &current,
                PeerToken,
                std::shared_ptr<ControlPeer> sender,
                std::shared_ptr<ControlPeer> current_sender,
                std::shared_ptr<moonlight::control::ControlKeyRegistry>,
                moonlight::control::ControlKeyState &,
                GrantHandle,
                DeviceGeneration,
                CurrentOwnerSource);
  OwnerPeerView(const OwnerPeerView &) = delete;
};
// Immutable table after publication. Context is owned, not a borrowed callback
// payload. create throws only before effect or after cleaning unpublished data;
// send/destroy/reset/service throws may have uncertain ownership (retain Unknown).
struct EnetOperations {
  std::shared_ptr<void> owner;
  ENetPacket *(*create)(void *, const void *, size_t, enet_uint32) = nullptr;
  int (*send)(void *, ENetPeer *, enet_uint8, ENetPacket *) = nullptr;
  void (*destroy)(void *, ENetPacket *) = nullptr;
  void (*reset_peer)(void *, ENetPeer *) = nullptr;
  void (*service)(void *) = nullptr;                           // trusted enclosing operation, no public completion ack
  static EnetOperations defaults(std::shared_ptr<void> owner); // no ENet initialize/host/socket
};
enum class Phase {
  Idle,
  Reserved,
  CallerOwns,
  EnetOwns,
  ReleaseSeen,
  Cleaning,
  Retired,
  Unknown
};
enum class Outcome {
  None,
  SentReleased,
  CancelledReleased,
  FailedReleased,
  Unknown
};
enum class PumpResult {
  Idle,
  Queued,
  Refused,
  Failed,
  Unknown
};
enum class HookPoint {
  BeforeReserve,
  AfterReserve,
  BeforeEncrypt,
  AfterEncrypt,
  BeforeSend,
  AfterSend,
  BeforePayloadRelease,
  AfterPayloadRelease
};
using AttemptHook = std::function<void(HookPoint)>; // default empty, INTERNAL deterministic tests only
struct Policy {
  size_t logical_bytes = 64 * 1024;
  size_t call_depth = 8;
};
struct Status {
  Phase phase;
  Outcome outcome;
  bool occupied, packet_live, cipher_live, sealed, closing;
  size_t active_calls, charged_bytes;
};
struct State;
class FeedbackDomain;
class ProtectedFeedbackExecutor {
  friend class FeedbackDomain;
  std::shared_ptr<State> state_;
  explicit ProtectedFeedbackExecutor(std::shared_ptr<State>);

public:
  bool bind_current_thread(); // once; no new thread, cannot rebind after owner loss
  // Unreserved failure has only the attempted exact grant's authority. Stale
  // frame refusal cannot fault a replacement or seal its shared executor.
  PumpResult pump_one(const OwnerPeerView &, AttemptHook = {});
  bool cancel_for_transfer(const OwnerPeerView &, std::shared_ptr<const ::sessions::TransferHandle>);
  // Failure can fault only the retained active operation. Without one, false is
  // a local service failure, not global shutdown; lifecycle owner uses request_close.
  bool service(); // owner-only wrapped operation, supports nested frames
  Status status() const;
  void request_close(); // no IO/wait; exact current seat revoke, never healthy cancellation
};
// Lifecycle/shutdown owner != ENet execution owner. This retained domain owns
// callback state independently of facades. Destructor waits outside all seat/
// queue/control locks for actual calls/packets/payload retirement. May wait forever
// on Unknown; forbidden on a callback/active execution stack. Passive waiting is
// NOT progress: owner must reset/drain first (future3B2), then join/domain release.
// No cycle, detach, force-free, global reaper or replacement slot.
class FeedbackDomain {
  std::mutex mutex_;
  bool reserved_ = false;
  std::shared_ptr<State> state_;

public:
  FeedbackDomain() = default;
  FeedbackDomain(const FeedbackDomain &) = delete;
  ~FeedbackDomain();
  std::optional<ProtectedFeedbackExecutor> create(std::shared_ptr<InputSeat>, EnetOperations, Policy = {});
  static size_t logical_charge(); // actual sizeof bounded storage, excludes library allocator overhead
};
} // namespace control::protected_feedback
#endif
