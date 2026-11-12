#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <future>
#include <sessions/protected_pairing.hpp>

using namespace sessions;
using namespace control;
using namespace streaming;
using namespace std::chrono_literals;
namespace pp = sessions::protected_pairing;
namespace pa = sessions::protected_authority;
namespace pi = sessions::protected_identity;
namespace access_api = sessions::protected_access;
namespace {
using Clock = std::chrono::steady_clock;
bool eventually(const std::function<bool()> &f) {
  const auto until = Clock::now() + 2s;
  while (!f() && Clock::now() < until)
    std::this_thread::sleep_for(1ms);
  return f();
}
struct Gate {
  std::mutex mutex;
  std::condition_variable cv;
  std::atomic_bool entered{false};
  bool released = false;
  void wait() {
    std::unique_lock lock(mutex);
    entered = true;
    cv.wait_for(lock, 4s, [&] { return released; });
  }
  void release() {
    std::lock_guard lock(mutex);
    released = true;
    cv.notify_all();
  }
  ~Gate() {
    release();
  }
};
const x509::x509_ptr &certificate() {
  static const auto c = x509::generate_x509(x509::generate_key());
  return c;
}
state::PairedClientList clients(std::string pem = x509::get_cert_pem(certificate())) {
  return state::PairedClientList{}.push_back(
      immer::box<wolf::config::PairedClient>(wolf::config::PairedClient{.client_cert = std::move(pem)}));
}
struct Source : access_api::VerifiedIdentitySource {
  access_api::VerifiedLeafSnapshot verified_leaf() const override {
    return {certificate(), certificate(), std::hash<std::string>{}(x509::get_cert_pem(certificate()))};
  }
};
access_api::Principal principal() {
  return *access_api::Principal::capture(Source{});
}
struct RequestTypes : SimpleWeb::ServerBase<SimpleWeb::HTTPS> {
  using SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Connection;
  using SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Session;
};
struct Request {
  boost::asio::io_context io;
  boost::asio::ssl::context tls{boost::asio::ssl::context::tls};
  std::shared_ptr<RequestTypes::Connection> connection = std::make_shared<RequestTypes::Connection>(
      std::make_shared<SimpleWeb::ScopeRunner>(), io, tls);
  std::shared_ptr<RequestTypes::Session> session = std::make_shared<RequestTypes::Session>(2048, connection);
};
struct IO {
  std::atomic_bool fail{false};
  std::atomic_size_t writes{0};
  inputtino::checked::detail::Io operations() {
    return {
        [this](int fd, const input_event &, size_t n) {
          if (fd != 42 || n != sizeof(input_event))
            throw std::runtime_error("not injected IO");
          ++writes;
          return fail ? inputtino::checked::IoResult{-1, EAGAIN} : inputtino::checked::IoResult{std::ptrdiff_t(n), 0};
        },
        [](int, int) {
          std::this_thread::sleep_for(1ms);
          return inputtino::checked::IoResult{0, 0};
        },
        [](int, input_event &, size_t) -> inputtino::checked::IoResult { throw std::runtime_error("read forbidden"); },
        [](int, unsigned long, void *) -> int { throw std::runtime_error("ioctl forbidden"); },
        [] { return Clock::now(); }};
  }
};
struct Operation : pp::PersistenceOperation {
  Gate *execute_gate = nullptr, *destroy_gate = nullptr;
  std::function<void()> on_destroy;
  std::optional<pp::PersistenceOutcome> replay;
  std::optional<pp::PersistenceOutcome> *save = nullptr;
  pp::PersistenceResult result = pp::PersistenceResult::Checked;
  bool throw_operation = false;
  pp::PersistenceOutcome execute(const pp::Transaction &tx) override {
    if (throw_operation)
      throw std::runtime_error("injected persistence failure");
    if (execute_gate)
      execute_gate->wait();
    if (save)
      save->emplace(tx.outcome(result));
    return replay ? *replay : tx.outcome(result);
  }
  ~Operation() override {
    if (destroy_gate)
      destroy_gate->wait();
    if (on_destroy)
      on_destroy();
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
  pa::OperatorCapability op = pa::OperatorCapability::fresh(0);
  std::shared_ptr<std::atomic<int64_t>> clock = std::make_shared<std::atomic<int64_t>>(10000);
  state::SessionsAtoms running = std::make_shared<immer::atom<immer::vector<wolf::core::events::StreamSession>>>();
  std::shared_ptr<ConsumerComposition> composition = std::make_shared<ConsumerComposition>(running);
  std::shared_ptr<ProtectedRuntimeOwner> runtime = std::make_shared<ProtectedRuntimeOwner>(composition);
  access_api::Leaf leaf = *access_api::Leaf::from_x509(certificate());
  std::array<access_api::PinView, 1> pins{{{leaf, std::hash<std::string>{}(x509::get_cert_pem(certificate()))}}};
  std::array<access_api::RuleView, 1> rules{{{"game", "42", pins}}};
  std::unique_ptr<pp::PairingMutationOwner> owner;
  Request request;
  std::vector<Gate *> teardown;
  explicit Fixture(uint64_t limit = std::numeric_limits<uint64_t>::max(),
                   std::unique_ptr<pa::RetirementResource> resource = {},
                   ConsumerPolicy policy = {})
      : composition(std::make_shared<ConsumerComposition>(running, policy)),
        runtime(std::make_shared<ProtectedRuntimeOwner>(composition)) {
    RuntimeBootstrap bootstrap{
        {{}, std::move(resource)},
        op,
        initial,
        principal(),
        {true, true, true},
        [io = io](auto callback) {
          auto xbox = inputtino::checked::Xbox::adopt({io, 42}, io->operations(), std::move(callback), [](auto f) {
            return std::thread(std::move(f));
          });
          return std::make_shared<CheckedXboxBackend>(std::move(xbox));
        },
        {},
        [clock = clock] { return Clock::time_point{} + std::chrono::milliseconds(clock->load()); }};
    owner = std::make_unique<pp::PairingMutationOwner>(runtime,
                                                       clients(),
                                                       rules,
                                                       "game",
                                                       "42",
                                                       std::move(bootstrap),
                                                       limit);
    REQUIRE(eventually([&] { return bool(runtime->current()); }));
  }
  ~Fixture() {
    for (auto gate : teardown)
      gate->release();
    runtime->request_stop();
    while (runtime->join_when_exited() != RuntimeJoin::Joined)
      std::this_thread::sleep_for(1ms); // all test gates released above, no detached cleanup
    owner.reset();
    runtime.reset();
  }
  pi::VerifiedHttpsIdentity identity(pp::PublicationHandle publication) {
    auto result = owner->capture_https(request.session->request,
                                       publication,
                                       {nullptr, +[](void *, const pi::RequestPtr &) { return certificate(); }});
    REQUIRE(result.value);
    return *result.value;
  }
  pp::BeginResult begin() {
    auto result = owner->begin_update(op, owner->capture_publication());
    REQUIRE(result.result == pp::Result::Accepted);
    return result;
  }
  pp::PublicationHandle prepare(const pp::UpdateHandle &update, state::PairedClientList list = clients()) {
    auto prepared = owner->prepare(update, list, rules);
    REQUIRE(prepared);
    return prepared;
  }
  bool completed() {
    return eventually([&] {
      const auto s = owner->status();
      return !s.occupied;
    });
  }
};
} // namespace

TEST_CASE("Pairing bootstrap seals real identity and exact typed permit; invalid callers preserve healthy grant",
          "[protected-pairing]") {
  Fixture f;
  auto publication = f.owner->capture_publication();
  REQUIRE(publication);
  CHECK(f.runtime->runtime() == f.composition->runtime());
  auto identity = f.identity(publication);
  CHECK(f.owner->capture_selection(f.next, identity, publication));
  CHECK_FALSE(f.runtime->capture(f.next, identity.principal(), {}, publication->revision));
  CHECK_FALSE(f.runtime->capture(f.next, identity.principal(), publication->facts, pa::PairingRevision::fresh(0)));
  CHECK_FALSE(f.owner->close_exact({}, publication));
  CHECK_FALSE(f.owner->close_exact(pa::OperatorCapability::fresh(0), publication));
  CHECK(f.owner->begin_update({}, publication).result == pp::Result::Stale);
  CHECK(f.owner->begin_update(f.op, {}).result == pp::Result::Stale);
  CHECK(f.runtime->current());
  CHECK(f.owner->status().phase == pp::Phase::Current);
}

TEST_CASE("Pairing checked transaction publishes facts only after full command destruction", "[protected-pairing]") {
  Gate destruction;
  std::atomic_bool inspected{false};
  Fixture f;
  f.teardown.push_back(&destruction);
  auto old = f.owner->capture_publication();
  auto old_identity = f.identity(old);
  auto update = f.begin();
  auto prepared = f.prepare(update.update);
  auto op = std::make_unique<Operation>();
  op->destroy_gate = &destruction;
  op->on_destroy = [&] { inspected = f.owner->status().phase == pp::Phase::Retiring; };
  REQUIRE(eventually([&] { return f.owner->ready(update.update); }));
  REQUIRE(f.owner->submit(update.update, prepared, std::move(op)) == pp::Result::Accepted);
  REQUIRE(eventually([&] { return destruction.entered.load(); }));
  CHECK_FALSE(f.owner->capture_publication());
  CHECK(f.owner->status().occupied);
  CHECK(f.runtime->status().occupied == 1);
  CHECK_FALSE(f.runtime->current());
  auto generic = std::make_unique<RuntimeJob>();
  generic->cell = RuntimeCell::Retirement;
  generic->retire = [] {};
  CHECK(f.runtime->submit(std::move(generic)).result != RuntimeSubmission::Accepted);
  destruction.release();
  REQUIRE(f.completed());
  CHECK(inspected.load());
  CHECK(f.owner->capture_publication() == prepared);
  CHECK_FALSE(f.runtime->current());
  CHECK_FALSE(f.owner->capture_selection(f.next, old_identity, prepared));
  CHECK_FALSE(f.owner->capture_https(f.request.session->request, old).value);
  CHECK_FALSE(old->revision.matches(prepared->revision));
  CHECK(f.owner->submit(update.update, prepared, std::make_unique<Operation>()) == pp::Result::Stale);
}

TEST_CASE("Pairing D26 permission-only Pending quarantine does not block healthy typed facts publication",
          "[protected-pairing]") {
  Gate consumer;
  Fixture f;
  f.teardown.push_back(&consumer);
  auto original = f.owner->capture_publication();
  auto identity = f.identity(original);
  ConsumerCorrelation correlation{"127.0.0.1", 12, {}};
  REQUIRE(f.runtime->runtime()->admit(f.initial.connection,
                                      f.initial.source,
                                      ConsumerKind::Audio,
                                      correlation,
                                      [&](auto &, const auto &) {
                                        consumer.wait();
                                        return true;
                                      }) == ConsumerAdmission::Accepted);
  f.runtime->runtime()->ping(ConsumerKind::Audio, {"127.0.0.1", 12, {}, {}});
  REQUIRE(eventually([&] { return consumer.entered.load(); }));
  auto selection = std::make_unique<RuntimeJob>();
  selection->transfer = TransferToken::fresh(0);
  selection->selection = {f.runtime->current(), f.next, {}, f.owner->capture_selection(f.next, identity, original)};
  auto submitted = f.runtime->submit(std::move(selection));
  REQUIRE(submitted.receipt);
  REQUIRE(eventually([&] { return submitted.receipt->state == RuntimeJobState::Complete; }));
  auto transfer = f.runtime->transfer();
  REQUIRE(transfer);
  REQUIRE(transfer->status() == TransferStatus::Pending);
  auto update = f.owner->begin_update(f.op, original);
  REQUIRE(update.result == pp::Result::Accepted);
  REQUIRE(eventually([&] { return transfer->status() == TransferStatus::Quarantined; }));
  // This real generic submission triggers the existing D26 permission Unknown guard.
  auto job = std::make_unique<RuntimeJob>();
  job->cell = RuntimeCell::Retirement;
  job->retire = [] {};
  CHECK(f.runtime->submit(std::move(job)).result == RuntimeSubmission::Closed);
  CHECK(f.runtime->status().unknown);
  CHECK_FALSE(transfer->saved_ticket()->complete());
  auto prepared = f.prepare(update.update);
  REQUIRE(eventually([&] { return f.owner->ready(update.update); }));
  REQUIRE(f.owner->submit(update.update, prepared, std::make_unique<Operation>()) == pp::Result::Accepted);
  REQUIRE(f.completed());
  CHECK(f.runtime->status().unknown);
  CHECK_FALSE(f.runtime->current());
  CHECK(transfer->status() == TransferStatus::Quarantined);
  consumer.release();
  REQUIRE(eventually([&] { return transfer->saved_ticket()->complete(); }));
  CHECK_FALSE(f.runtime->current());
}

TEST_CASE("Pairing exact reserved close between facts replacement and tuple exchange rejects late publication",
          "[protected-pairing]") {
  Gate destruction;
  Fixture f;
  f.teardown.push_back(&destruction);
  auto original = f.owner->capture_publication();
  auto update = f.begin();
  auto prepared = f.prepare(update.update);
  auto op = std::make_unique<Operation>();
  op->destroy_gate = &destruction;
  REQUIRE(eventually([&] { return f.owner->ready(update.update); }));
  REQUIRE(f.owner->submit(update.update, prepared, std::move(op)) == pp::Result::Accepted);
  REQUIRE(eventually([&] { return destruction.entered.load(); }));
  CHECK(f.owner->close_exact(f.op, original));
  CHECK(f.owner->status().phase == pp::Phase::Unknown);
  CHECK(f.owner->begin_update(f.op, original).result == pp::Result::Busy);
  destruction.release();
  REQUIRE(eventually([&] { return f.runtime->status().occupied == 0; }));
  f.owner->poll();
  CHECK_FALSE(f.owner->capture_publication());
  CHECK(f.owner->status().occupied);
  CHECK_FALSE(f.runtime->current());
}

TEST_CASE("Pairing failed provider and replayed foreign outcome retain closed cap", "[protected-pairing]") {
  std::optional<pp::PersistenceOutcome> saved;
  {
    Fixture first;
    auto u = first.begin();
    auto p = first.prepare(u.update);
    auto op = std::make_unique<Operation>();
    op->save = &saved;
    REQUIRE(eventually([&] { return first.owner->ready(u.update); }));
    REQUIRE(first.owner->submit(u.update, p, std::move(op)) == pp::Result::Accepted);
    REQUIRE(first.completed());
  }
  REQUIRE(saved);
  Fixture f;
  auto update = f.begin();
  auto prepared = f.prepare(update.update);
  auto op = std::make_unique<Operation>();
  op->replay.emplace(*saved);
  REQUIRE(eventually([&] { return f.owner->ready(update.update); }));
  REQUIRE(f.owner->submit(update.update, prepared, std::move(op)) == pp::Result::Accepted);
  REQUIRE(eventually([&] { return f.owner->status().phase == pp::Phase::Unknown; }));
  CHECK(f.runtime->status().stop);
  CHECK(f.runtime->status().occupied == 1);
  CHECK_FALSE(f.runtime->current());
  CHECK(f.owner->status().occupied);
}

TEST_CASE("Pairing preparation caps invalid PEM empty result and exhausted sequence are fail closed",
          "[protected-pairing]") {
  SECTION("empty result is ClosedEmpty without principal") {
    Fixture f;
    auto u = f.begin();
    auto p = f.prepare(u.update, {});
    CHECK(p->source.empty());
    CHECK(p->sealed.size() == 0);
    REQUIRE(eventually([&] { return f.owner->ready(u.update); }));
    REQUIRE(f.owner->submit(u.update, p, std::make_unique<Operation>()) == pp::Result::Accepted);
    REQUIRE(f.completed());
    CHECK(f.owner->status().phase == pp::Phase::ClosedEmpty);
    CHECK_FALSE(f.owner->capture_https(f.request.session->request, p).value);
    CHECK_FALSE(f.runtime->current());
  }
  SECTION("malformed or oversized input keeps preparation occupied") {
    for (const auto &pem : {std::string("bad PEM"), std::string(access_api::max_pem_bytes + 1, 'x')}) {
      Fixture f;
      auto u = f.begin();
      CHECK_FALSE(f.owner->prepare(u.update, clients(pem), f.rules));
      CHECK(f.owner->status().phase == pp::Phase::Unknown);
      CHECK(f.owner->status().occupied);
    }
  }
  SECTION("entry count before parsing") {
    Fixture f;
    auto u = f.begin();
    state::PairedClientList many;
    for (unsigned i = 0; i < 65; ++i)
      many = many.push_back(*clients().begin());
    CHECK_FALSE(f.owner->prepare(u.update, many, f.rules));
    CHECK(f.owner->status().occupied);
  }
  SECTION("checked sequence never wraps/reopens") {
    Fixture f(1);
    auto u = f.begin();
    auto p = f.prepare(u.update);
    REQUIRE(eventually([&] { return f.owner->ready(u.update); }));
    REQUIRE(f.owner->submit(u.update, p, std::make_unique<Operation>()) == pp::Result::Accepted);
    REQUIRE(f.completed());
    CHECK(f.owner->begin_update(f.op, p).result == pp::Result::Unknown);
    CHECK(f.owner->status().sequence == 1);
    CHECK_FALSE(f.owner->capture_publication());
  }
}

TEST_CASE("Pairing failed actual retirement and popped generic cleanup cannot become healthy typed execution",
          "[protected-pairing]") {
  SECTION("actual generic callback throws") {
    Fixture f;
    auto original = f.owner->capture_publication();
    auto job = std::make_unique<RuntimeJob>();
    job->cell = RuntimeCell::Retirement;
    job->retire = [] { throw std::runtime_error("unproved resource retirement"); };
    auto r = f.runtime->submit(std::move(job));
    REQUIRE(r.receipt);
    REQUIRE(eventually([&] { return r.receipt->state == RuntimeJobState::Unknown; }));
    auto u = f.owner->begin_update(f.op, original);
    REQUIRE(u.result == pp::Result::Accepted);
    auto p = f.prepare(u.update);
    CHECK(f.owner->submit(u.update, p, std::make_unique<Operation>()) == pp::Result::Unknown);
    CHECK(f.runtime->status().occupied == 1);
    CHECK(f.runtime->status().stop);
  }
  SECTION("blocked popped cell counts, close remains available") {
    Gate blocking;
    Fixture f;
    f.teardown.push_back(&blocking);
    auto original = f.owner->capture_publication();
    auto job = std::make_unique<RuntimeJob>();
    job->cell = RuntimeCell::Retirement;
    job->retire = [&] { blocking.wait(); };
    auto r = f.runtime->submit(std::move(job));
    REQUIRE(r.receipt);
    REQUIRE(eventually([&] { return blocking.entered.load(); }));
    auto u = f.begin();
    auto p = f.prepare(u.update);
    CHECK(f.owner->submit(u.update, p, std::make_unique<Operation>()) == pp::Result::Unknown);
    CHECK(f.owner->close_exact(f.op, original));
    CHECK(f.owner->status().occupied);
    blocking.release();
    REQUIRE(eventually([&] { return r.receipt->state == RuntimeJobState::Complete; }));
    CHECK_FALSE(f.owner->capture_publication());
  }
}

TEST_CASE("Pairing capture rechecks exact facts under both actual gate sections", "[protected-pairing]") {
  Gate evaluated;
  Fixture f;
  f.teardown.push_back(&evaluated);
  auto publication = f.owner->capture_publication();
  auto identity = f.identity(publication);
  pa::PermitHandle permit;
  std::jthread capture([&] {
    permit = f.owner->capture_selection(f.next, identity, publication, {&evaluated, +[](void *p) {
                                                                          static_cast<Gate *>(p)->wait();
                                                                        }});
  });
  REQUIRE(eventually([&] { return evaluated.entered.load(); }));
  CHECK(f.owner->close_exact(f.op, publication));
  evaluated.release();
  capture.join();
  CHECK_FALSE(permit);
  CHECK_FALSE(f.runtime->current());
}

TEST_CASE("Pairing provider blocked before publication allows reserved close and deadline decisions",
          "[protected-pairing]") {
  Gate execute;
  Fixture f;
  f.teardown.push_back(&execute);
  auto old = f.owner->capture_publication();
  auto u = f.begin();
  auto p = f.prepare(u.update);
  auto op = std::make_unique<Operation>();
  op->execute_gate = &execute;
  REQUIRE(eventually([&] { return f.owner->ready(u.update); }));
  REQUIRE(f.owner->submit(u.update, p, std::move(op)) == pp::Result::Accepted);
  REQUIRE(eventually([&] { return execute.entered.load(); }));
  f.clock->store(16001);
  CHECK(f.owner->status().occupied);
  CHECK(f.owner->close_exact(f.op, old));
  CHECK_FALSE(f.runtime->current());
  execute.release();
  REQUIRE(eventually([&] { return f.runtime->status().stop; }));
  CHECK(f.owner->status().phase == pp::Phase::Unknown);
  CHECK(f.runtime->status().occupied == 1);
}

TEST_CASE("Pairing actual authority resource retirement failure is never typed completion", "[protected-pairing]") {
  struct Resource : pa::RetirementResource {
    void retire() override {
      throw std::runtime_error("retirement failed");
    }
  };
  Fixture f(std::numeric_limits<uint64_t>::max(), std::make_unique<Resource>());
  auto u = f.begin();
  auto p = f.prepare(u.update);
  REQUIRE(eventually([&] { return f.owner->ready(u.update); }));
  REQUIRE(f.owner->submit(u.update, p, std::make_unique<Operation>()) == pp::Result::Accepted);
  REQUIRE(eventually([&] { return f.runtime->status().stop; }));
  CHECK(f.owner->status().phase == pp::Phase::Unknown);
  CHECK(f.owner->status().occupied);
  CHECK(f.runtime->status().occupied == 1);
  CHECK_FALSE(f.runtime->current());
}

TEST_CASE("Pairing same values fresh revision and result mismatch cannot reuse transaction evidence",
          "[protected-pairing]") {
  std::optional<pp::PersistenceOutcome> saved;
  Fixture f;
  auto initial = f.owner->capture_publication();
  auto u = f.begin();
  auto first = f.prepare(u.update);
  auto op = std::make_unique<Operation>();
  op->save = &saved;
  REQUIRE(eventually([&] { return f.owner->ready(u.update); }));
  REQUIRE(f.owner->submit(u.update, first, std::move(op)) == pp::Result::Accepted);
  REQUIRE(f.completed());
  REQUIRE(saved);
  CHECK_FALSE(f.owner->close_exact(f.op, initial));
  CHECK(f.owner->begin_update(f.op, initial).result == pp::Result::Stale);
  auto second = f.owner->begin_update(f.op, first);
  REQUIRE(second.result == pp::Result::Accepted);
  auto next = f.prepare(second.update);
  CHECK_FALSE(first->identity.matches(next->identity));
  CHECK_FALSE(first->revision.matches(next->revision));
  CHECK_FALSE(f.owner->prepare(second.update, clients(), f.rules)); // at most one prepared result
  CHECK(f.owner->submit(second.update, first, std::make_unique<Operation>()) == pp::Result::Stale);
  auto replay = std::make_unique<Operation>();
  replay->replay.emplace(*saved);
  REQUIRE(eventually([&] { return f.owner->ready(second.update); }));
  REQUIRE(f.owner->submit(second.update, next, std::move(replay)) == pp::Result::Accepted);
  REQUIRE(eventually([&] { return f.owner->status().phase == pp::Phase::Unknown; }));
  CHECK_FALSE(f.owner->capture_publication());
  CHECK_FALSE(f.runtime->current());
}

TEST_CASE("Pairing rejected command sole provider destruction runs outside owner queue and Seat locks",
          "[protected-pairing]") {
  Gate worker, destruction;
  std::atomic_bool done{false};
  pp::Result result = pp::Result::Accepted;
  Fixture f;
  f.teardown = {&worker, &destruction};
  auto initial = f.owner->capture_publication();
  auto generic = std::make_unique<RuntimeJob>();
  generic->cell = RuntimeCell::Retirement;
  generic->retire = [&] { worker.wait(); };
  auto r = f.runtime->submit(std::move(generic));
  REQUIRE(r.receipt);
  REQUIRE(eventually([&] { return worker.entered.load(); }));
  auto u = f.begin();
  auto p = f.prepare(u.update);
  std::jthread caller([&] {
    auto op = std::make_unique<Operation>();
    op->destroy_gate = &destruction;
    result = f.owner->submit(u.update, p, std::move(op));
    done = true;
  });
  REQUIRE(eventually([&] { return destruction.entered.load(); }));
  CHECK_FALSE(done.load());
  CHECK(f.owner->status().occupied);
  CHECK(f.runtime->status().occupied == 1);
  CHECK(f.owner->close_exact(f.op, initial));
  CHECK_FALSE(f.runtime->current());
  destruction.release();
  caller.join();
  CHECK(result == pp::Result::Unknown);
  worker.release();
  REQUIRE(eventually([&] { return r.receipt->state == RuntimeJobState::Complete; }));
  CHECK(f.owner->status().occupied);
  CHECK_FALSE(f.owner->capture_publication());
}

TEST_CASE("Pairing failed checked neutral refuses typed publication without recovery", "[protected-pairing]") {
  Fixture f;
  f.io->fail = true;
  auto u = f.begin();
  auto p = f.prepare(u.update);
  CHECK_FALSE(f.owner->ready(u.update));
  CHECK(f.owner->submit(u.update, p, std::make_unique<Operation>()) == pp::Result::Unknown);
  CHECK(f.owner->status().occupied);
  CHECK_FALSE(f.runtime->current());
  f.io->fail = false;
  f.owner->poll();
  CHECK_FALSE(f.owner->capture_publication());
}

TEST_CASE("Pairing observed actual consumer Unknown is not permission-only closure", "[protected-pairing]") {
  Gate consumer;
  ConsumerPolicy policy;
  policy.drain_timeout = 30ms;
  Fixture f(std::numeric_limits<uint64_t>::max(), {}, policy);
  f.teardown.push_back(&consumer);
  auto original = f.owner->capture_publication();
  auto identity = f.identity(original);
  REQUIRE(f.runtime->runtime()->admit(f.initial.connection,
                                      f.initial.source,
                                      ConsumerKind::Audio,
                                      {"127.0.0.1", 12, {}},
                                      [&](auto &, const auto &) {
                                        consumer.wait();
                                        return true;
                                      }) == ConsumerAdmission::Accepted);
  f.runtime->runtime()->ping(ConsumerKind::Audio, {"127.0.0.1", 12, {}, {}});
  REQUIRE(eventually([&] { return consumer.entered.load(); }));
  auto job = std::make_unique<RuntimeJob>();
  job->transfer = TransferToken::fresh(0);
  job->selection = {f.runtime->current(), f.next, {}, f.owner->capture_selection(f.next, identity, original)};
  auto r = f.runtime->submit(std::move(job));
  REQUIRE(r.receipt);
  REQUIRE(eventually([&] { return r.receipt->state == RuntimeJobState::Complete; }));
  REQUIRE(eventually([&] {
    auto status = f.runtime->runtime()->status(f.initial.connection, ConsumerKind::Audio);
    return status && status->state == ConsumerState::Unknown;
  }));
  CHECK_FALSE(f.runtime->status().stop); // orchestration itself has not failed/stopped
  auto u = f.owner->begin_update(f.op, original);
  REQUIRE(u.result == pp::Result::Accepted);
  auto p = f.prepare(u.update);
  CHECK_FALSE(f.owner->ready(u.update));
  CHECK(f.owner->submit(u.update, p, std::make_unique<Operation>()) == pp::Result::Unknown);
  consumer.release();
  f.owner->poll();
  CHECK(f.owner->status().occupied);
  CHECK_FALSE(f.runtime->current());
}

TEST_CASE("Pairing foreign publication cannot close or reserve a healthy unrelated domain", "[protected-pairing]") {
  Fixture a, b;
  auto foreign = a.owner->capture_publication();
  auto own = b.owner->capture_publication();
  const auto old = b.runtime->current();
  REQUIRE(old);
  CHECK_FALSE(b.owner->close_exact(b.op, foreign));
  CHECK(b.owner->begin_update(b.op, foreign).result == pp::Result::Stale);
  CHECK(b.owner->begin_update(a.op, own).result == pp::Result::Stale);
  CHECK(b.runtime->current() == old);
  CHECK(b.owner->status().sequence == 0);
  CHECK_FALSE(b.owner->status().occupied);
  auto identity = a.identity(foreign);
  CHECK_FALSE(b.owner->capture_selection(b.next, identity, own));
}

TEST_CASE("Pairing negative persistence outcomes and throw retain typed slot without success", "[protected-pairing]") {
  for (unsigned mode = 0; mode < 3; ++mode) {
    Fixture f;
    auto u = f.begin();
    auto p = f.prepare(u.update);
    auto op = std::make_unique<Operation>();
    op->result = mode == 0 ? pp::PersistenceResult::Failed : pp::PersistenceResult::Unknown;
    op->throw_operation = mode == 2;
    REQUIRE(eventually([&] { return f.owner->ready(u.update); }));
    REQUIRE(f.owner->submit(u.update, p, std::move(op)) == pp::Result::Accepted);
    REQUIRE(eventually([&] { return f.runtime->status().stop; }));
    CHECK(f.runtime->status().occupied == 1);
    CHECK(f.owner->status().phase == pp::Phase::Unknown);
    CHECK(f.owner->status().occupied);
    CHECK_FALSE(f.owner->capture_publication());
    CHECK_FALSE(f.runtime->current());
  }
}
