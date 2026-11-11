#include "protected_authority.hpp"
#ifdef __linux__
#include <stdexcept>
namespace sessions::protected_authority {
Facts::Facts(std::unique_ptr<const access::AccessStore> store,
             std::string game,
             std::string app,
             access::TargetFact target,
             PairingRevision pairing)
    : store_(std::move(store)), game_(std::move(game)), app_(std::move(app)), target(std::move(target)),
      pairing(std::move(pairing)) {}
std::shared_ptr<const Facts> Facts::prepare(std::span<const access::RuleView> rules,
                                            std::string_view game,
                                            std::string_view app,
                                            control::RunScope run,
                                            PairingRevision pairing) {
  if (!pairing || !run || game.size() > access::max_identifier_bytes || app.size() > access::max_identifier_bytes)
    return {};
  auto store = std::make_unique<access::AccessStore>();
  if (store->replace_policy(rules) != access::Result::Allowed)
    return {};
  auto target = store->bind_target(game, app, run);
  if (!target)
    return {};
  // Mutable store is consumed here: no caller has ever received an alias.
  return std::shared_ptr<const Facts>(
      new Facts(std::move(store), std::string(game), std::string(app), *target, std::move(pairing)));
}
access::Evaluation Facts::evaluate(const access::Principal &principal) const {
  return store_->evaluate(principal, game_, app_);
}
bool Facts::locally_current(const access::Evidence &evidence) const {
  return store_->is_current(evidence);
}
ProtectedAuthorityDomain::ProtectedAuthorityDomain(PreparedFacts prepared, OperatorCapability cap, uint64_t limit)
    : gate_(std::make_shared<control::seat_detail::Gate>()) {
  if (!prepared.facts || !cap || !limit)
    throw std::invalid_argument("Missing protected authority facts/capacity");
  gate_->authority = std::make_shared<Cell>(std::move(prepared), std::move(cap), limit);
}
std::shared_ptr<control::InputSeat> ProtectedAuthorityDomain::create_seat(control::SeatCandidate candidate,
                                                                          const access::Principal &principal,
                                                                          control::ProtectedInventory inventory,
                                                                          control::InputSeat::Factory factory,
                                                                          control::SeatValidation additional,
                                                                          control::SeatPolicy policy,
                                                                          control::SeatThreadFactory start) {
  auto captured = facts();
  auto evaluation = captured->evaluate(principal); // parsing/store lock outside gate
  if (!candidate.valid() || !candidate.destination.matches(captured->target.run()) ||
      evaluation.result != access::Result::Allowed)
    throw std::invalid_argument("Bootstrap identity not allowed for exact destination");
  auto &c = *gate_->authority;
  {
    std::lock_guard lock(gate_->mutex);
    if (reserved_ || c.closed_ || c.facts_ != captured)
      throw std::invalid_argument("Protected domain unavailable");
    reserved_ = true; // start failure consumes slot
  }
  // All allocations/callback owners prepared before publication and backend start.
  try {
    auto bootstrap = PermitHandle(new Permit(c.domain_, captured, 0, evaluation.evidence, candidate, {}));
    {
      std::lock_guard lock(gate_->mutex);
      if (c.closed_ || c.sequence_ != 0)
        throw std::invalid_argument("Bootstrap was revoked");
      c.bootstrap_ = std::move(bootstrap);
    }
    seat_ = control::InputSeat::create(std::move(candidate),
                                       inventory,
                                       std::move(additional),
                                       std::move(factory),
                                       policy,
                                       std::move(start),
                                       gate_);
    return seat_;
  } catch (...) {
    std::lock_guard lock(gate_->mutex);
    c.closed_ = true;
    gate_->close(true);
    throw;
  }
}
FactHandle ProtectedAuthorityDomain::facts() const {
  std::lock_guard lock(gate_->mutex);
  return gate_->authority->facts_;
}
Status ProtectedAuthorityDomain::status() const {
  std::lock_guard lock(gate_->mutex);
  const auto &c = *gate_->authority;
  return {bool(c.bootstrap_), c.closed_, gate_->quarantined, c.cleanup_locked(), bool(c.update_), c.sequence_};
}
PermitHandle ProtectedAuthorityDomain::capture_selection(const control::SeatCandidate &candidate,
                                                         const access::Principal &principal) const {
  FactHandle captured;
  control::DeviceGeneration device;
  uint64_t seq;
  const auto &c = *gate_->authority;
  {
    std::lock_guard lock(gate_->mutex);
    if (c.closed_ || gate_->quarantined || !gate_->device)
      return {};
    captured = c.facts_;
    device = gate_->device;
    seq = c.sequence_;
  }
  if (!candidate.valid() || !candidate.destination.matches(captured->target.run()))
    return {};
  auto evaluated = captured->evaluate(principal);
  if (evaluated.result != access::Result::Allowed)
    return {};
  auto permit = PermitHandle(new Permit(c.domain_, captured, seq, evaluated.evidence, candidate, device));
  std::lock_guard lock(gate_->mutex);
  return !gate_->quarantined && c.permits_locked(permit, candidate, gate_->device) ? permit : nullptr;
}
UpdateResult ProtectedAuthorityDomain::begin_update(const OperatorCapability &cap, const FactHandle &expected) {
  // Fresh token allocation before gate. No callback/payload owns this token.
  auto token = std::make_shared<UpdateToken>();
  token->identity_ = UpdateIdentity::fresh(0);
  std::lock_guard lock(gate_->mutex);
  auto &c = *gate_->authority;
  if (!cap.matches(c.operator_) || !expected || c.facts_ != expected)
    return {Result::Stale, {}};
  if (c.sequence_ == c.sequence_limit_) {
    c.closed_ = true;
    gate_->close(true);
    return {Result::Unknown, {}};
  }
  // A newer command closes even if the one pending slot is occupied: no stale publish can revive it.
  ++c.sequence_;
  c.closed_ = true;
  gate_->close(true);
  if (c.update_ || c.cleanup_locked())
    return {Result::Busy, {}};
  token->domain_ = c.domain_;
  token->expected_ = expected;
  token->sequence_ = c.sequence_;
  c.update_ = token;
  return {Result::Accepted, std::move(token)};
}
bool ProtectedAuthorityDomain::close_exact(const OperatorCapability &cap, const FactHandle &expected) {
  std::lock_guard lock(gate_->mutex);
  auto &c = *gate_->authority;
  if (!cap.matches(c.operator_) || !expected || expected != c.facts_)
    return false;
  c.closed_ = true;
  if (c.sequence_ < c.sequence_limit_)
    ++c.sequence_;
  gate_->close(true);
  return true;
}
bool ProtectedAuthorityDomain::close_exact(const control::ConnectionScope &connection, const control::PeerToken &peer) {
  std::lock_guard lock(gate_->mutex);
  auto &c = *gate_->authority;
  // Candidate notices use a retained pending permit, recorded by actual request.
  const auto matches = [&](const control::SeatCandidate &v) {
    return connection.matches(v.connection) && peer.matches(v.peer);
  };
  if (!(gate_->current && matches(gate_->current->owner)) && !(c.pending_ && matches(c.pending_->candidate)))
    return false;
  c.closed_ = true;
  if (c.sequence_ < c.sequence_limit_)
    ++c.sequence_;
  gate_->close(true);
  return true;
}
Result ProtectedAuthorityDomain::publish_prepared(const std::shared_ptr<const UpdateToken> &token,
                                                  PreparedFacts prepared) {
  Retirement retirement(gate_);
  PreparedFacts incoming;
  incoming.facts.swap(prepared.facts);
  incoming.resource.swap(prepared.resource); // empty parameter before ack
  std::unique_ptr<RetirementResource> retired;
  {
    std::lock_guard lock(gate_->mutex);
    auto &c = *gate_->authority;
    if (!token || token != c.update_ || !token->domain_.matches(c.domain_) || token->sequence_ != c.sequence_ ||
        token->expected_ != c.facts_)
      return Result::Stale;
    if (!incoming.facts)
      return Result::Denied; // closed, pending cap retained
    if (!retirement.reserve_locked())
      return Result::Busy;
    retired = std::move(c.resource_);
    c.resource_ = std::move(incoming.resource);
    c.facts_ = std::move(incoming.facts);
    c.update_.reset(); // data-only token
  }
  try {
    if (retired)
      retired->retire();
    retired.reset(); // actual destructor outside ALL locks, before RAII ack
  } catch (...) {
    retirement.failed();
    std::lock_guard lock(gate_->mutex);
    gate_->authority->failed_resource_ = std::move(retired);
    gate_->close(true);
    return Result::Unknown;
  }
  return Result::Accepted; // facts only: closed/quarantined remains sticky
}
} // namespace sessions::protected_authority
#endif
