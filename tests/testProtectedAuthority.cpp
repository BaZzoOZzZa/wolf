#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <future>
#include <sessions/input_handoff.hpp>
#include <sessions/protected_access.hpp>
#include <sessions/protected_authority.hpp>

namespace pa = sessions::protected_authority;
namespace facts_api = sessions::protected_access;
using namespace control;
using namespace sessions;
using namespace streaming;
using namespace std::chrono_literals;
namespace {
using Clock = std::chrono::steady_clock;
bool eventually(const std::function<bool()> &fn) {
  const auto until = Clock::now() + 2s;
  while (!fn() && Clock::now() < until)
    std::this_thread::sleep_for(1ms);
  return fn();
}
struct TestGate {
  std::mutex mutex;
  std::condition_variable cv;
  std::atomic_bool entered{false};
  bool released = false;
  void wait() {
    std::unique_lock lock(mutex);
    entered = true;
    cv.wait_for(lock, 4s, [&] { return released; }); // bounded failure unwind
  }
  void release() {
    std::lock_guard lock(mutex);
    released = true;
    cv.notify_all();
  }
};
struct Source : facts_api::VerifiedIdentitySource {
  x509::x509_ptr cert;
  explicit Source(x509::x509_ptr cert) : cert(std::move(cert)) {}
  facts_api::VerifiedLeafSnapshot verified_leaf() const override {
    return {cert, cert, 7};
  }
};
const x509::x509_ptr &certificate() {
  static const auto cert = x509::generate_x509(x509::generate_key());
  return cert;
}
facts_api::Principal principal() {
  auto p = facts_api::Principal::capture(Source(certificate()));
  if (!p)
    throw std::runtime_error("certificate capture failed");
  return *p;
}
pa::FactHandle facts(const RunScope &run, std::string_view app = "42", bool allowed = true) {
  auto leaf = facts_api::Leaf::from_x509(certificate());
  if (!leaf)
    throw std::runtime_error("DER failed");
  std::array<facts_api::PinView, 1> pins{{{*leaf, 7}}};
  const std::span<const facts_api::PinView> acl = allowed ? std::span<const facts_api::PinView>(pins)
                                                          : std::span<const facts_api::PinView>{};
  std::array<facts_api::RuleView, 1> rules{{{"game", app, acl}}};
  return pa::Facts::prepare(rules, "game", app, run, pa::PairingRevision::fresh(0));
}
struct IO {
  TestGate gate;
  std::atomic_bool block{false}, failed{false};
  std::atomic_size_t writes{0};
  inputtino::checked::detail::Io ops() {
    return {
        [this](int fd, const input_event &, size_t size) {
          if (fd != 42 || size != sizeof(input_event))
            throw std::runtime_error("not injected IO");
          if (block.exchange(false))
            gate.wait();
          ++writes;
          return failed ? inputtino::checked::IoResult{-1, EAGAIN}
                        : inputtino::checked::IoResult{std::ptrdiff_t(size), 0};
        },
        [](int, int) {
          std::this_thread::sleep_for(1ms);
          return inputtino::checked::IoResult{0, 0};
        },
        [](int, input_event &, size_t) -> inputtino::checked::IoResult { throw std::runtime_error("unexpected read"); },
        [](int, unsigned long, void *) -> int { throw std::runtime_error("unexpected ioctl"); },
        [] { return Clock::now(); }};
  }
};
struct Current {
  std::mutex mutex;
  std::vector<ConnectionScope> scopes;
  bool contains(const ConnectionScope &s) {
    std::lock_guard lock(mutex);
    return std::any_of(scopes.begin(), scopes.end(), [&](auto &v) { return v.matches(s); });
  }
};
struct Fixture {
  RunScope destination = RunScope::fresh("L");
  SeatCandidate initial{RunScope::fresh("42"),
                        destination,
                        ConnectionScope::fresh(42),
                        PeerToken::fresh(0),
                        BindingRevision::fresh(0),
                        0};
  SeatCandidate next{RunScope::fresh("42"),
                     destination,
                     ConnectionScope::fresh(42),
                     PeerToken::fresh(0),
                     BindingRevision::fresh(0),
                     0};
  std::shared_ptr<IO> io = std::make_shared<IO>();
  std::shared_ptr<Current> current = std::make_shared<Current>();
  std::shared_ptr<std::atomic<int64_t>> clock = std::make_shared<std::atomic<int64_t>>(10000);
  pa::OperatorCapability op = pa::OperatorCapability::fresh(0);
  pa::FactHandle original = facts(destination);
  std::unique_ptr<pa::ProtectedAuthorityDomain> authority;
  std::shared_ptr<InputSeat> seat;
  std::shared_ptr<ConsumerRuntime> runtime;
  std::unique_ptr<InputHandoff> handoff;
  GrantHandle old;
  bool before_factory = false, before_worker = false;
  std::vector<std::shared_ptr<TestGate>> gates;
  Fixture(bool block_bootstrap = false,
          SeatValidation validation = {},
          std::unique_ptr<pa::RetirementResource> resource = {},
          uint64_t limit = UINT64_MAX) {
    authority =
        std::make_unique<pa::ProtectedAuthorityDomain>(pa::PreparedFacts{original, std::move(resource)}, op, limit);
    current->scopes = {initial.connection, next.connection};
    ConsumerPolicy policy;
    policy.poll_interval = 1ms;
    runtime =
        std::make_shared<ConsumerRuntime>([c = current](const auto &scope) { return c->contains(scope); }, policy);
    io->block = block_bootstrap;
    seat = authority->create_seat(
        initial,
        principal(),
        {true, true, true},
        [&](auto callback) {
          before_factory = authority->status().installed;
          callback(4, 5); // bootstrap callback cannot publish feedback/input authority
          auto xbox = inputtino::checked::Xbox::adopt({io, 42}, io->ops(), std::move(callback), [](auto work) {
            return std::thread(std::move(work));
          });
          return std::make_shared<CheckedXboxBackend>(std::move(xbox));
        },
        std::move(validation),
        {},
        [&](auto work) {
          before_worker = authority->status().installed;
          return std::thread(std::move(work));
        });
    if (!block_bootstrap) {
      REQUIRE(eventually([&] { return bool(seat->current()); }));
      old = seat->current();
    }
    handoff = std::make_unique<InputHandoff>(seat, runtime, [time = clock] {
      return Clock::time_point{} + std::chrono::milliseconds(time->load());
    });
  }
  void release() {
    io->gate.release();
    for (auto &g : gates)
      g->release();
  }
  ~Fixture() {
    release();
    handoff.reset();
    seat.reset();
    authority.reset();
    runtime.reset();
  }
  AuthorizedSelection selection(SeatValidation additional = {}) {
    return {old, next, std::move(additional), authority->capture_selection(next, principal())};
  }
  TransferResult transfer(SeatValidation additional = {}) {
    return handoff->request(TransferToken::fresh(0), selection(std::move(additional)));
  }
  SeatCommand command(GrantHandle grant) {
    SeatCommand c;
    c.grant = std::move(grant);
    c.frame.lx = 900;
    return c;
  }
  std::shared_ptr<TestGate> consumer() {
    auto g = std::make_shared<TestGate>();
    gates.push_back(g);
    REQUIRE(runtime->admit(initial.connection,
                           initial.source,
                           ConsumerKind::Video,
                           {"local", 12, {}},
                           [g](auto &job, const auto &) {
                             job.begin_drain();
                             g->wait();
                             return true;
                           }) == ConsumerAdmission::Accepted);
    runtime->ping(ConsumerKind::Video, {"local", 12, std::array<char, 16>{}, {}});
    REQUIRE(eventually([&] { return g->entered.load(); }));
    return g;
  }
};
struct Joiner {
  Fixture &f;
  std::vector<std::thread> threads;
  ~Joiner() {
    f.release();
    for (auto &t : threads)
      if (t.joinable())
        t.join();
  }
};
struct Capture {
  std::shared_ptr<TestGate> gate;
  std::function<void()> after;
  ~Capture() {
    gate->wait();
    if (after)
      after();
  }
};
struct Resource : pa::RetirementResource {
  std::shared_ptr<TestGate> gate;
  std::atomic_bool *destroyed;
  bool fail;
  Resource(std::shared_ptr<TestGate> g, std::atomic_bool *d, bool fail = false)
      : gate(std::move(g)), destroyed(d), fail(fail) {}
  void retire() override {
    if (fail)
      throw std::runtime_error("unproved provider retirement");
  }
  ~Resource() override {
    if (gate)
      gate->wait();
    *destroyed = true;
  }
};
} // namespace

