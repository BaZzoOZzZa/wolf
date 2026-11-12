#include "protected_runtime.hpp"
#include "protected_pairing.hpp"
#include <stdexcept>

namespace wolf::core::sessions {
bool consumer_legacy_current(const state::SessionsAtoms &, const ConnectionScope &);
ConsumerComposition::ConsumerComposition(state::SessionsAtoms running, streaming::ConsumerPolicy policy)
    : view_(std::make_shared<View>()) {
  if (!running)
    throw std::invalid_argument("Consumer composition requires actual running sessions");
  runtime_ = std::make_shared<streaming::ConsumerRuntime>(
      [running, view = view_](const auto &scope) {
        if (!scope)
          return false;
        auto snapshot = view->snapshot.load();
        if (snapshot)
          for (const auto &s : snapshot->scopes)
            if (s.matches(scope))
              return true;
        return consumer_legacy_current(running, scope);
      },
      policy);
}
std::shared_ptr<ConsumerComposition> make_consumer_composition(state::SessionsAtoms running) {
  return std::make_shared<ConsumerComposition>(std::move(running));
}
std::shared_ptr<streaming::ConsumerRuntime> consumer_runtime(const std::shared_ptr<ConsumerComposition> &composition) {
  if (!composition)
    throw std::invalid_argument("Missing consumer composition");
  return composition->runtime();
}
bool ConsumerComposition::contains(const streaming::ConnectionScope &scope) const {
  auto snapshot = view_->snapshot.load();
  return snapshot && (snapshot->scopes[0].matches(scope) || snapshot->scopes[1].matches(scope));
}
} // namespace wolf::core::sessions
#ifdef __linux__
namespace sessions {
using namespace std::chrono_literals;
struct ProtectedRuntimeOwner::State {
  struct Cell {
    std::unique_ptr<RuntimeJob> job;
    std::unique_ptr<protected_pairing::PublicationCommand> publication;
    std::shared_ptr<RuntimeReceipt> receipt;
    size_t charge = 0;
    bool occupied = false, typed_publication = false;
  };
  const std::shared_ptr<ConsumerComposition> composition;
  std::shared_ptr<const RuntimeInspectionProbe> probe; // immutable operations, lifecycle-owned storage
  mutable std::mutex mutex;
  std::condition_variable wake;
  std::array<Cell, 4> cells;
  std::array<unsigned, 4> order{};
  size_t queued = 0;
  std::atomic_bool stop{false}, unknown{false}, started{false}, execution_failed{false};
  // Completed receipt storage remains retained in each cell. Reserve it even
  // while empty, separately from the new in-flight receipt charged per job.
  size_t bytes = sizeof(cells) + sizeof(order) + 4 * sizeof(RuntimeReceipt) + 2 * sizeof(streaming::ConnectionScope) +
                 sizeof(std::optional<streaming::ConsumerDrainTicket>);
  protected_authority::OperatorCapability op;
  protected_authority::FactHandle facts;
  std::unique_ptr<protected_authority::ProtectedAuthorityDomain> authority;
  std::shared_ptr<control::InputSeat> seat;
  std::unique_ptr<InputHandoff> handoff;
  std::weak_ptr<const TransferHandle> transfer;
  // Bounded data-only health observations for typed facts publication. Never Current/authority.
  std::array<streaming::ConnectionScope, 2> consumer_scopes;
  std::optional<streaming::ConsumerDrainTicket> observed_drain;
  // Fixed candidate rollback obligation, never inferred from a missing status.
  std::optional<streaming::ConsumerDrainTicket> failed_media;
  State(std::shared_ptr<ConsumerComposition> c, std::shared_ptr<const RuntimeInspectionProbe> p)
      : composition(std::move(c)), probe(std::move(p)) {}
};
ProtectedRuntimeOwner::ProtectedRuntimeOwner(std::shared_ptr<ConsumerComposition> c,
                                             std::shared_ptr<const RuntimeInspectionProbe> probe) {
  if (!c)
    throw std::invalid_argument("Protected owner requires shared consumer composition");
  state_ = std::make_unique<State>(std::move(c), std::move(probe));
}
bool ProtectedRuntimeOwner::start(RuntimeBootstrap bootstrap, control::SeatThreadFactory start_thread) {
  if (attempted_ || state_->stop)
    return false;
  attempted_ = true;
  auto &s = *state_;
  if (s.composition->view_->reserved.exchange(true)) {
    s.execution_failed = s.unknown = true;
    return false;
  }
  try {
    if (!start_thread || !bootstrap.facts.facts || !bootstrap.initial.valid() ||
        !bootstrap.initial.destination.matches(bootstrap.facts.facts->target_run))
      throw std::invalid_argument("Unresolved protected bootstrap binding");
    s.op = bootstrap.op;
    s.facts = bootstrap.facts.facts;
    s.authority =
        std::make_unique<protected_authority::ProtectedAuthorityDomain>(std::move(bootstrap.facts), bootstrap.op);
    s.seat = s.authority->create_seat(bootstrap.initial,
                                      bootstrap.principal,
                                      bootstrap.inventory,
                                      std::move(bootstrap.backend),
                                      std::move(bootstrap.validate));
    if (!s.seat)
      throw std::runtime_error("Protected bootstrap refused");
    auto snapshot = std::make_shared<ConsumerComposition::Lifetimes>();
    snapshot->scopes[0] = bootstrap.initial.connection;
    s.consumer_scopes[0] = bootstrap.initial.connection;
    s.composition->view_->snapshot.store(std::move(snapshot));
    s.handoff = std::make_unique<InputHandoff>(s.seat, s.composition->runtime(), std::move(bootstrap.now));
    auto done = std::make_shared<std::promise<void>>();
    exited_ = done->get_future().share();
    worker_ = start_thread([this, done] {
      try {
        if (state_->probe)
          state_->probe->before_worker_run();
        run();
      } catch (...) {
        state_->execution_failed = state_->unknown = true;
        request_stop();
      }
      done->set_value_at_thread_exit(); // ready only after thread/TLS exit
    });
    if (!worker_.joinable())
      throw std::runtime_error("Unowned orchestration thread");
    s.started = true;
    return true;
  } catch (...) {
    s.execution_failed = s.unknown = true;
    request_stop();
    return false; // start cap retained, never replacement/retry
  }
}
RuntimeSubmitResult ProtectedRuntimeOwner::submit(std::unique_ptr<RuntimeJob> job) {
  if (!job || unsigned(job->cell) >= 4)
    return {RuntimeSubmission::Invalid, {}};
  constexpr size_t limit = 64 * 1024;
  // Validate capacities BEFORE addition (overflow) or any internal copy/allocation.
  if (job->payload.capacity() > limit || job->correlation.ip.capacity() > 256)
    return {RuntimeSubmission::Capacity, {}};
  size_t scope_bytes = 0;
  for (const auto *scope : {&job->selection.candidate.source, &job->selection.candidate.destination, &job->producer}) {
    if (scope->id().capacity() > 256)
      return {RuntimeSubmission::Capacity, {}};
    scope_bytes += scope->id().capacity();
  }
  const size_t charge = scope_bytes + sizeof(RuntimeJob) + sizeof(RuntimeReceipt) + job->payload.capacity() +
                        job->correlation.ip.capacity();
  auto &s = *state_;
  auto receipt = std::make_shared<RuntimeReceipt>();
  receipt->identity = RuntimeJobIdentity::fresh(unsigned(job->cell));
  std::shared_ptr<RuntimeReceipt> retired;
  // Observe under Seat gate BEFORE queue lock; never retain a validator owner
  // across queue publication, notification or caller return.
  const auto observed = s.handoff ? s.handoff->observe() : std::optional<TransferObservation>{};
  {
    std::lock_guard lock(s.mutex);
    if (observed && observed->status == TransferStatus::Quarantined)
      s.unknown = true;
    if (!s.started || s.stop || s.unknown)
      return {RuntimeSubmission::Closed, {}};
    if (s.cells[3].occupied && s.cells[3].typed_publication)
      return {RuntimeSubmission::Busy, {}};
    // A preparation cannot retain old media owners across Handoff replacement.
    // Refuse before effects; existing media cleanup (including popped) stays charged.
    if ((job->cell == RuntimeCell::Preparation && (s.cells[1].occupied || s.cells[2].occupied)) ||
        ((job->cell == RuntimeCell::Audio || job->cell == RuntimeCell::Video) && s.cells[0].occupied))
      return {RuntimeSubmission::Busy, {}};
    auto &cell = s.cells[unsigned(job->cell)];
    if (cell.occupied)
      return {RuntimeSubmission::Busy, {}};
    if (charge > limit - s.bytes)
      return {RuntimeSubmission::Capacity, {}};
    cell.occupied = true;
    cell.charge = charge;
    s.bytes += charge;
    retired = std::move(cell.receipt);
    cell.receipt = receipt;
    cell.job.swap(job); // zero copy; no arbitrary capture destruction under lock
    s.order[s.queued++] = unsigned(cell.receipt->identity.id()); // fixed FIFO, no growing history
  }
  s.wake.notify_one();
  if (s.probe)
    s.probe->after_submit_publish();
  return {RuntimeSubmission::Accepted, std::move(receipt)};
}
protected_authority::UpdateResult
ProtectedRuntimeOwner::begin_authority_update(const protected_authority::OperatorCapability &op,
                                              const protected_authority::FactHandle &expected) {
  return state_->authority ? state_->authority->begin_update(op, expected)
                           : protected_authority::UpdateResult{protected_authority::Result::Unknown, {}};
}
bool ProtectedRuntimeOwner::consumer_execution_known() const {
  auto &s = *state_;
  std::array<streaming::ConnectionScope, 2> scopes;
  std::optional<streaming::ConsumerDrainTicket> drain;
  {
    std::lock_guard lock(s.mutex);
    scopes = s.consumer_scopes;
    drain = s.observed_drain;
  }
  // No queue -> Runtime/job lock nesting. Missing status is NOT release proof;
  // exact retained ticket still detects a pruned failed/broken completion.
  if (drain &&
      (!drain->released.valid() || (drain->released.wait_for(0ms) == std::future_status::ready && !drain->complete())))
    return false;
  for (const auto &scope : scopes) {
    if (!scope)
      continue;
    for (const auto kind : {streaming::ConsumerKind::Audio, streaming::ConsumerKind::Video}) {
      const auto status = s.composition->runtime()->status(scope, kind);
      if (status &&
          (status->state == streaming::ConsumerState::Unknown || status->state == streaming::ConsumerState::Failed))
        return false;
    }
  }
  return true; // absence of observed fault, not actual drain/physical completion
}
bool ProtectedRuntimeOwner::publication_ready(
    const protected_authority::OperatorCapability &op,
    const std::shared_ptr<const protected_authority::UpdateToken> &token) const {
  auto &s = *state_;
  if (!s.authority || !s.authority->publication_ready(op, token) || !consumer_execution_known())
    return false;
  std::lock_guard lock(s.mutex);
  if (!s.started || s.stop || s.execution_failed)
    return false;
  for (const auto &cell : s.cells)
    if (cell.occupied)
      return false;
  return true; // observation only; admission/execution recheck
}
RuntimeSubmission
ProtectedRuntimeOwner::submit_publication(std::unique_ptr<protected_pairing::PublicationCommand> command) {
  auto &s = *state_;
  if (!command || !command->transaction_ || !command->operation_ || !command->receipt_ || !command->facts_.facts ||
      command->facts_.facts != command->transaction_->result->facts)
    return RuntimeSubmission::Invalid;
  // Do not infer execution health from public permission Unknown. All actual
  // worker/start/job failures latch execution_failed (never reset).
  if (!s.authority || !s.authority->publication_ready(command->op_, command->token_) || !consumer_execution_known())
    return RuntimeSubmission::Closed;
  constexpr size_t charge = sizeof(protected_pairing::PublicationCommand) + sizeof(protected_pairing::Transaction) +
                            sizeof(protected_pairing::PublicationReceipt) + sizeof(RuntimeReceipt);
  auto receipt = std::make_shared<RuntimeReceipt>();
  receipt->identity = RuntimeJobIdentity::fresh(unsigned(RuntimeCell::Retirement));
  std::shared_ptr<RuntimeReceipt> retired;
  {
    std::lock_guard lock(s.mutex);
    if (!s.started || s.stop || s.execution_failed)
      return RuntimeSubmission::Closed;
    for (const auto &cell : s.cells)
      if (cell.occupied)
        return RuntimeSubmission::Busy; // popped/Unknown resource is NOT healthy execution evidence
    if (charge > 64 * 1024 - s.bytes)
      return RuntimeSubmission::Capacity;
    auto &cell = s.cells[unsigned(RuntimeCell::Retirement)];
    cell.occupied = true;
    cell.charge = charge;
    s.bytes += charge;
    retired = std::move(cell.receipt);
    cell.receipt = std::move(receipt);
    cell.typed_publication = true;
    cell.publication.swap(command);
    s.order[s.queued++] = unsigned(RuntimeCell::Retirement);
  }
  s.wake.notify_one();
  return RuntimeSubmission::Accepted;
}
void ProtectedRuntimeOwner::run() {
  auto &s = *state_;
  for (;;) {
    std::unique_ptr<RuntimeJob> job;
    std::unique_ptr<protected_pairing::PublicationCommand> publication;
    std::shared_ptr<RuntimeReceipt> receipt;
    unsigned index = 0;
    {
      std::unique_lock lock(s.mutex);
      s.wake.wait(lock, [&] { return s.queued || s.stop.load(); });
      if (!s.queued)
        break;
      index = s.order[0];
      for (size_t i = 1; i < s.queued; ++i)
        s.order[i - 1] = s.order[i];
      --s.queued;
      auto &c = s.cells[index];
      job = std::move(c.job);
      publication = std::move(c.publication);
      receipt = c.receipt;
      receipt->state = RuntimeJobState::Executing;
    }
    if (publication) {
      const auto completion = publication->receipt_; // DATA ONLY; outlives command release
      completion->state = RuntimeJobState::Executing;
      bool succeeded = false;
      try {
        bool healthy;
        {
          std::lock_guard lock(s.mutex);
          healthy = !s.stop && !s.execution_failed;
          for (unsigned i = 0; i < s.cells.size(); ++i)
            if (i != index && s.cells[i].occupied)
              healthy = false;
        }
        if (healthy && consumer_execution_known() &&
            s.authority->publication_ready(publication->op_, publication->token_)) {
          // Outcome scope MUST end before command reset/final completion.
          const auto outcome = publication->operation_->execute(*publication->transaction_);
          if (publication->transaction_->matches(outcome) &&
              outcome.result == protected_pairing::PersistenceResult::Checked && !s.stop && !s.execution_failed &&
              consumer_execution_known() && s.authority->publication_ready(publication->op_, publication->token_)) {
            const auto result = s.authority->publish_prepared(publication->token_, std::move(publication->facts_));
            completion->authority_result = result;
            succeeded = result == protected_authority::Result::Accepted;
          }
        }
      } catch (...) { /* uncertain provider/retirement; retained below, never success */
      }
      if (!succeeded) {
        s.execution_failed = s.unknown = true;
        request_stop();
        std::lock_guard lock(s.mutex);
        s.cells[index].publication.swap(publication);
        receipt->state = RuntimeJobState::Unknown;
        completion->state = RuntimeJobState::Unknown;
      } else {
        publication.reset(); // provider, resource, transaction temporaries retire OUTSIDE ALL locks
        std::lock_guard lock(s.mutex);
        auto &cell = s.cells[index];
        s.bytes -= cell.charge;
        cell.charge = 0;
        cell.occupied = false;
        cell.typed_publication = false;
        receipt->state = RuntimeJobState::Complete;
        completion->state = RuntimeJobState::Complete; // local facts completion only; no Unknown reset
      }
      continue;
    }
    std::shared_ptr<const TransferHandle> attempted_media;
    bool failed = false, refused = s.stop || s.unknown;
    try {
      if (!refused && job->cell == RuntimeCell::Preparation) {
        auto &selection = job->selection;
        auto expected = s.seat->current();
        if (!job->transfer || !expected || !selection.expected_old ||
            !expected->token.matches(selection.expected_old->token) || !selection.authority ||
            !selection.candidate.valid() || !selection.candidate.destination.matches(expected->owner.destination) ||
            selection.candidate.connection.matches(expected->owner.connection) ||
            selection.candidate.peer.matches(expected->owner.peer)) {
          refused = true;
        } else {
          // Old stays recognized while real request captures its exact ticket.
          auto result = s.handoff->request(job->transfer, std::move(selection));
          if (result.admission != TransferAdmission::Accepted && result.admission != TransferAdmission::Duplicate) {
            refused = true;
            failed = result.admission == TransferAdmission::Quarantined;
          } else {
            {
              std::lock_guard lock(s.mutex);
              s.transfer = result.handle; // weak diagnostic only: no extra validator owner
            }
            auto ticket = result.handle->saved_ticket();
            if (!ticket || !ticket->connection.matches(expected->owner.connection))
              throw std::runtime_error("Missing exact old drain ticket");
            {
              std::lock_guard lock(s.mutex);
              s.consumer_scopes = {expected->owner.connection, result.handle->prepared_grant->owner.connection};
              s.observed_drain = ticket; // only future<void>/identity, no validator/TransferHandle owner
            }
            // Publication retires lifetime, never the retained Runtime ticket.
            auto next = std::make_shared<ConsumerComposition::Lifetimes>();
            next->scopes[0] = result.handle->prepared_grant->owner.connection;
            s.composition->view_->snapshot.store(std::move(next));
          }
        }
      } else if (!refused && (job->cell == RuntimeCell::Audio || job->cell == RuntimeCell::Video)) {
        std::shared_ptr<const TransferHandle> exact;
        {
          std::lock_guard lock(s.mutex);
          exact = s.transfer.lock();
        }
        if (!exact || job->handle != exact || !job->producer.matches(exact->prepared_grant->owner.destination) ||
            !job->work) {
          refused = true;
        } else {
          attempted_media = exact;
          const auto &candidate = exact->prepared_grant->owner;
          streaming::ConsumerRuntime::Work work;
          work.swap(job->work);
          std::function<void()> hook;
          hook.swap(job->before_cleanup);
          const auto result = s.handoff->admit_prepared(
              exact,
              candidate.connection,
              candidate.peer,
              job->cell == RuntimeCell::Audio ? streaming::ConsumerKind::Audio : streaming::ConsumerKind::Video,
              std::move(job->correlation),
              std::move(work),
              std::move(hook));
          receipt->media = result;
          if (result != streaming::ConsumerAdmission::Accepted && result != streaming::ConsumerAdmission::Duplicate) {
            s.handoff->candidate_gone(candidate.connection, candidate.peer);
            s.failed_media = s.composition->runtime()->revoke(candidate.connection);
            failed = true;
          }
        }
      } else if (!refused && job->cell == RuntimeCell::Retirement) {
        if (job->retire)
          job->retire();
        else
          refused = true;
      }
    } catch (...) {
      failed = true;
      if (attempted_media) {
        const auto &candidate = attempted_media->prepared_grant->owner;
        s.handoff->candidate_gone(candidate.connection, candidate.peer);
        try {
          s.failed_media = s.composition->runtime()->revoke(candidate.connection);
        } catch (...) { /* Missing ticket is Unknown, never successful release. Composition stays retained. */
        }
      }
    }
    attempted_media.reset(); // before cell release/next preparation; outside all locks
    if (failed) {
      s.execution_failed = s.unknown = true;
      request_stop();
      // Failed callbacks can retain unproved resources. Keep their owner in this
      // occupied cell until lifecycle teardown; no retry or successful ack.
      std::lock_guard lock(s.mutex);
      s.cells[index].job.swap(job);
      receipt->state = RuntimeJobState::Unknown;
    } else {
      job.reset(); // may block/reenter; queue/status/close are independent
      std::lock_guard lock(s.mutex);
      auto &cell = s.cells[index];
      s.bytes -= cell.charge;
      cell.charge = 0;
      cell.occupied = false;
      receipt->state = refused ? RuntimeJobState::Refused : RuntimeJobState::Complete;
    }
  }
}
RuntimeOwnerStatus ProtectedRuntimeOwner::status() const {
  auto &s = *state_;
  // Never queue -> Seat. Responsive decision when observed, not an autonomous
  // timer if all callers (including the existing supervisor) are blocked/idle.
  if (s.handoff)
    s.handoff->poll_deadline();
  const auto observed = s.handoff ? s.handoff->observe() : std::optional<TransferObservation>{};
  RuntimeOwnerStatus result{};
  const bool authority_unknown = s.authority && s.authority->status().quarantined;
  {
    std::lock_guard lock(s.mutex);
    size_t occupied = 0;
    for (const auto &c : s.cells)
      occupied += c.occupied;
    result = {s.started, s.stop, s.unknown, joined_, occupied, s.bytes};
  }
  if (s.probe)
    s.probe->after_status_inspection();
  result.unknown = result.unknown || authority_unknown || (observed && observed->status == TransferStatus::Quarantined);
  return result;
}
std::shared_ptr<const TransferHandle> ProtectedRuntimeOwner::transfer() const {
  std::lock_guard lock(state_->mutex);
  return state_->transfer.lock();
}
control::GrantHandle ProtectedRuntimeOwner::current() const {
  return state_->seat ? state_->seat->current() : nullptr;
}
protected_authority::PermitHandle ProtectedRuntimeOwner::capture(const control::SeatCandidate &c,
                                                                 const protected_access::Principal &p) const {
  return state_->authority ? state_->authority->capture_selection(c, p) : nullptr;
}
protected_authority::PermitHandle ProtectedRuntimeOwner::capture(const control::SeatCandidate &candidate,
                                                                 const protected_access::Principal &principal,
                                                                 const protected_authority::FactHandle &expected,
                                                                 const protected_authority::PairingRevision &pairing,
                                                                 protected_authority::CaptureProbe probe) const {
  return state_->authority ? state_->authority->capture_selection(candidate, principal, expected, pairing, probe)
                           : nullptr;
}
bool ProtectedRuntimeOwner::close_exact(const control::ConnectionScope &c, const control::PeerToken &p) {
  return state_->authority && state_->authority->close_exact(c, p);
}
bool ProtectedRuntimeOwner::close_exact(const protected_authority::OperatorCapability &op,
                                        const protected_authority::FactHandle &facts) {
  return state_->authority && state_->authority->close_exact(op, facts);
}
void ProtectedRuntimeOwner::request_stop() {
  auto &s = *state_;
  if (s.stop.exchange(true))
    return;
  if (s.authority)
    s.authority->close_exact(s.op, s.facts); // lifecycle capability, includes Pending/bootstrap
  s.wake.notify_all();
}
RuntimeJoin ProtectedRuntimeOwner::join_when_exited() {
  if (joined_)
    return RuntimeJoin::Joined;
  if (!worker_.joinable())
    return RuntimeJoin::Refused;
  if (!exited_.valid() || exited_.wait_for(0ms) != std::future_status::ready)
    return RuntimeJoin::Unknown;
  worker_.join();
  joined_ = true;
  return RuntimeJoin::Joined;
}
const std::shared_ptr<streaming::ConsumerRuntime> &ProtectedRuntimeOwner::runtime() const {
  return state_->composition->runtime();
}
ProtectedRuntimeOwner::~ProtectedRuntimeOwner() {
  request_stop();
  if (worker_.joinable())
    worker_.join();
  // Payload retirement may inspect the still-live bundle. Never hold queue lock.
  for (auto &cell : state_->cells)
    cell.job.reset();
  if (state_->probe)
    state_->probe->before_bundle_teardown();
  // Probe captures retire while State is complete, never a per-call last release.
  state_->probe.reset();
  state_->handoff.reset();
  state_->seat.reset();
  state_->authority.reset();
}
} // namespace sessions
#endif
