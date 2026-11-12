#include "protected_pairing.hpp"
#ifdef __linux__
namespace sessions::protected_pairing {
Publication::Publication(OwnerIdentity owner,
                         state::PairedClientList source,
                         authority::PairingRevision revision,
                         identity::PairedSnapshot sealed,
                         authority::FactHandle facts)
    : owner(std::move(owner)), identity(PublicationIdentity::fresh(0)), source(std::move(source)),
      revision(std::move(revision)), sealed(std::move(sealed)), facts(std::move(facts)) {}
PersistenceOutcome::PersistenceOutcome(OwnerIdentity owner,
                                       authority::UpdateIdentity update,
                                       uint64_t seq,
                                       PublicationHandle source,
                                       PublicationHandle target,
                                       PersistenceResult status)
    : owner_(std::move(owner)), update_(std::move(update)), sequence_(seq), source_(std::move(source)),
      result_(std::move(target)), result(status) {}
Transaction::Transaction(OwnerIdentity owner,
                         authority::UpdateIdentity update,
                         uint64_t seq,
                         PublicationHandle previous,
                         PublicationHandle next)
    : owner_(std::move(owner)), update_(std::move(update)), sequence_(seq), source(std::move(previous)),
      result(std::move(next)) {}
PersistenceOutcome Transaction::outcome(PersistenceResult status) const {
  return {owner_, update_, sequence_, source, result, status};
}
bool Transaction::matches(const PersistenceOutcome &outcome) const {
  return owner_.matches(outcome.owner_) && update_.matches(outcome.update_) && sequence_ == outcome.sequence_ &&
         source == outcome.source_ && result == outcome.result_;
}
bool PairingMutationOwner::bounded(const state::PairedClientList &clients) {
  if (clients.size() > access::max_pin_entries)
    return false;
  size_t bytes = 0;
  for (const auto &client : clients) {
    const auto size = client->client_cert.size();
    if (size > access::max_pem_bytes || size > access::max_policy_der_bytes - bytes)
      return false;
    bytes += size;
  }
  return true;
}
PublicationHandle PairingMutationOwner::prepare_publication(const state::PairedClientList &clients,
                                                            std::span<const access::RuleView> rules,
                                                            const control::RunScope &run,
                                                            authority::PairingRevision revision) const {
  if (!bounded(clients) || !revision)
    return {};
  identity::PairedSnapshot sealed;
  if (!clients.empty()) {
    auto result = identity::PairedSnapshot::seal(clients, revision);
    if (!result.value)
      return {};
    sealed = std::move(*result.value);
  }
  auto facts = authority::Facts::prepare(rules, game_, app_, run, revision);
  if (!facts)
    return {};
  return PublicationHandle(
      new Publication(identity_, clients, std::move(revision), std::move(sealed), std::move(facts)));
}
PairingMutationOwner::PairingMutationOwner(std::shared_ptr<ProtectedRuntimeOwner> runtime,
                                           const state::PairedClientList &clients,
                                           std::span<const access::RuleView> rules,
                                           std::string_view game,
                                           std::string_view app,
                                           RuntimeBootstrap bootstrap,
                                           uint64_t limit)
    : op_(bootstrap.op), runtime_(std::move(runtime)),
      game_(game.size() <= access::max_identifier_bytes ? game : std::string_view{}),
      app_(app.size() <= access::max_identifier_bytes ? app : std::string_view{}), sequence_limit_(limit) {
  if (!runtime_ || !op_ || !limit || game_.empty() || app_.empty() || clients.empty())
    throw std::invalid_argument("Unresolved pairing bootstrap");
  current_ = prepare_publication(clients, rules, bootstrap.initial.destination, authority::PairingRevision::fresh(0));
  if (!current_)
    throw std::invalid_argument("Invalid bounded pairing publication");
  bootstrap.facts.facts = current_->facts; // revision installed BEFORE actual protected Seat bootstrap
  if (!runtime_->start(std::move(bootstrap)))
    throw std::runtime_error("Protected runtime start failed; reserved runtime cap retained");
}
PairingMutationOwner::~PairingMutationOwner() {
  close_exact(op_, current_); // no worker owns this facade
}
PublicationHandle PairingMutationOwner::capture_publication() const {
  std::lock_guard lock(mutex_);
  return phase_ == Phase::Current || phase_ == Phase::ClosedEmpty ? current_ : nullptr;
}
Status PairingMutationOwner::status() {
  const auto execution = runtime_->status(); // existing deadline driver OUTSIDE publication mutex
  if (execution.stop) {
    std::lock_guard lock(mutex_);
    if (pending_)
      phase_ = Phase::Unknown; // failed/abandoned queued work cannot look like progress
  }
  poll();
  std::lock_guard lock(mutex_);
  return {phase_, sequence_, bool(pending_)};
}
bool PairingMutationOwner::ready(const UpdateHandle &update) const {
  std::shared_ptr<const authority::UpdateToken> token;
  {
    std::lock_guard lock(mutex_);
    if (!update || pending_ != update || phase_ != Phase::Preparing || sequence_ != update->sequence_)
      return false;
    token = update->token_;
  }
  return runtime_->publication_ready(op_, token);
}
void PairingMutationOwner::poll() {
  // Every retained member here is data-only. No arbitrary callback graph is inspected.
  std::lock_guard lock(mutex_);
  if (phase_ != Phase::Retiring || !pending_ || !receipt_)
    return;
  const auto state = receipt_->state.load();
  if (state == RuntimeJobState::Queued || state == RuntimeJobState::Executing)
    return;
  if (state != RuntimeJobState::Complete || receipt_->authority_result != authority::Result::Accepted ||
      pending_->sequence_ != sequence_ || pending_->source_ != current_ || !pending_->prepared_) {
    phase_ = Phase::Unknown;
    return; // retained Unknown cap; no late automatic retry/grant
  }
  current_ = pending_->prepared_;
  phase_ = current_->source.empty() ? Phase::ClosedEmpty : Phase::Current;
  pending_.reset();
  receipt_.reset();
}
BeginResult PairingMutationOwner::begin_update(const authority::OperatorCapability &op,
                                               const PublicationHandle &expected) {
  // Validate BEFORE fresh allocation or Closing. Recheck after preparation and actual gate call.
  {
    std::lock_guard lock(mutex_);
    if (!op.matches(op_) || !expected || expected != current_ || !expected->owner.matches(identity_))
      return {Result::Stale, {}};
    if (pending_ || (phase_ != Phase::Current && phase_ != Phase::ClosedEmpty))
      return {Result::Busy, {}};
  }
  auto update = std::shared_ptr<Update>(new Update);
  update->owner_ = identity_;
  update->identity_ = authority::UpdateIdentity::fresh(0);
  update->revision_ = authority::PairingRevision::fresh(0);
  update->source_ = expected;
  bool exhausted = false;
  {
    std::lock_guard lock(mutex_);
    if (current_ != expected || !op.matches(op_))
      return {Result::Stale, {}};
    if (pending_ || (phase_ != Phase::Current && phase_ != Phase::ClosedEmpty))
      return {Result::Busy, {}};
    exhausted = sequence_ == sequence_limit_;
    if (!exhausted)
      ++sequence_;
    update->sequence_ = sequence_;
    pending_ = update;
    phase_ = exhausted ? Phase::Unknown : Phase::Closing;
  }
  if (exhausted) {
    runtime_->close_exact(op, expected->facts);
    return {Result::Unknown, update};
  }
  authority::UpdateResult result{authority::Result::Unknown, {}};
  try {
    result = runtime_->begin_authority_update(op, expected->facts);
  } catch (...) {
    runtime_->close_exact(op, expected->facts);
  }
  std::lock_guard lock(mutex_);
  if (phase_ != Phase::Closing || sequence_ != update->sequence_ || result.result != authority::Result::Accepted ||
      !result.token) {
    phase_ = Phase::Unknown;
    return {Result::Unknown, update};
  }
  update->token_ = std::move(result.token); // data only
  phase_ = Phase::Preparing;
  return {Result::Accepted, update};
}
bool PairingMutationOwner::close_exact(const authority::OperatorCapability &op, const PublicationHandle &expected) {
  PublicationHandle prepared;
  {
    std::lock_guard lock(mutex_);
    if (!op.matches(op_) || !expected || expected != current_ || !expected->owner.matches(identity_))
      return false;
    if (sequence_ < sequence_limit_)
      ++sequence_;
    phase_ = Phase::Unknown;
    if (pending_)
      prepared = pending_->prepared_;
  }
  // Either exact side of a racing authority publication. Never lookup arbitrary current facts.
  const bool old_closed = runtime_->close_exact(op, expected->facts);
  const bool new_closed = prepared && runtime_->close_exact(op, prepared->facts);
  return old_closed || new_closed;
}
PublicationHandle PairingMutationOwner::prepare(const UpdateHandle &update,
                                                const state::PairedClientList &clients,
                                                std::span<const access::RuleView> rules) {
  {
    std::lock_guard lock(mutex_);
    if (!update || update != pending_ || phase_ != Phase::Preparing || update->sequence_ != sequence_ ||
        pending_->preparation_claimed_)
      return {};
    pending_->preparation_claimed_ = true;
  }
  PublicationHandle prepared;
  try {
    prepared = prepare_publication(clients, rules, update->source_->facts->target_run, update->revision_);
  } catch (...) { /* closed/occupied; no rollback grant */
  }
  std::lock_guard lock(mutex_);
  if (phase_ != Phase::Preparing || update != pending_ || update->sequence_ != sequence_ || !prepared) {
    phase_ = Phase::Unknown;
    return {};
  }
  pending_->prepared_ = prepared;
  return prepared;
}
Result PairingMutationOwner::submit(const UpdateHandle &update,
                                    const PublicationHandle &prepared,
                                    std::unique_ptr<PersistenceOperation> operation,
                                    std::unique_ptr<authority::RetirementResource> resource) {
  // All parameter destruction on rejection/exception is outside the bookkeeping lock.
  auto command = std::unique_ptr<PublicationCommand>(new PublicationCommand);
  command->operation_ = std::move(operation);
  command->facts_ = {prepared ? prepared->facts : nullptr, std::move(resource)};
  command->receipt_ = std::make_shared<PublicationReceipt>();
  {
    std::lock_guard lock(mutex_);
    if (!update || update != pending_ || !prepared || prepared != update->prepared_ || phase_ != Phase::Preparing ||
        update->sequence_ != sequence_ || !command->operation_)
      return Result::Stale;
  }
  command->transaction_ = std::shared_ptr<const Transaction>(
      new Transaction(identity_, update->identity_, update->sequence_, update->source_, prepared));
  command->op_ = op_;
  command->token_ = update->token_;
  {
    std::lock_guard lock(mutex_);
    if (phase_ != Phase::Preparing || pending_ != update || sequence_ != update->sequence_)
      return Result::Stale;
    phase_ = Phase::Retiring;
    receipt_ = command->receipt_;
  }
  RuntimeSubmission result = RuntimeSubmission::Closed;
  try {
    result = runtime_->submit_publication(std::move(command));
  } catch (...) { /* command/captures unwind outside locks; occupied owner remains closed */
  }
  if (result != RuntimeSubmission::Accepted) {
    std::lock_guard lock(mutex_);
    phase_ = Phase::Unknown;
    return Result::Unknown;
  }
  return Result::Accepted; // queue acceptance ONLY
}
identity::Result<identity::VerifiedHttpsIdentity>
PairingMutationOwner::capture_https(const identity::RequestPtr &request,
                                    const PublicationHandle &publication,
                                    identity::PeerCertificateProvider provider) const {
  {
    std::lock_guard lock(mutex_);
    if (!publication || current_ != publication || phase_ != Phase::Current)
      return {identity::Error::Stale, {}};
  }
  auto captured =
      identity::VerifiedHttpsIdentity::capture(request, publication->sealed, publication->revision, provider);
  std::lock_guard lock(mutex_);
  if (current_ != publication || phase_ != Phase::Current)
    return {identity::Error::Stale, {}};
  return captured; // historical only
}
authority::PermitHandle PairingMutationOwner::capture_selection(const control::SeatCandidate &candidate,
                                                                const identity::VerifiedHttpsIdentity &identity,
                                                                const PublicationHandle &publication,
                                                                authority::CaptureProbe probe) const {
  {
    std::lock_guard lock(mutex_);
    if (!publication || current_ != publication || phase_ != Phase::Current ||
        !identity.revision().matches(publication->revision))
      return {};
  }
  return runtime_->capture(candidate, identity.principal(), publication->facts, identity.revision(), probe);
}
} // namespace sessions::protected_pairing
#endif