TEST_CASE("Protected authority is installed before backend and worker and bootstrap close wins",
          "[protected-authority]") {
  Fixture f(true, [](const auto &) { return true; });
  REQUIRE(f.before_factory);
  REQUIRE(f.before_worker);
  REQUIRE(eventually([&] { return f.io->gate.entered.load(); }));
  CHECK_FALSE(f.seat->current());
  REQUIRE(f.authority->close_exact(f.op, f.original));
  f.io->gate.release();
  REQUIRE(eventually([&] { return f.seat->quarantined(); }));
  CHECK_FALSE(f.seat->current());
  CHECK_FALSE(f.authority->capture_selection(f.next, principal()));
}
TEST_CASE("Protected authority exact DER and private facts cannot be replaced by a true validator",
          "[protected-authority]") {
  Fixture f;
  auto historical = f.original->evaluate(principal()).evidence;
  REQUIRE(f.original->locally_current(historical));
  auto s = f.selection([](const auto &) { return true; });
  REQUIRE(s.authority);
  s.authority.reset();
  CHECK(f.handoff->request(TransferToken::fresh(0), std::move(s)).admission == TransferAdmission::Invalid);
  CHECK(f.seat->current()->token.matches(f.old->token));
  auto valid = f.selection([](const auto &) { return true; });
  REQUIRE(f.authority->close_exact(f.op, f.original));
  CHECK(f.original->locally_current(historical)); // historical facts still true, effect authority gone
  CHECK(f.handoff->request(TransferToken::fresh(0), std::move(valid)).admission == TransferAdmission::Quarantined);
  CHECK(f.seat->submit(f.command(f.old)) == SeatAdmission::Quarantined);
}
TEST_CASE(
    "Protected null foreign same-value revisions and candidate bindings fail closed without revoking healthy owner",
    "[protected-authority]") {
  Fixture f;
  auto equal = facts(f.destination);
  REQUIRE(equal);
  REQUIRE(equal != f.original);
  CHECK_FALSE(f.authority->close_exact(f.op, equal));
  CHECK_FALSE(f.authority->close_exact({}, f.original));
  CHECK_FALSE(f.authority->close_exact(f.op, {}));
  CHECK_FALSE(f.authority->close_exact(ConnectionScope{}, PeerToken{}));
  Fixture foreign;
  auto s = f.selection();
  s.authority = foreign.authority->capture_selection(foreign.next, principal());
  CHECK(f.handoff->request(TransferToken::fresh(0), std::move(s)).admission == TransferAdmission::Invalid);
  s = f.selection();
  s.candidate.binding = BindingRevision::fresh(0);
  CHECK(f.handoff->request(TransferToken::fresh(0), std::move(s)).admission == TransferAdmission::Invalid);
  s = f.selection();
  s.candidate.peer = PeerToken::fresh(0);
  CHECK(f.handoff->request(TransferToken::fresh(0), std::move(s)).admission == TransferAdmission::Invalid);
  CHECK(f.seat->current()->token.matches(f.old->token));
  auto changed = x509::generate_x509(x509::generate_key());
  auto p = facts_api::Principal::capture(Source(changed));
  REQUIRE(p);
  CHECK_FALSE(f.authority->capture_selection(f.next, *p));
}
TEST_CASE("Protected real joint ticket precedes old registry removal and exact route commits",
          "[protected-authority]") {
  Fixture f;
  auto blocked = f.consumer();
  auto backend = f.seat->backend_identity();
  auto generation = f.seat->generation();
  auto expected_selection = f.selection();
  auto expected_route = expected_selection.authority;
  auto transfer = f.handoff->request(TransferToken::fresh(0), std::move(expected_selection));
  REQUIRE(transfer.admission == TransferAdmission::Accepted);
  REQUIRE(transfer.handle->saved_ticket());
  CHECK_FALSE(transfer.handle->saved_ticket()->complete());
  {
    std::lock_guard lock(f.current->mutex);
    std::erase_if(f.current->scopes, [&](auto &s) { return s.matches(f.initial.connection); });
  }
  REQUIRE(eventually([&] { return f.seat->neutral_complete(); }));
  CHECK(transfer.handle->status() == TransferStatus::Pending);
  blocked->release();
  REQUIRE(eventually([&] { return transfer.handle->status() == TransferStatus::Granted; }));
  CHECK(f.authority->route() == expected_route);
  CHECK(f.seat->backend_identity() == backend);
  CHECK(f.seat->generation().matches(generation));
  CHECK_FALSE(f.authority->close_exact(f.initial.connection, f.initial.peer));
  CHECK(f.seat->current()->token.matches(transfer.handle->prepared_grant->token));
  CHECK(f.handoff->admit_prepared(transfer.handle,
                                  f.next.connection,
                                  f.next.peer,
                                  ConsumerKind::Audio,
                                  {"local", 13, {}},
                                  [](auto &, const auto &) { return true; }) == ConsumerAdmission::Accepted);
  REQUIRE(f.authority->close_exact(f.next.connection, f.next.peer));
  CHECK(f.handoff->admit_prepared(transfer.handle,
                                  f.next.connection,
                                  f.next.peer,
                                  ConsumerKind::Video,
                                  {"local", 13, {}},
                                  [](auto &, const auto &) { return true; }) == ConsumerAdmission::Revoked);
  CHECK_FALSE(f.handoff->prepared_admission(transfer.handle, f.next.connection, f.next.peer));
}
TEST_CASE("Protected pending peer auth catalog and lifecycle closes beat final commit", "[protected-authority]") {
  for (int cause = 0; cause < 3; ++cause) {
    Fixture f;
    auto blocked = f.consumer();
    auto transfer = f.transfer();
    REQUIRE(transfer.admission == TransferAdmission::Accepted);
    if (cause == 0)
      REQUIRE(f.authority->close_exact(f.next.connection, f.next.peer));
    else if (cause == 1) {
      auto update = f.authority->begin_update(f.op, f.original);
      REQUIRE(update.result == pa::Result::Accepted);
      REQUIRE(f.authority->publish_prepared(update.token, {facts(f.destination, "43"), {}}) == pa::Result::Accepted);
    } else
      REQUIRE(f.authority->close_exact(f.op, f.original)); // exact L lifecycle authority notice
    blocked->release();
    REQUIRE(eventually([&] { return transfer.handle->status() == TransferStatus::Quarantined; }));
    CHECK_FALSE(f.seat->current());
    CHECK_FALSE(f.authority->capture_selection(f.next, principal()));
  }
}
TEST_CASE("Protected operator tokens cannot publish across newer close or foreign domain", "[protected-authority]") {
  Fixture f;
  auto first = f.authority->begin_update(f.op, f.original);
  REQUIRE(first.result == pa::Result::Accepted);
  CHECK(f.authority->status().update_pending);
  auto second = f.authority->begin_update(f.op, f.original);
  CHECK(second.result == pa::Result::Busy);
  CHECK(f.authority->publish_prepared(first.token, {facts(f.destination), {}}) == pa::Result::Stale);
  CHECK(f.authority->facts() == f.original);
  Fixture other;
  CHECK(other.authority->publish_prepared(first.token, {facts(other.destination), {}}) == pa::Result::Stale);
  CHECK(other.seat->current());
  CHECK(f.authority->close_exact(f.op, f.original));
  CHECK(f.authority->status().update_pending); // occupied until reconciliation; no replacement
}
TEST_CASE("Protected sequence overflow and failed prepared policy never reopen", "[protected-authority]") {
  Fixture f(false, {}, {}, 1);
  auto update = f.authority->begin_update(f.op, f.original);
  REQUIRE(update.result == pa::Result::Accepted);
  CHECK(f.authority->publish_prepared(update.token, {}) == pa::Result::Denied);
  auto replacement = facts(f.destination);
  REQUIRE(f.authority->publish_prepared(update.token, {replacement, {}}) == pa::Result::Accepted);
  CHECK(f.authority->facts() == replacement);
  CHECK_FALSE(f.seat->current());
  CHECK(f.authority->begin_update(f.op, replacement).result == pa::Result::Unknown);
  CHECK(f.authority->status().sequence == 1);
  CHECK_FALSE(f.authority->capture_selection(f.next, principal()));
}
TEST_CASE("Protected retired fact owner really destroys outside gate and reserved close progresses",
          "[protected-authority]") {
  auto gate = std::make_shared<TestGate>();
  std::atomic_bool destroyed = false;
  Fixture f(false, {}, std::make_unique<Resource>(gate, &destroyed));
  f.gates.push_back(gate);
  auto update = f.authority->begin_update(f.op, f.original);
  REQUIRE(update.result == pa::Result::Accepted);
  Joiner join{f};
  std::atomic_bool returned = false;
  join.threads.emplace_back([&] {
    f.authority->publish_prepared(update.token, {facts(f.destination), {}});
    returned = true;
  });
  REQUIRE(eventually([&] { return gate->entered.load(); }));
  CHECK_FALSE(destroyed);
  CHECK_FALSE(returned);
  CHECK(f.authority->status().cleanup_pending);
  REQUIRE(f.authority->close_exact(f.op, f.authority->facts()));
  f.handoff->poll();
  CHECK_FALSE(f.seat->current());
  gate->release();
  REQUIRE(eventually([&] { return returned.load(); }));
  CHECK(destroyed);
  CHECK_FALSE(f.authority->status().cleanup_pending);
  CHECK(f.authority->status().closed);
}
TEST_CASE("Protected failed resource retirement retains Unknown capacity", "[protected-authority]") {
  std::atomic_bool destroyed = false;
  Fixture f(false, {}, std::make_unique<Resource>(nullptr, &destroyed, true));
  auto update = f.authority->begin_update(f.op, f.original);
  REQUIRE(update.result == pa::Result::Accepted);
  CHECK(f.authority->publish_prepared(update.token, {facts(f.destination), {}}) == pa::Result::Unknown);
  CHECK_FALSE(destroyed);
  CHECK(f.authority->status().cleanup_pending);
  CHECK(f.seat->quarantined());
  CHECK(f.authority->begin_update(f.op, f.authority->facts()).result == pa::Result::Busy);
  CHECK(f.authority->close_exact(f.op, f.authority->facts()));
}
TEST_CASE("Protected actual previous validator cleanup blocks grant not gate and late cleanup never auto grants",
          "[protected-authority]") {
  for (bool expire : {false, true}) {
    Fixture f;
    auto gate = std::make_shared<TestGate>();
    f.gates.push_back(gate);
    std::atomic_bool reentered = false;
    auto holder = std::make_shared<Capture>();
    holder->gate = gate;
    holder->after = [&] {
      (void)f.seat->current();
      (void)f.authority->status();
      reentered = true;
    };
    auto first = f.transfer([holder](const auto &) { return true; });
    holder.reset();
    REQUIRE(first.admission == TransferAdmission::Accepted);
    REQUIRE(eventually([&] { return first.handle->status() == TransferStatus::Granted; }));
    f.old = f.seat->current();
    first.handle.reset(); // pending_ now owns sole actual validator
    f.next = {RunScope::fresh("42"),
              f.destination,
              ConnectionScope::fresh(42),
              PeerToken::fresh(0),
              BindingRevision::fresh(0),
              0};
    {
      std::lock_guard lock(f.current->mutex);
      f.current->scopes.push_back(f.next.connection);
    }
    Joiner join{f};
    std::promise<TransferResult> result;
    auto future = result.get_future();
    join.threads.emplace_back([&] {
      try {
        result.set_value(f.transfer());
      } catch (...) {
        result.set_exception(std::current_exception());
      }
    });
    REQUIRE(eventually([&] { return gate->entered.load(); }));
    CHECK(f.authority->status().cleanup_pending);
    CHECK_FALSE(f.seat->current());
    REQUIRE(eventually([&] { return f.seat->neutral_complete(); }));
    f.handoff->poll();
    CHECK_FALSE(f.seat->current());
    if (expire) {
      f.clock->store(16001);
      f.handoff->poll();
      CHECK(f.seat->quarantined());
    }
    gate->release();
    REQUIRE(future.wait_for(2s) == std::future_status::ready);
    auto second = future.get();
    REQUIRE(second.admission == TransferAdmission::Accepted);
    REQUIRE(eventually([&] { return reentered.load(); }));
    if (expire) {
      f.handoff->poll();
      CHECK(second.handle->status() == TransferStatus::Quarantined);
      CHECK_FALSE(f.seat->current());
    } else
      REQUIRE(eventually([&] { return second.handle->status() == TransferStatus::Granted; }));
  }
}
TEST_CASE("Protected optional validation runs outside gate and cannot escape current authority",
          "[protected-authority]") {
  Fixture f;
  auto selection = f.selection([&](const auto &) {
    (void)f.seat->current();
    return true;
  });
  auto result = f.handoff->request(TransferToken::fresh(0), std::move(selection));
  REQUIRE(result.admission == TransferAdmission::Accepted);
  REQUIRE(eventually([&] { return result.handle->status() == TransferStatus::Granted; }));
  REQUIRE(f.authority->close_exact(f.op, f.original));
  CHECK_FALSE(f.handoff->prepared_admission(result.handle, f.next.connection, f.next.peer));
}

