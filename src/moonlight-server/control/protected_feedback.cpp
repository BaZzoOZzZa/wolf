#include "protected_feedback.hpp"
#ifdef __linux__
#include "control.hpp"
#include <limits>
#include <utility>

namespace control::protected_feedback {
using namespace moonlight::control;
OwnerPeerView::OwnerPeerView(ENetPeer &peer,
                             const events::StreamSession &associated,
                             const events::StreamSession &current,
                             PeerToken token,
                             std::shared_ptr<ControlPeer> sender,
                             std::shared_ptr<ControlPeer> current_sender,
                             std::shared_ptr<ControlKeyRegistry> registry,
                             ControlKeyState &key,
                             GrantHandle grant,
                             DeviceGeneration device,
                             CurrentOwnerSource source)
    : raw_(peer), associated_(associated), current_(current), token_(std::move(token)), sender_(std::move(sender)),
      current_sender_(std::move(current_sender)), registry_(std::move(registry)), key_(key), grant_(std::move(grant)),
      device_(std::move(device)), current_source_(std::move(source)) {}
EnetOperations EnetOperations::defaults(std::shared_ptr<void> owner) {
  return {
      std::move(owner),
      [](void *, const void *data, size_t size, enet_uint32 flags) { return enet_packet_create(data, size, flags); },
      [](void *, ENetPeer *peer, enet_uint8 channel, ENetPacket *packet) {
        return enet_peer_send(peer, channel, packet);
      },
      [](void *, ENetPacket *packet) { enet_packet_destroy(packet); },
      [](void *, ENetPeer *peer) { enet_peer_reset(peer); },
      nullptr};
}
struct State {
  const std::shared_ptr<InputSeat> seat;
  const std::shared_ptr<SeatFeedbackRouter> router;
  const EnetOperations ops;
  const Policy policy;
  std::mutex lifetime;
  std::condition_variable wake;
  std::thread::id owner;
  // Immutable identity of the reserved operation, protected by lifetime. Foreign
  // threads may snapshot this metadata, NEVER inspect the owner-only Cell.
  GrantHandle active_grant;
  std::atomic_bool closing{false}, sealed{false}, occupied{false}, live{false}, cipher_live{false};
  std::atomic_size_t calls{0};
  std::atomic<Phase> phase{Phase::Idle};
  std::atomic<Outcome> outcome{Outcome::None};
  // Below: only bound owner thread, except marker atomics in packet callback.
  size_t depth = 0;
  bool cleaning = false;
  struct Cell {
    std::shared_ptr<FeedbackReservation> reservation;
    std::unique_ptr<ControlEncryptedPacket> cipher;
    ControlRumblePacket plaintext{};
    AttemptHook hook;
    ENetPacket *packet = nullptr;
    bool caller_owns = false, failed = false, cancelling = false;
    std::atomic_bool release_seen{false}, callback_invalid{false};
    std::atomic_uint32_t flags{0};
  } cell;
  State(std::shared_ptr<InputSeat> s, EnetOperations o, Policy p)
      : seat(std::move(s)), router(seat->router()), ops(std::move(o)), policy(p) {}
  void close() {
    closing = true;
    if (auto grant = seat->current())
      seat->revoke(grant, grant->owner.peer);
  }
  void set_active(GrantHandle grant) {
    {
      std::lock_guard lock(lifetime);
      active_grant.swap(grant);
    } // displaced identity's last release is outside the lock
  }
  void fail_exact(const GrantHandle &grant) {
    // revoke performs the exact identity comparison AND fault transition under
    // the seat gate. A later lookup/current comparison would recreate F13.
    if (!grant || !seat->revoke(grant, grant->owner.peer))
      return;
    sealed = true;
    phase = Phase::Unknown;
    outcome = Outcome::Unknown;
  }
  void fail_active() {
    // Bound owner only. A live reservation fences replacement until retirement.
    if (cell.reservation)
      fail_exact(cell.reservation->feedback.grant);
  }
  static void released(ENetPacket *packet) noexcept {
    auto *cell = static_cast<Cell *>(packet->userData);
    if (!cell)
      return;
    if (cell->packet != packet)
      cell->callback_invalid = true;
    cell->flags = packet->flags;
    cell->release_seen = true; // NOT free(data)/free(packet), stack exit or ack
  }
  bool identities(const OwnerPeerView &v, const GrantHandle &grant) {
    if (v.associated_.aes_key.size() != 32 ||
        !std::all_of(v.associated_.aes_key.begin(), v.associated_.aes_key.end(), [](unsigned char c) {
          return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        }))
      return false;
    return v.thread_ == std::this_thread::get_id() && grant && v.grant_ && grant->token.matches(v.grant_->token) &&
           grant->owner.peer.matches(v.token_) && grant->owner.peer.matches(v.grant_->owner.peer) &&
           grant->owner.connection.matches(v.associated_.connection_scope) &&
           grant->owner.source.matches(v.associated_.run_scope) &&
           grant->owner.destination.matches(v.grant_->owner.destination) &&
           grant->owner.binding.matches(v.grant_->owner.binding) && grant->owner.slot == v.grant_->owner.slot &&
           v.device_.matches(seat->generation()) && v.sender_ && v.sender_ == v.current_sender_ && v.registry_ &&
           v.associated_.control_crypto && same_control_generation(v.associated_, v.current_) &&
           v.associated_.control_crypto->mode.load() == EncryptionMode::v2 && v.current_source_.owner &&
           v.current_source_.matches &&
           v.current_source_
               .matches(v.current_source_.owner.get(), v.raw_, v.token_, v.sender_, v.associated_, v.key_) &&
           v.registry_->find_or_insert(crypto::hex_to_str(v.associated_.aes_key, true)) == &v.key_;
  }
  void hook(HookPoint point) {
    if (cell.hook)
      cell.hook(point);
  }
  void destroy_caller() noexcept {
    if (!cell.packet || !cell.caller_owns)
      return;
    // Mark uncertain BEFORE a potentially throwing injected operation. Never
    // guess a second destroy; real defaults are nonthrowing C functions.
    cell.caller_owns = false;
    try {
      ops.destroy(ops.owner.get(), cell.packet);
    } catch (...) {
      fail_active();
    }
  }
  void retire_ready() noexcept {
    if (!occupied || depth || cleaning)
      return;
    if (live && !cell.release_seen)
      return;
    if (cell.callback_invalid)
      fail_active();
    if (seat->quarantined() && !cell.failed)
      fail_active();
    cleaning = true;
    // Only called after ALL nested actual operations returned. The marker alone
    // was not sufficient: packet/data have now been freed by the actual destroy.
    if (cell.release_seen) {
      live = false;
      cell.packet = nullptr;
    }
    phase = sealed ? Phase::Unknown : Phase::Cleaning;
    try {
      hook(HookPoint::BeforePayloadRelease);
    } catch (...) {
      cell.failed = true;
    }
    cell.cipher.reset();
    cipher_live = false;
    cell.plaintext = {};
    try {
      hook(HookPoint::AfterPayloadRelease);
    } catch (...) {
      cell.failed = true;
    }
    AttemptHook retired;
    retired.swap(cell.hook);
    retired = {}; // actual sole owned capture destruction outside ALL locks
    // Cleanup itself can reenter revoke; never relabel that terminal fault as a
    // successful local release merely because the earlier snapshot was healthy.
    if (seat->quarantined() && !cell.failed)
      fail_active();
    if (cell.reservation) {
      bool success = !sealed && !closing && !cell.failed && cell.release_seen &&
                     (cell.cancelling || (cell.flags.load() & ENET_PACKET_FLAG_SENT));
      if (success)
        outcome = cell.cancelling ? Outcome::CancelledReleased : Outcome::SentReleased;
      else if (!sealed)
        outcome = Outcome::FailedReleased;
      try {
        cell.reservation->retire(success);
      } catch (...) {
        fail_active();
        cleaning = false;
        return;
      }
      cell.reservation.reset(); // only after retire returned and unlocked its gate
      set_active({});
    }
    if (!sealed) {
      phase = Phase::Retired;
      occupied = false;
    } // Unknown domain slot remains occupied even after late actual cleanup.
    cleaning = false;
  }
  struct Frame {
    std::shared_ptr<State> state;
    bool entered = false;
    bool counted = false;
    GrantHandle authority;
    explicit Frame(std::shared_ptr<State> s, GrantHandle attempted = {}, bool from_active = false)
        : state(std::move(s)), authority(std::move(attempted)) {
      bool bad = false;
      GrantHandle snapshot;
      {
        std::lock_guard lock(state->lifetime);
        bad = state->owner != std::this_thread::get_id() || state->owner == std::thread::id{};
        if (from_active)
          snapshot = state->active_grant; // snapshot was empty: no last release under lock
        ++state->calls;
        counted = true;
        if (!bad)
          entered = true;
      }
      if (from_active)
        authority.swap(snapshot); // all prior/snapshot ownership releases outside lock
      if (bad) {
        state->fail_exact(authority);
        return;
      }
      if (state->depth >= state->policy.call_depth) {
        state->fail_exact(authority);
      } else {
        ++state->depth;
        return;
      }
      entered = false;
    }
    ~Frame() {
      if (!counted)
        return;
      if (entered) {
        --state->depth;
        state->retire_ready();
      }
      std::lock_guard lock(state->lifetime);
      --state->calls;
      state->wake.notify_all();
    }
  };
};
ProtectedFeedbackExecutor::ProtectedFeedbackExecutor(std::shared_ptr<State> state) : state_(std::move(state)) {}
bool ProtectedFeedbackExecutor::bind_current_thread() {
  std::lock_guard lock(state_->lifetime);
  if (state_->owner != std::thread::id{} || state_->closing)
    return false;
  state_->owner = std::this_thread::get_id();
  return true;
}
Status ProtectedFeedbackExecutor::status() const {
  return {state_->phase.load(),
          state_->outcome.load(),
          state_->occupied.load(),
          state_->live.load(),
          state_->cipher_live.load(),
          state_->sealed.load(),
          state_->closing.load(),
          state_->calls.load(),
          FeedbackDomain::logical_charge()};
}
void ProtectedFeedbackExecutor::request_close() {
  state_->close();
}
PumpResult ProtectedFeedbackExecutor::pump_one(const OwnerPeerView &view, AttemptHook hook) {
  State::Frame frame(state_, view.grant_); // last local destructor: ack only after outer owners
  AttemptHook owned;
  owned.swap(hook); // empties parameter; moved std::function is not an ownership proof
  auto &s = *state_;
  if (!frame.entered)
    return s.sealed ? PumpResult::Unknown : PumpResult::Refused;
  if (s.sealed)
    return PumpResult::Unknown;
  if (s.closing || s.occupied || s.depth != 1 || s.cleaning)
    return PumpResult::Refused;
  bool reserved_by_attempt = false;
  try {
    auto current = s.seat->current();
    // Preliminary refusal only; scoped reservation below is the atomic gate.
    // Errors never derive authority from this observational current snapshot.
    if (!current || !view.grant_ || !current->token.matches(view.grant_->token))
      return PumpResult::Refused;
    if (owned)
      owned(HookPoint::BeforeReserve);
    s.cell.reservation = s.router->reserve_send(view.grant_);
    if (!s.cell.reservation)
      return PumpResult::Idle;
    reserved_by_attempt = true;
    s.set_active(s.cell.reservation->feedback.grant);
    s.occupied = true;
    s.phase = Phase::Reserved;
    s.outcome = Outcome::None;
    s.cell.failed = s.cell.cancelling = s.cell.caller_owns = false;
    s.cell.release_seen = s.cell.callback_invalid = false;
    s.cell.flags = 0;
    s.cell.packet = nullptr;
    s.cell.hook.swap(owned);
    const auto &feedback = s.cell.reservation->feedback;
    // Validate fresh owner facts only once the exact grant reservation fences
    // replacement. A failed pre-reservation probe must not revoke a later grant.
    if (!s.identities(view, feedback.grant)) {
      s.cell.failed = true;
      return PumpResult::Refused;
    }
    s.hook(HookPoint::AfterReserve);
    if (!s.identities(view, feedback.grant) || !feedback.device.matches(view.device_) ||
        feedback.grant->owner.slot >= 16 || feedback.low < 0 || feedback.low > 65535 || feedback.high < 0 ||
        feedback.high > 65535)
      throw std::runtime_error("Invalid protected feedback");
    auto sequence = view.key_.reserve_sequence(); // same actual authenticated registry, never roll back
    if (!sequence)
      throw std::runtime_error("Control rekey required");
    s.cell.plaintext = {.header = {.type = pkts::RUMBLE_DATA, .length = boost::endian::native_to_little<uint16_t>(10)},
                        .useless = 0,
                        .controller_number = boost::endian::native_to_little<uint16_t>(feedback.grant->owner.slot),
                        .low_freq = boost::endian::native_to_little<uint16_t>(feedback.low),
                        .high_freq = boost::endian::native_to_little<uint16_t>(feedback.high)};
    s.hook(HookPoint::BeforeEncrypt);
    s.cell.cipher = encrypt_packet(view.associated_.aes_key,
                                   *sequence,
                                   {reinterpret_cast<const char *>(&s.cell.plaintext), sizeof(s.cell.plaintext)},
                                   EncryptionMode::v2,
                                   Direction::host);
    s.cipher_live = true;
    s.hook(HookPoint::AfterEncrypt);
    // Immediate nonallocating RAII after transfer-full create. All hooks are later.
    struct PacketOwner {
      State &s;
      ~PacketOwner() {
        s.destroy_caller();
      }
    } packet_owner{s};
    s.cell.packet =
        s.ops.create(s.ops.owner.get(), s.cell.cipher.get(), s.cell.cipher->full_size(), ENET_PACKET_FLAG_RELIABLE);
    if (!s.cell.packet)
      throw std::runtime_error("Packet create failed");
    s.cell.caller_owns = true;
    s.live = true;
    s.phase = Phase::CallerOwns;
    s.cell.packet->userData = &s.cell;
    s.cell.packet->freeCallback = State::released;
    s.hook(HookPoint::BeforeSend);
    // Current generation/sender/token/registry are rechecked, never retagged.
    if (s.closing || s.sealed || !s.identities(view, feedback.grant))
      throw std::runtime_error("Peer changed before send");
    s.cell.caller_owns = false; // unknown until raw send returns a disposition
    int result;
    try {
      result = s.ops.send(s.ops.owner.get(), &view.raw_, 0, s.cell.packet);
    } catch (...) {
      s.fail_active();
      return PumpResult::Unknown;
    }
    if (result < 0) {
      s.cell.caller_owns = true;
      throw std::runtime_error("ENet rejected packet");
    }
    s.phase = Phase::EnetOwns;
    s.hook(HookPoint::AfterSend);
    return PumpResult::Queued;
  } catch (...) {
    if (reserved_by_attempt)
      s.cell.failed = true;
    else
      s.fail_exact(view.grant_); // no Cell access/mutation for an unreserved fault
    return s.sealed ? PumpResult::Unknown : PumpResult::Failed;
  }
}
bool ProtectedFeedbackExecutor::owner_call(void *context, void (*call)(void *)) {
  State::Frame frame(state_, {}, true);
  if (!frame.entered || !call)
    return false;
  try {
    call(context);
    return true;
  } catch (...) {
    state_->fail_exact(frame.authority);
    return false;
  }
}
bool ProtectedFeedbackExecutor::service() {
  return owner_call(state_.get(), state_->ops.service ? +[](void *p) {
    auto &state = *static_cast<State *>(p);
    state.ops.service(state.ops.owner.get());
  } : nullptr);
}
bool ProtectedFeedbackExecutor::cancel_for_transfer(const OwnerPeerView &view,
                                                    std::shared_ptr<const ::sessions::TransferHandle> transfer) {
  State::Frame frame(state_, view.grant_);
  std::shared_ptr<const ::sessions::TransferHandle> retained;
  retained.swap(transfer); // parameter emptied; owned validator refs retire BEFORE frame ack
  auto &s = *state_;
  if (!frame.entered || s.sealed || !s.cell.reservation || !s.live || !retained ||
      retained->status() != ::sessions::TransferStatus::Pending)
    return false;
  try {
    const auto &feedback = s.cell.reservation->feedback;
    auto ticket = retained->saved_ticket();
    if (!s.identities(view, feedback.grant) || !feedback.device.matches(view.device_) || !retained->old_grant ||
        !retained->old_grant->token.matches(feedback.grant->token) ||
        !retained->old_grant->owner.connection.matches(feedback.grant->owner.connection) ||
        !retained->old_grant->owner.peer.matches(feedback.grant->owner.peer) ||
        !retained->prepared_grant->owner.destination.matches(feedback.grant->owner.destination) || !ticket ||
        !ticket->released.valid() || !ticket->connection.matches(feedback.grant->owner.connection))
      return false;
    // Real Pending TransferHandle proves Handoff already closed(false). A public
    // InputSeat::revoke would close(true) and cannot authorize healthy cancellation.
    s.cell.cancelling = true;
    s.ops.reset_peer(s.ops.owner.get(), &view.raw_);
    // Quarantine racing reset cannot be reopened by a local cleanup ack.
    if (retained->status() != ::sessions::TransferStatus::Pending)
      s.cell.failed = true;
    return true;
  } catch (...) {
    s.fail_exact(view.grant_);
    return false;
  }
}
size_t FeedbackDomain::logical_charge() {
  // One fixed cell + router queue + maximum encrypted wire packet + ENet queue
  // command. EVP/allocator overhead and external retained graphs are not RSS caps.
  return sizeof(State) + 256 * sizeof(std::optional<SeatFeedback>) + sizeof(FeedbackReservation) +
         sizeof(ControlEncryptedPacket) + sizeof(ENetPacket) + sizeof(ControlEncryptedPacket) +
         sizeof(ENetOutgoingCommand) + 8 * sizeof(State::Frame);
}
std::optional<ProtectedFeedbackExecutor>
FeedbackDomain::create(std::shared_ptr<InputSeat> seat, EnetOperations ops, Policy policy) {
  {
    std::lock_guard lock(mutex_);
    if (reserved_)
      return {};
    reserved_ = true; // failed/Unknown creation consumes this domain slot
  }
  if (!seat || !ops.owner || !ops.create || !ops.send || !ops.destroy || !ops.reset_peer || !policy.call_depth ||
      policy.call_depth > 8 || policy.logical_bytes < logical_charge()) {
    if (seat)
      if (auto grant = seat->current())
        seat->revoke(grant, grant->owner.peer);
    return {};
  }
  try {
    auto state = std::make_shared<State>(seat, std::move(ops), policy);
    {
      std::lock_guard lock(mutex_);
      state_ = state;
    }
    return ProtectedFeedbackExecutor(std::move(state));
  } catch (...) {
    if (auto grant = seat->current())
      seat->revoke(grant, grant->owner.peer);
    throw;
  }
}
FeedbackDomain::~FeedbackDomain() {
  // Lifecycle owner only. Execution owner must make progress independently.
  auto state = state_;
  if (!state)
    return;
  state->close();
  std::unique_lock lock(state->lifetime);
  state->wake.wait(lock, [&] { return state->calls == 0 && !state->live && !state->cipher_live; });
}
} // namespace control::protected_feedback
#endif
