#include "input_seat.hpp"
#ifdef __linux__
#include <stdexcept>
namespace control {
bool SeatCandidate::valid() const {
  return source && destination && connection && source.id() == std::to_string(connection.id()) && peer && binding &&
         slot < 16;
}
bool seat_detail::Gate::matches(const GrantHandle &grant) const {
  return grant && current && current->token.matches(grant->token) && current->owner.peer.matches(grant->owner.peer) &&
         current->owner.binding.matches(grant->owner.binding);
}
void seat_detail::Gate::close(bool fault) {
  quarantined |= fault;
  if (open || bootstrap) {
    neutral_requested = true;
    neutral_done = false;
    neutral.reset();
  }
  open = false;
  bootstrap = false;
  for (auto &entry : queue)
    entry.reset();
  for (auto &entry : feedback)
    entry.reset();
  count = bytes = head = feedback_count = feedback_head = 0;
  wake.notify_all();
}
FeedbackReservation::FeedbackReservation(std::shared_ptr<seat_detail::Gate> gate, SeatFeedback value)
    : gate_(std::move(gate)), feedback(std::move(value)) {}
void FeedbackReservation::retire(bool success) {
  std::lock_guard lock(gate_->mutex);
  if (retired_)
    return;
  retired_ = true;
  --gate_->sends;
  if (!success)
    gate_->close(true);
  gate_->wake.notify_all();
}
std::optional<SeatFeedback> SeatFeedbackRouter::capture(int low, int high) {
  std::lock_guard lock(gate_->mutex);
  if (!gate_->open || gate_->quarantined)
    return {};
  return SeatFeedback{gate_->current, gate_->device, low, high};
}
bool SeatFeedbackRouter::enqueue(SeatFeedback value) {
  std::lock_guard lock(gate_->mutex);
  if (!gate_->open || gate_->quarantined || !gate_->matches(value.grant) || !gate_->device.matches(value.device))
    return false;
  if (gate_->feedback_count == gate_->feedback.size()) {
    gate_->close(true);
    return false;
  }
  const auto index = (gate_->feedback_head + gate_->feedback_count++) % gate_->feedback.size();
  gate_->feedback[index] = std::move(value);
  return true;
}
void SeatFeedbackRouter::intake(int low, int high) noexcept {
  try {
    if (auto value = capture(low, high))
      enqueue(std::move(*value));
  } catch (...) {
    std::lock_guard lock(gate_->mutex);
    gate_->close(true);
  }
}
std::shared_ptr<FeedbackReservation> SeatFeedbackRouter::reserve_send() {
  std::lock_guard lock(gate_->mutex);
  if (!gate_->open || gate_->quarantined || !gate_->feedback_count || gate_->sends)
    return {};
  auto &value = gate_->feedback[gate_->feedback_head];
  if (!value || !gate_->matches(value->grant) || !gate_->device.matches(value->device)) {
    gate_->close(true);
    return {};
  }
  try {
    auto reservation = std::shared_ptr<FeedbackReservation>(new FeedbackReservation(gate_, *value));
    ++gate_->sends;
    value.reset();
    --gate_->feedback_count;
    gate_->feedback_head = (gate_->feedback_head + 1) % gate_->feedback.size();
    return reservation;
  } catch (...) {
    gate_->close(true);
    return {};
  }
}
std::shared_ptr<InputSeat> InputSeat::create(
    SeatCandidate owner, ProtectedInventory inventory, SeatValidation validation, Factory factory, SeatPolicy policy) {
  if (!owner.valid() || !inventory.valid() || !validation || !factory || !policy.packets || policy.packets > 256 ||
      policy.bytes < sizeof(SeatCommand) || policy.bytes > 65536)
    throw std::invalid_argument("Protected seat requires explicit fresh inventory and retained authority");
  auto seat = std::shared_ptr<InputSeat>(new InputSeat);
  seat->gate_ = std::make_shared<seat_detail::Gate>();
  auto &g = *seat->gate_;
  g.destination = owner.destination;
  g.current = std::make_shared<const SeatGrant>(std::move(owner));
  g.initial_validation = std::move(validation);
  g.policy = policy;
  seat->router_ = std::shared_ptr<SeatFeedbackRouter>(new SeatFeedbackRouter(seat->gate_));
  seat->backend_ = factory([router = seat->router_](int low, int high) { router->intake(low, high); });
  if (!seat->backend_)
    throw std::invalid_argument("Missing checked backend");
  g.device = seat->backend_->generation();
  if (!g.device.matches(g.device))
    throw std::invalid_argument("Missing device generation");
  seat->worker_ = std::thread([gate = seat->gate_, backend = seat->backend_] { work(gate, backend); });
  return seat;
}
InputSeat::~InputSeat() {
  if (gate_) {
    std::lock_guard lock(gate_->mutex);
    gate_->shutdown = true;
    gate_->close(true);
  }
  if (worker_.joinable())
    worker_.join();
}
void InputSeat::work(const std::shared_ptr<seat_detail::Gate> &gate,
                     const std::shared_ptr<CheckedXboxBackend> &backend) {
  for (;;) {
    std::optional<SeatCommand> command;
    bool neutral = false;
    {
      std::unique_lock lock(gate->mutex);
      gate->wake.wait(lock, [&] { return gate->shutdown || gate->count || (gate->neutral_requested && !gate->sends); });
      if (gate->shutdown)
        return;
      neutral = gate->neutral_requested && !gate->sends;
      if (neutral) {
        gate->neutral_requested = false;
        gate->neutral_running = true;
      } else if (gate->count) {
        command = std::move(gate->queue[gate->head]);
        gate->queue[gate->head].reset();
        gate->head = (gate->head + 1) % gate->queue.size();
        --gate->count;
        gate->bytes -= sizeof(SeatCommand) + command->packet.capacity();
        if (!gate->open || !gate->matches(command->grant))
          continue;
      }
      if (!neutral && !command)
        continue;
      gate->writing = true;
      gate->inflight_bytes = command ? sizeof(SeatCommand) + command->packet.capacity() : 0;
    }
    inputtino::checked::WriteReceipt receipt;
    try {
      receipt = neutral ? backend->neutral(gate->device) : backend->apply(gate->device, command->frame);
    } catch (...) {
      receipt.outcome = inputtino::checked::WriteOutcome::Unknown;
    }
    {
      std::lock_guard lock(gate->mutex);
      gate->writing = false;
      gate->inflight_bytes = 0;
      const bool accepted = receipt.outcome == inputtino::checked::WriteOutcome::KernelFrameAccepted &&
                            receipt.generation.matches(gate->device) && receipt.accepted_events == 20 &&
                            receipt.attempted_events == 20;
      if (neutral) {
        gate->neutral = receipt;
        gate->neutral_running = false;
        gate->neutral_done = accepted;
        if (!accepted)
          gate->quarantined = true;
        if (gate->bootstrap) {
          bool valid = false;
          try {
            valid = gate->initial_validation(gate->current->owner);
          } catch (...) {
          }
          gate->bootstrap = false;
          const auto status = backend->listener_status();
          gate->open = accepted && valid && !gate->quarantined && !status.failed && !status.stop_requested;
          if (!gate->open)
            gate->quarantined = true;
          gate->initial_validation = {};
        }
      } else if (!accepted)
        gate->close(true);
      gate->wake.notify_all();
    }
  }
}
SeatAdmission InputSeat::submit(SeatCommand command) {
  std::lock_guard lock(gate_->mutex);
  if (gate_->quarantined)
    return SeatAdmission::Quarantined;
  if (!gate_->open || !gate_->matches(command.grant))
    return SeatAdmission::Stale;
  if (command.kind != InputKind::Pad || command.slot >= 16 || command.slot != command.grant->owner.slot ||
      (command.frame.buttons & ~uint32_t(0xf7ff)))
    return SeatAdmission::Denied;
  const auto bytes = command.packet.capacity();
  if (gate_->count + (gate_->inflight_bytes ? 1 : 0) >= gate_->policy.packets ||
      bytes > gate_->policy.bytes - sizeof(SeatCommand) ||
      gate_->bytes + gate_->inflight_bytes > gate_->policy.bytes - sizeof(SeatCommand) - bytes) {
    gate_->close(true);
    return SeatAdmission::Overflow;
  }
  const auto index = (gate_->head + gate_->count) % gate_->queue.size();
  gate_->queue[index] = std::move(command);
  ++gate_->count;
  gate_->bytes += sizeof(SeatCommand) + bytes;
  gate_->wake.notify_all();
  return SeatAdmission::Accepted;
}
bool InputSeat::revoke(const GrantHandle &grant, const PeerToken &peer) {
  std::lock_guard lock(gate_->mutex);
  if (!gate_->matches(grant) || !peer.matches(grant->owner.peer))
    return false;
  gate_->close(true);
  return true;
}
GrantHandle InputSeat::current() const {
  std::lock_guard lock(gate_->mutex);
  return gate_->open && !gate_->quarantined ? gate_->current : GrantHandle{};
}
bool InputSeat::quarantined() const {
  std::lock_guard lock(gate_->mutex);
  return gate_->quarantined;
}
bool InputSeat::neutral_complete() const {
  std::lock_guard lock(gate_->mutex);
  return gate_->neutral_done;
}
std::shared_ptr<InputSeat> InputSeatDomain::create(SeatCandidate owner,
                                                   ProtectedInventory inventory,
                                                   SeatValidation validation,
                                                   InputSeat::Factory factory,
                                                   SeatPolicy policy) {
  {
    std::lock_guard lock(mutex_);
    if (reserved_)
      return {};
    reserved_ = true;
  }
  auto seat = InputSeat::create(std::move(owner), inventory, std::move(validation), std::move(factory), policy);
  {
    std::lock_guard lock(mutex_);
    seat_ = seat;
  }
  return seat;
}
} // namespace control
#endif
