#include "protected_runtime.hpp"
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
    std::shared_ptr<RuntimeReceipt> receipt;
    size_t charge = 0;
    bool occupied = false;
  };
  const std::shared_ptr<ConsumerComposition> composition;
  mutable std::mutex mutex;
  std::condition_variable wake;
  std::array<Cell, 4> cells;
  std::atomic_bool stop{false}, unknown{false}, started{false};
  size_t bytes = sizeof(cells);
  protected_authority::OperatorCapability op;
  protected_authority::FactHandle facts;
  std::unique_ptr<protected_authority::ProtectedAuthorityDomain> authority;
  std::shared_ptr<control::InputSeat> seat;
  std::unique_ptr<InputHandoff> handoff;
  std::weak_ptr<const TransferHandle> transfer;
  // Fixed candidate rollback obligation, never inferred from a missing status.
  std::optional<streaming::ConsumerDrainTicket> failed_media;
  explicit State(std::shared_ptr<ConsumerComposition> c) : composition(std::move(c)) {}
};
ProtectedRuntimeOwner::ProtectedRuntimeOwner(std::shared_ptr<ConsumerComposition> c) {
  if (!c)
    throw std::invalid_argument("Protected owner requires shared consumer composition");
  state_ = std::make_unique<State>(std::move(c));
}
bool ProtectedRuntimeOwner::start(RuntimeBootstrap bootstrap, control::SeatThreadFactory start_thread) {
  if (attempted_ || state_->stop)
    return false;
  attempted_ = true;
  auto &s = *state_;
  if (s.composition->view_->reserved.exchange(true)) {
    s.unknown = true;
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
    s.composition->view_->snapshot.store(std::move(snapshot));
    s.handoff = std::make_unique<InputHandoff>(s.seat, s.composition->runtime(), std::move(bootstrap.now));
    auto done = std::make_shared<std::promise<void>>();
    exited_ = done->get_future().share();
    worker_ = start_thread([this, done] {
      try {
        run();
      } catch (...) {
        state_->unknown = true;
        request_stop();
      }
      done->set_value_at_thread_exit(); // ready only after thread/TLS exit
    });
    if (!worker_.joinable())
      throw std::runtime_error("Unowned orchestration thread");
    s.started = true;
    return true;
  } catch (...) {
    s.unknown = true;
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
  {
    std::lock_guard lock(s.mutex);
    if (!s.started || s.stop || s.unknown)
      return {RuntimeSubmission::Closed, {}};
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
  }
  s.wake.notify_one();
  return {RuntimeSubmission::Accepted, std::move(receipt)};
}
void ProtectedRuntimeOwner::run() {
  auto &s = *state_;
  for (;;) {
    std::unique_ptr<RuntimeJob> job;
    std::shared_ptr<RuntimeReceipt> receipt;
    unsigned index = 0;
    {
      std::unique_lock lock(s.mutex);
      s.wake.wait(lock, [&] {
        for (auto &c : s.cells)
          if (c.job && c.receipt->state == RuntimeJobState::Queued)
            return true;
        return s.stop.load();
      });
      bool found = false;
      for (unsigned i = 0; i < 4; ++i) {
        auto &c = s.cells[i];
        if (c.job && c.receipt->state == RuntimeJobState::Queued) {
          index = i;
          job = std::move(c.job);
          receipt = c.receipt;
          receipt->state = RuntimeJobState::Executing;
          found = true;
          break;
        }
      }
      if (!found)
        break;
    }
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
    }
    if (failed) {
      s.unknown = true;
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
  std::lock_guard lock(s.mutex);
  size_t occupied = 0;
  for (const auto &c : s.cells)
    occupied += c.occupied;
  return {s.started, s.stop, s.unknown, joined_, occupied, s.bytes};
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
  state_->handoff.reset();
  state_->seat.reset();
  state_->authority.reset();
}
} // namespace sessions
#endif