TEST_CASE("Protected facade loss closes retained seat without recreating backend", "[protected-authority]") {
  Fixture f;
  const auto backend = f.seat->backend_identity();
  f.authority.reset();
  CHECK(f.seat->quarantined());
  CHECK_FALSE(f.seat->current());
  CHECK(f.seat->submit(f.command(f.old)) == SeatAdmission::Quarantined);
  CHECK(f.seat->backend_identity() == backend);
}
TEST_CASE("Protected bootstrap validator actual capture retirement precedes initial grant", "[protected-authority]") {
  auto gate = std::make_shared<TestGate>();
  auto holder = std::make_shared<Capture>();
  holder->gate = gate;
  std::atomic_bool reentered = false;
  Fixture f(true, [holder](const auto &) { return true; });
  f.gates.push_back(gate);
  holder->after = [&] {
    (void)f.seat->current();
    (void)f.authority->status();
    reentered = true;
  };
  holder.reset();
  REQUIRE(eventually([&] { return f.io->gate.entered.load(); }));
  f.io->gate.release();
  REQUIRE(eventually([&] { return gate->entered.load(); }));
  CHECK(f.authority->status().cleanup_pending);
  CHECK_FALSE(f.seat->current());
  f.handoff->poll();
  gate->release();
  REQUIRE(eventually([&] { return reentered.load(); }));
  REQUIRE(eventually([&] { return bool(f.seat->current()); }));
}

