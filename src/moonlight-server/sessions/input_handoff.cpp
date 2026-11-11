#include "input_handoff.hpp"
#ifdef __linux__
#include <stdexcept>
namespace sessions {
using namespace std::chrono_literals;
TransferHandle::TransferHandle(std::shared_ptr<control::seat_detail::Gate> gate,
                               TransferToken token,
                               AuthorizedSelection selection,
                               std::chrono::steady_clock::time_point time)
    : gate_(std::move(gate)), validate_(std::move(selection.validate)), identity(std::move(token)),
      old_grant(std::move(selection.expected_old)),
      prepared_grant(std::make_shared<const control::SeatGrant>(std::move(selection.candidate))), started(time) {}
std::optional<streaming::ConsumerDrainTicket> TransferHandle::saved_ticket() const {
  std::lock_guard lock(gate_->mutex);
  return ticket_;
}
InputHandoff::InputHandoff(std::shared_ptr<control::InputSeat> seat,
                           std::shared_ptr<streaming::ConsumerRuntime> consumers,
                           std::function<Clock::time_point()> now)
    : seat_(std::move(seat)), consumers_(std::move(consumers)), now_(std::move(now)) {
  if (!seat_ || !consumers_ || !now_)
    throw std::invalid_argument("Handoff requires retained seat/runtime/clock");
  {
    std::lock_guard lock(seat_->gate_->mutex);
    if (seat_->gate_->coordinator_reserved)
      throw std::invalid_argument("One coordinator per retained seat");
    seat_->gate_->coordinator_reserved = true;
  }
  try {
    supervisor_ = std::thread([this] {
      while (!closing_.load()) {
        poll();
        std::unique_lock lock(seat_->gate_->mutex);
        seat_->gate_->wake.wait_for(lock, 5ms);
      }
    });
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
TransferResult InputHandoff::request(TransferToken identity, AuthorizedSelection selection) {
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
      selection.expected_old->owner.peer.matches(selection.candidate.peer) || !selection.validate)
    return {TransferAdmission::Invalid, {}};
  try {
    if (!selection.validate(selection.candidate))
      return {TransferAdmission::Invalid, {}};
    // Allocate complete strong transaction/prepared grant BEFORE revoking anything.
    auto transaction = std::shared_ptr<TransferHandle>(
        new TransferHandle(seat_->gate_, std::move(identity), std::move(selection), now_()));
    pending_ = std::move(transaction);
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
void InputHandoff::poll() {
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
    if (g.quarantined || now_() - pending_->started >= 5s || !pending_->validate_(pending_->prepared_grant->owner)) {
      quarantine_locked();
      return;
    }
    if (g.writing || g.sends || g.neutral_running || g.neutral_requested)
      return;
    if (!g.neutral_done || !g.neutral || !g.device.matches(seat_->backend_->generation()) ||
        !g.neutral->generation.matches(g.device)) {
      quarantine_locked();
      return;
    }
    if (!drain_complete(pending_->ticket_, pending_->old_grant->owner.connection))
      return;
    // All guards and publication share input/send reservation serial gate.
    if (!g.matches(pending_->old_grant) || !pending_->prepared_grant->owner.destination.matches(g.destination) ||
        !pending_->validate_(pending_->prepared_grant->owner)) {
      quarantine_locked();
      return;
    }
    g.current = pending_->prepared_grant;
    g.open = true;
    pending_->status_ = TransferStatus::Granted;
  } catch (...) {
    quarantine_locked();
  }
}
bool InputHandoff::candidate_gone(const control::ConnectionScope &connection, const control::PeerToken &peer) {
  std::lock_guard lock(seat_->gate_->mutex);
  if (!pending_ || pending_->status() != TransferStatus::Pending ||
      !connection.matches(pending_->prepared_grant->owner.connection) ||
      !peer.matches(pending_->prepared_grant->owner.peer))
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
         peer.matches(handle->prepared_grant->owner.peer);
}
streaming::ConsumerAdmission InputHandoff::admit_prepared(const std::shared_ptr<const TransferHandle> &handle,
                                                          const control::ConnectionScope &connection,
                                                          const control::PeerToken &peer,
                                                          streaming::ConsumerKind kind,
                                                          streaming::ConsumerCorrelation correlation,
                                                          streaming::ConsumerRuntime::Work work) {
  std::lock_guard lock(seat_->gate_->mutex);
  auto &g = *seat_->gate_;
  if (!handle || handle->status() != TransferStatus::Granted || !g.open || g.quarantined ||
      !g.matches(handle->prepared_grant) || !connection.matches(handle->prepared_grant->owner.connection) ||
      !peer.matches(handle->prepared_grant->owner.peer))
    return streaming::ConsumerAdmission::Revoked;
  try {
    if (!handle->validate_(handle->prepared_grant->owner)) {
      quarantine_locked();
      return streaming::ConsumerAdmission::Revoked;
    }
    return consumers_->admit(connection, g.destination, kind, std::move(correlation), std::move(work));
  } catch (...) {
    quarantine_locked();
    return streaming::ConsumerAdmission::Revoked;
  }
}
} // namespace sessions
#endif
