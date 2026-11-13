#include "input_handoff.hpp"
#ifdef __linux__
#include "protected_authority.hpp"
#include <stdexcept>
namespace sessions {
using namespace std::chrono_literals;
namespace {
class AdmissionRetirement {
  std::shared_ptr<control::seat_detail::Gate> gate_;
  control::GrantHandle owner_;

public:
  explicit AdmissionRetirement(std::shared_ptr<control::seat_detail::Gate> gate) : gate_(std::move(gate)) {}
  AdmissionRetirement(const AdmissionRetirement &) = delete;
  bool reserve(const control::GrantHandle &owner) { // gate held; never allocates
    if (!owner || gate_->admission_cleanup_pending || gate_->admission_cleanup_owner) {
      gate_->close(true);
      return false;
    }
    owner_ = owner;
    gate_->admission_cleanup_owner = owner;
    gate_->admission_cleanup_pending = true;
    return true;
  }
  ~AdmissionRetirement() {
    if (!owner_)
      return; // no actual Runtime admission was attempted
    // Declared before EVERY temporary callable owner in admit_prepared: their
    // destructors have actually returned before this acknowledgement can run.
    std::lock_guard lock(gate_->mutex);
    const auto &saved = gate_->admission_cleanup_owner;
    if (!gate_->admission_cleanup_pending || !saved || !saved->token.matches(owner_->token) ||
        !saved->owner.connection.matches(owner_->owner.connection) || !saved->owner.peer.matches(owner_->owner.peer)) {
      gate_->close(true); // inconsistent accounting never acknowledges retirement
    } else {
      gate_->admission_cleanup_owner.reset();
      gate_->admission_cleanup_pending = false;
    }
    gate_->wake.notify_all();
  }
};
} // namespace
TransferHandle::TransferHandle(std::shared_ptr<control::seat_detail::Gate> gate,
                               TransferToken token,
                               AuthorizedSelection selection,
                               std::chrono::steady_clock::time_point time)
    : gate_(std::move(gate)), validate_(std::move(selection.validate)), authority_(std::move(selection.authority)),
      identity(std::move(token)), old_grant(std::move(selection.expected_old)),
      prepared_grant(std::make_shared<const control::SeatGrant>(std::move(selection.candidate))), started(time) {}
std::optional<streaming::ConsumerDrainTicket> TransferHandle::saved_ticket() const {
  std::lock_guard lock(gate_->mutex);
  return ticket_;
}
InputHandoff::InputHandoff(std::shared_ptr<control::InputSeat> seat,
                           std::shared_ptr<streaming::ConsumerRuntime> consumers,
                           std::function<Clock::time_point()> now,
                           control::SeatThreadFactory start)
    : seat_(std::move(seat)), consumers_(std::move(consumers)), now_(std::move(now)) {
  if (!seat_ || !consumers_ || !now_ || !start)
    throw std::invalid_argument("Handoff requires retained seat/runtime/clock");
  {
    std::lock_guard lock(seat_->gate_->mutex);
    if (seat_->gate_->coordinator_reserved)
      throw std::invalid_argument("One coordinator per retained seat");
    seat_->gate_->coordinator_reserved = true;
  }
  try {
    supervisor_ = start([this] {
      while (!closing_.load()) {
        poll();
        std::unique_lock lock(seat_->gate_->mutex);
        seat_->gate_->wake.wait_for(lock, 5ms);
      }
    });
    if (!supervisor_.joinable())
      throw std::runtime_error("Coordinator launch returned no owned thread");
  } catch (...) {
    std::lock_guard lock(seat_->gate_->mutex);
    seat_->gate_->close(true);
    throw;
  }
}
InputHandoff::~InputHandoff() {
  closing_ = true;
  {
    std::lock_guard lock(seat_->gate_->mutex);
    quarantine_locked();
    seat_->gate_->wake.notify_all();
  }
  if (supervisor_.joinable())
    supervisor_.join();
}
void InputHandoff::quarantine_locked() {
  seat_->gate_->close(true);
  if (pending_ && pending_->status() == TransferStatus::Pending)
    pending_->status_ = TransferStatus::Quarantined;
}
TransferResult InputHandoff::request(TransferToken identity, AuthorizedSelection supplied) {
  protected_authority::Retirement authority_retirement(seat_->gate_);
  AuthorizedSelection selection{std::move(supplied.expected_old),
                                std::move(supplied.candidate),
                                {},
                                std::move(supplied.authority)};
  selection.validate.swap(supplied.validate); // parameter has no callback owner after local RAII ack
  bool additional_valid = true;
  if (seat_->gate_->authority && selection.validate) {
    try {
      additional_valid = selection.validate(selection.candidate);
    } catch (...) {
      additional_valid = false;
    }
  }
  // Retain the actual callable, not a potentially independent std::function copy.
  // Both allocations happen before the gate. Constructor rollback may destroy
  // its wrapper under the gate, but cannot retire this callable until unlock.
  std::shared_ptr<control::SeatValidation> retained_validation;
  if (selection.validate) {
    retained_validation = std::make_shared<control::SeatValidation>();
    retained_validation->swap(selection.validate);
    selection.validate = [retained_validation](const control::SeatCandidate &candidate) {
      return (*retained_validation)(candidate);
    };
  }
  std::shared_ptr<TransferHandle> retired_transaction;
  std::lock_guard lock(seat_->gate_->mutex);
  auto &g = *seat_->gate_;
  if (pending_ && identity.matches(pending_->identity))
    return {TransferAdmission::Duplicate, pending_};
  if (g.quarantined)
    return {TransferAdmission::Quarantined, {}};
  if (pending_ && pending_->status() == TransferStatus::Pending)
    return {TransferAdmission::Busy, {}};
  if (!identity || !g.open || !g.matches(selection.expected_old) || !selection.candidate.valid() ||
      !selection.candidate.destination.matches(g.destination) ||
      selection.expected_old->owner.connection.matches(selection.candidate.connection) ||
      selection.expected_old->owner.peer.matches(selection.candidate.peer) || (!selection.validate && !g.authority))
    return {TransferAdmission::Invalid, {}};
  try {
    if (g.authority &&
        (!additional_valid || !g.authority->permits_locked(selection.authority, selection.candidate, g.device)))
      return {TransferAdmission::Invalid, {}};
    if (!g.authority && !selection.validate(selection.candidate))
      return {TransferAdmission::Invalid, {}};
    if (g.authority && !authority_retirement.reserve_locked()) {
      quarantine_locked();
      return {TransferAdmission::Quarantined, {}};
    }
    // Allocate complete strong transaction/prepared grant BEFORE revoking anything.
    auto transaction = std::shared_ptr<TransferHandle>(
        new TransferHandle(seat_->gate_, std::move(identity), std::move(selection), now_()));
    // The old handle can be the sole owner of an arbitrary validator capture.
    // Declare its retirement owner before lock so replacement cannot run cleanup
    // inside the serial domain, including when a later revoke throws.
    retired_transaction = std::move(pending_);
    pending_ = std::move(transaction);
    if (g.authority)
      g.authority->pending_locked(pending_->authority_);
    g.close(false);
    // Exact old scope, still held/current here. No ID lookup or shadow registry.
    pending_->ticket_ = consumers_->revoke(pending_->old_grant->owner.connection);
    if (!pending_->ticket_ || !pending_->ticket_->connection.matches(pending_->old_grant->owner.connection))
      quarantine_locked();
    g.wake.notify_all();
    return {TransferAdmission::Accepted, pending_};
  } catch (...) {
    quarantine_locked();
    return {TransferAdmission::Quarantined, pending_};
  }
}
bool InputHandoff::drain_complete(const std::optional<streaming::ConsumerDrainTicket> &ticket,
                                  const control::ConnectionScope &scope) {
  return ticket && ticket->connection.matches(scope) && ticket->complete();
}
std::optional<TransferObservation> InputHandoff::observe() const {
  std::lock_guard lock(seat_->gate_->mutex);
  if (!pending_)
    return {};
  return TransferObservation{pending_->identity, pending_->status()};
}
void InputHandoff::poll_deadline() {
  std::lock_guard lock(seat_->gate_->mutex);
  if (!pending_ || pending_->status() != TransferStatus::Pending)
    return;
  try {
    const auto now = now_();
    const auto elapsed_ticks = uint64_t(now.time_since_epoch().count()) -
                               uint64_t(pending_->started.time_since_epoch().count());
    if (now < pending_->started || elapsed_ticks >= uint64_t(std::chrono::duration_cast<Clock::duration>(5s).count()))
      quarantine_locked();
  } catch (...) {
    quarantine_locked();
  }
}
void InputHandoff::poll() {
  bool additional_valid = true, validation_ready = !seat_->gate_->authority;
  TransferToken observed_identity;
  if (seat_->gate_->authority) {
    // Retain the ACTUAL validator owner while invoking outside the gate. This
    // temporary owner retires before its own authority cleanup acknowledgement.
    protected_authority::Retirement retirement(seat_->gate_);
    std::shared_ptr<TransferHandle> observed;
    {
      std::lock_guard lock(seat_->gate_->mutex);
      if (pending_ && pending_->status() == TransferStatus::Pending && !seat_->gate_->authority->cleanup_locked() &&
          retirement.reserve_locked()) {
        observed = pending_;
        observed_identity = observed->identity;
      }
    }
    if (observed) {
      try {
        if (observed->validate_)
          additional_valid = observed->validate_(observed->prepared_grant->owner);
      } catch (...) {
        additional_valid = false;
      }
      validation_ready = true;
    }
  }
  std::lock_guard lock(seat_->gate_->mutex);
  auto &g = *seat_->gate_;
  try {
    const auto backend_status = seat_->backend_->listener_status();
    if (backend_status.failed || backend_status.stop_requested)
      quarantine_locked();
    if (g.quarantined && !quarantine_revoked_ && g.current) {
      quarantine_revoked_ = true;
      quarantine_ticket_ = consumers_->revoke(g.current->owner.connection);
    }
    if (!pending_ || pending_->status() != TransferStatus::Pending)
      return;
    const auto now = now_();
    const auto elapsed_ticks = uint64_t(now.time_since_epoch().count()) -
                               uint64_t(pending_->started.time_since_epoch().count());
    const bool typed_valid =
        !g.authority || g.authority->permits_locked(pending_->authority_, pending_->prepared_grant->owner, g.device);
    if (g.quarantined || !typed_valid || now < pending_->started ||
        elapsed_ticks >= uint64_t(std::chrono::duration_cast<Clock::duration>(5s).count()) ||
        (!g.authority && !pending_->validate_(pending_->prepared_grant->owner)) ||
        (g.authority && validation_ready && observed_identity.matches(pending_->identity) && !additional_valid)) {
      quarantine_locked();
      return;
    }
    if (g.authority &&
        (g.authority->cleanup_locked() || !validation_ready || !observed_identity.matches(pending_->identity)))
      return;
    if (g.admission_cleanup_pending || g.admission_cleanup_owner) {
      const auto &owner = g.admission_cleanup_owner;
      if (!g.admission_cleanup_pending || !owner || !owner->token.matches(pending_->old_grant->token) ||
          !owner->owner.connection.matches(pending_->old_grant->owner.connection))
        quarantine_locked();
      return; // neutral remains independent; the single decision deadline above still applies
    }
    if (g.writing || g.sends || g.neutral_running || g.neutral_requested)
      return;
    if (!g.neutral_done || !g.neutral || !g.device.matches(seat_->backend_->generation()) ||
        !g.neutral->generation.matches(g.device)) {
      quarantine_locked();
      return;
    }
    if (!pending_->ticket_ || !pending_->ticket_->released.valid()) {
      quarantine_locked();
      return;
    }
    if (!drain_complete(pending_->ticket_, pending_->old_grant->owner.connection)) {
      if (pending_->ticket_->released.wait_for(0ms) == std::future_status::ready)
        quarantine_locked();
      return;
    }
    // All guards and publication share input/send reservation serial gate.
    if (!g.matches(pending_->old_grant) || !pending_->prepared_grant->owner.destination.matches(g.destination) ||
        (!g.authority && !pending_->validate_(pending_->prepared_grant->owner))) {
      quarantine_locked();
      return;
    }
    if (g.authority)
      g.authority->commit_locked(pending_->authority_);
    g.current = pending_->prepared_grant;
    g.open = true;
    pending_->status_ = TransferStatus::Granted;
  } catch (...) {
    quarantine_locked();
  }
}
bool InputHandoff::candidate_gone(const control::ConnectionScope &connection, const control::PeerToken &peer) {
  std::lock_guard lock(seat_->gate_->mutex);
  if (!pending_ || pending_->status() == TransferStatus::Quarantined ||
      !connection.matches(pending_->prepared_grant->owner.connection) ||
      !peer.matches(pending_->prepared_grant->owner.peer))
    return false;
  // Notification racing the commit must also revoke the just-published grant.
  // A historical handle for an older grant cannot close another incarnation.
  if (pending_->status() == TransferStatus::Granted && !seat_->gate_->matches(pending_->prepared_grant))
    return false;
  quarantine_locked();
  return true;
}
bool InputHandoff::prepared_admission(const std::shared_ptr<const TransferHandle> &handle,
                                      const control::ConnectionScope &connection,
                                      const control::PeerToken &peer) const {
  std::lock_guard lock(seat_->gate_->mutex);
  return handle && handle->status() == TransferStatus::Granted && seat_->gate_->open && !seat_->gate_->quarantined &&
         seat_->gate_->matches(handle->prepared_grant) &&
         connection.matches(handle->prepared_grant->owner.connection) &&
         peer.matches(handle->prepared_grant->owner.peer) &&
         (!seat_->gate_->authority || (!seat_->gate_->authority->cleanup_locked() &&
                                       seat_->gate_->authority->permits_locked(handle->authority_,
                                                                               handle->prepared_grant->owner,
                                                                               seat_->gate_->device)));
}
streaming::ConsumerAdmission InputHandoff::admit_prepared(const std::shared_ptr<const TransferHandle> &handle,
                                                          const control::ConnectionScope &connection,
                                                          const control::PeerToken &peer,
                                                          streaming::ConsumerKind kind,
                                                          streaming::ConsumerCorrelation correlation,
                                                          streaming::ConsumerRuntime::Work work,
                                                          std::function<void()> before_cleanup) {
  // Same bounded slot as protected poll: reserve before invoking the actual
  // retained callable. Keep it through Work/validator owner destruction, outside
  // all locks. No extra executor, copied callback or caller serialization needed.
  protected_authority::Retirement invocation(seat_->gate_);
  const auto retained_handle = handle;
  bool additional_valid = true;
  AdmissionRetirement retirement(seat_->gate_);
  // These actual callable owners are prepared before the gate. Swap leaves the
  // function parameters empty, including on implementations retaining moved-from
  // std::function state. Parameters must not release payload after local cleanup.
  const auto retained_work = std::make_shared<streaming::ConsumerRuntime::Work>();
  retained_work->swap(work);
  streaming::ConsumerRuntime::Work forwarded = [retained_work](streaming::ConsumerJob &job,
                                                               const streaming::ConsumerPing &ping) {
    return (*retained_work)(job, ping);
  };
  std::function<void()> cleanup_hook;
  cleanup_hook.swap(before_cleanup);
  if (seat_->gate_->authority) {
    {
      std::lock_guard lock(seat_->gate_->mutex);
      auto &g = *seat_->gate_;
      if (!retained_handle || retained_handle->status() != TransferStatus::Granted || !g.open || g.quarantined ||
          !g.matches(retained_handle->prepared_grant) ||
          !connection.matches(retained_handle->prepared_grant->owner.connection) ||
          !peer.matches(retained_handle->prepared_grant->owner.peer) ||
          !g.authority->permits_locked(retained_handle->authority_, retained_handle->prepared_grant->owner, g.device))
        return streaming::ConsumerAdmission::Revoked;
      // Ordinary overlap is refusal, not a fault on the healthy current grant.
      if (g.authority->cleanup_locked())
        return streaming::ConsumerAdmission::Capacity;
      if (!invocation.reserve_locked())
        return streaming::ConsumerAdmission::Capacity;
    }
    try {
      if (retained_handle->validate_)
        additional_valid = retained_handle->validate_(retained_handle->prepared_grant->owner);
    } catch (...) {
      additional_valid = false;
    }
  }
  bool runtime_returned = false;
  auto result = [&] {
    std::lock_guard lock(seat_->gate_->mutex);
    auto &g = *seat_->gate_;
    if (!handle || handle->status() != TransferStatus::Granted || !g.open || g.quarantined ||
        !g.matches(handle->prepared_grant) || !connection.matches(handle->prepared_grant->owner.connection) ||
        !peer.matches(handle->prepared_grant->owner.peer))
      return streaming::ConsumerAdmission::Revoked;
    try {
      const auto status = seat_->backend_->listener_status();
      if (status.failed || status.stop_requested ||
          (g.authority ? (!additional_valid ||
                          !g.authority->permits_locked(handle->authority_, handle->prepared_grant->owner, g.device))
                       : !handle->validate_(handle->prepared_grant->owner))) {
        quarantine_locked();
        return streaming::ConsumerAdmission::Revoked;
      }
      if (!retirement.reserve(handle->prepared_grant)) {
        quarantine_locked();
        return streaming::ConsumerAdmission::Capacity;
      }
      auto admitted = consumers_->admit(connection,
                                        handle->prepared_grant->owner.source,
                                        kind,
                                        std::move(correlation),
                                        std::move(forwarded));
      runtime_returned = true;
      return admitted;
    } catch (...) {
      quarantine_locked();
      return streaming::ConsumerAdmission::Revoked;
    }
  }();
  // Internal injected synchronization seam, never a completion acknowledgement.
  // Invocation AND capture destruction are outside the serial gate.
  try {
    if (runtime_returned && cleanup_hook)
      cleanup_hook();
  } catch (...) {
    std::lock_guard lock(seat_->gate_->mutex);
    quarantine_locked();
    result = streaming::ConsumerAdmission::Revoked;
  }
  return result;
}
} // namespace sessions
#endif