TEST_CASE("Protected failed bootstrap never publishes a route even with historical allowed evidence",
          "[protected-authority]") {
  for (bool failed_io : {true, false}) {
    Fixture f(true, [failed_io](const auto &) { return failed_io; });
    REQUIRE(eventually([&] { return f.io->gate.entered.load(); }));
    f.io->failed = failed_io;
    f.io->gate.release();
    REQUIRE(eventually([&] { return f.seat->quarantined(); }));
    CHECK_FALSE(f.seat->current());
    CHECK_FALSE(f.authority->route());
    CHECK(f.original->evaluate(principal()).result == facts_api::Result::Allowed);
  }
}

TEST_CASE("Protected absent principal and empty ACL refuse before backend publication", "[protected-authority]") {
  const auto run = RunScope::fresh("L");
  const auto cap = pa::OperatorCapability::fresh(0);
  SeatCandidate c{RunScope::fresh("42"),
                  run,
                  ConnectionScope::fresh(42),
                  PeerToken::fresh(0),
                  BindingRevision::fresh(0),
                  0};
  auto empty = facts(run, "42", false);
  CHECK_FALSE(empty); // actual 3A refuses this policy before any factory exists
  CHECK_THROWS_AS((pa::ProtectedAuthorityDomain(pa::PreparedFacts{empty, {}}, cap)), std::invalid_argument);
  pa::ProtectedAuthorityDomain domain({facts(run), {}}, cap);
  bool called = false;
  CHECK_THROWS_AS(domain.create_seat(c,
                                     facts_api::Principal{},
                                     {true, true, true},
                                     [&](auto) -> std::shared_ptr<CheckedXboxBackend> {
                                       called = true;
                                       return {};
                                     }),
                  std::invalid_argument);
  CHECK_FALSE(called);
  CHECK_FALSE(domain.status().installed);
}
TEST_CASE("Protected launch rollback consumes domain and never falls back to plain seat", "[protected-authority]") {
  const auto run = RunScope::fresh("L");
  pa::ProtectedAuthorityDomain domain({facts(run), {}}, pa::OperatorCapability::fresh(0));
  SeatCandidate c{RunScope::fresh("42"),
                  run,
                  ConnectionScope::fresh(42),
                  PeerToken::fresh(0),
                  BindingRevision::fresh(0),
                  0};
  auto io = std::make_shared<IO>();
  size_t calls = 0;
  auto factory = [&](auto callback) {
    ++calls;
    auto xbox = inputtino::checked::Xbox::adopt({io, 42}, io->ops(), std::move(callback), [](auto work) {
      return std::thread(std::move(work));
    });
    return std::make_shared<CheckedXboxBackend>(std::move(xbox));
  };
  CHECK_THROWS_AS(
      domain.create_seat(c, principal(), {true, true, true}, factory, {}, {}, [](auto) { return std::thread{}; }),
      std::runtime_error);
  CHECK(domain.status().quarantined);
  CHECK(domain.status().closed);
  CHECK_FALSE(domain.route());
  CHECK_THROWS_AS(domain.create_seat(c, principal(), {true, true, true}, factory), std::invalid_argument);
  CHECK(calls == 1);
}

TEST_CASE("Protected exhausted close invalidates the last pending update without counter wrap",
          "[protected-authority]") {
  for (bool begin_again : {true, false}) {
    Fixture f(false, {}, {}, 1);
    auto token = f.authority->begin_update(f.op, f.original);
    REQUIRE(token.result == pa::Result::Accepted);
    if (begin_again)
      CHECK(f.authority->begin_update(f.op, f.original).result == pa::Result::Unknown);
    else
      CHECK(f.authority->close_exact(f.op, f.original));
    CHECK(f.authority->publish_prepared(token.token, {facts(f.destination), {}}) == pa::Result::Stale);
    CHECK(f.authority->facts() == f.original);
    CHECK(f.authority->status().sequence == 1);
    CHECK(f.authority->status().update_pending); // retained exhausted/Unknown obligation
    CHECK_FALSE(f.seat->current());
  }
}
