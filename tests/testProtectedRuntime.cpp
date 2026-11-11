#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <future>
#include <sessions/handlers.hpp>
#include <sessions/protected_access.hpp>
#include <sessions/protected_authority.hpp>
#include <sessions/protected_runtime.hpp>

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
  pa::FactHandle original = facts(destination);
  std::shared_ptr<std::atomic<int64_t>> clock = std::make_shared<std::atomic<int64_t>>(10000);
  state::SessionsAtoms running = std::make_shared<immer::atom<immer::vector<wolf::core::events::StreamSession>>>();
  std::shared_ptr<ConsumerComposition> composition = std::make_shared<ConsumerComposition>(running);
  std::unique_ptr<ProtectedRuntimeOwner> owner = std::make_unique<ProtectedRuntimeOwner>(composition);
  std::vector<std::shared_ptr<TestGate>> gates;
  RuntimeBootstrap bootstrap() {
    return {{original, {}},
            op,
            initial,
            principal(),
            {true, true, true},
            [io = io](auto callback) {
              auto xbox = inputtino::checked::Xbox::adopt({io, 42}, io->ops(), std::move(callback), [](auto work) {
                return std::thread(std::move(work));
              });
              return std::make_shared<CheckedXboxBackend>(std::move(xbox));
            },
            {},
            [clock = clock] { return Clock::time_point{} + std::chrono::milliseconds(clock->load()); }};
  }
  explicit Fixture(bool start = true) {
    if (start) {
      REQUIRE(owner->start(bootstrap()));
      REQUIRE(eventually([&] { return bool(owner->current()); }));
    }
  }
  ~Fixture() {
    io->gate.release();
    for (auto &g : gates)
      g->release();
    owner.reset();
  }
  std::shared_ptr<TestGate> gate() {
    auto g = std::make_shared<TestGate>();
    gates.push_back(g);
    return g;
  }
  std::unique_ptr<RuntimeJob> selection(SeatValidation validate = {}) {
    auto job = std::make_unique<RuntimeJob>();
    job->transfer = TransferToken::fresh(0);
    job->selection = {owner->current(), next, std::move(validate), owner->capture(next, principal())};
    return job;
  }
  std::shared_ptr<const TransferHandle> transfer() {
    auto submitted = owner->submit(selection());
    REQUIRE(submitted.result == RuntimeSubmission::Accepted);
    REQUIRE(eventually([&] { return submitted.receipt->state == RuntimeJobState::Complete; }));
    auto h = owner->transfer();
    REQUIRE(h);
    REQUIRE(eventually([&] { return h->status() == TransferStatus::Granted; }));
    return h;
  }
  std::unique_ptr<RuntimeJob> media(std::shared_ptr<const TransferHandle> h, RuntimeCell cell) {
    auto job = std::make_unique<RuntimeJob>();
    job->cell = cell;
    job->handle = std::move(h);
    job->producer = destination;
    job->work = [](auto &, const auto &) { return true; };
    job->correlation.ip = "127.0.0.1";
    job->correlation.port = 1234;
    return job;
  }
};
} // namespace

TEST_CASE("Protected orchestration shares actual Runtime and saves ticket before pruning lifetime",
          "[protected-runtime]") {
  Fixture f;
  CHECK(f.owner->runtime() == f.composition->runtime());
  CHECK(f.composition->contains(f.initial.connection));
  CHECK_FALSE(f.composition->contains(ConnectionScope::fresh(42)));
  CHECK_FALSE(f.composition->contains({}));
  auto h = f.transfer();
  CHECK(h->old_grant->owner.connection.matches(f.initial.connection));
  REQUIRE(h->saved_ticket());
  CHECK(h->saved_ticket()->complete());
  CHECK_FALSE(f.composition->contains(f.initial.connection));
  CHECK(f.composition->contains(f.next.connection));
  CHECK(f.owner->current()->owner.destination.matches(f.destination));
  CHECK_FALSE(f.owner->close_exact(f.initial.connection, f.initial.peer));
  CHECK(f.owner->current());
  auto audio = f.owner->submit(f.media(h, RuntimeCell::Audio));
  REQUIRE(audio.receipt);
  REQUIRE(eventually([&] { return audio.receipt->state == RuntimeJobState::Complete; }));
  CHECK(audio.receipt->media == ConsumerAdmission::Accepted);
  auto video = f.owner->submit(f.media(h, RuntimeCell::Video));
  REQUIRE(video.receipt);
  REQUIRE(eventually([&] { return video.receipt->state == RuntimeJobState::Complete; }));
  CHECK(video.receipt->media == ConsumerAdmission::Accepted);
  CHECK(f.composition->runtime()->status(f.next.connection, ConsumerKind::Audio));
  CHECK(f.composition->runtime()->status(f.next.connection, ConsumerKind::Video));
}

TEST_CASE("Protected worker counts popped cell and bytes while reserved close progresses", "[protected-runtime]") {
  Fixture f;
  auto gate = f.gate();
  auto job = f.selection([gate](auto &) {
    gate->wait();
    return true;
  });
  job->payload.resize(40000);
  auto submitted = f.owner->submit(std::move(job));
  REQUIRE(submitted.receipt);
  REQUIRE(eventually([&] { return gate->entered.load(); }));
  CHECK(f.owner->status().occupied == 1);
  CHECK(f.owner->status().bytes >= 40000);
  CHECK(f.owner->submit(f.selection()).result == RuntimeSubmission::Busy);
  auto large = std::make_unique<RuntimeJob>();
  large->cell = RuntimeCell::Retirement;
  large->payload.resize(30000);
  CHECK(f.owner->submit(std::move(large)).result == RuntimeSubmission::Capacity);
  auto audio = f.owner->submit(f.media({}, RuntimeCell::Audio));
  auto video = f.owner->submit(f.media({}, RuntimeCell::Video));
  auto retirement = std::make_unique<RuntimeJob>();
  retirement->cell = RuntimeCell::Retirement;
  retirement->retire = [] {};
  auto r = f.owner->submit(std::move(retirement));
  CHECK(audio.result == RuntimeSubmission::Busy);
  CHECK(video.result == RuntimeSubmission::Busy);
  REQUIRE(r.receipt);
  CHECK(f.owner->status().occupied == 2);
  CHECK(f.owner->close_exact(f.op, f.original));
  f.owner->request_stop();
  CHECK(f.owner->status().stop);
  CHECK(f.owner->join_when_exited() == RuntimeJoin::Unknown);
  gate->release();
  REQUIRE(eventually([&] { return f.owner->join_when_exited() == RuntimeJoin::Joined; }));
  CHECK_FALSE(f.owner->current());
  CHECK(r.receipt->state == RuntimeJobState::Refused);
  CHECK(f.owner->status().occupied == 1);
  CHECK(f.owner->status().unknown);
  CHECK(submitted.receipt->state == RuntimeJobState::Unknown); // actual request observed concurrent authority close
}

TEST_CASE("Protected worker payload retirement is outside queue and precedes cell reuse", "[protected-runtime]") {
  Fixture f;
  auto gate = f.gate();
  struct Capture {
    std::shared_ptr<TestGate> gate;
    ProtectedRuntimeOwner *owner;
    ~Capture() {
      (void)owner->status();
      gate->wait();
    }
  };
  auto job = std::make_unique<RuntimeJob>();
  job->cell = RuntimeCell::Retirement;
  auto capture = std::make_shared<Capture>();
  capture->gate = gate;
  capture->owner = f.owner.get();
  job->retire = [capture] {};
  capture.reset();
  auto result = f.owner->submit(std::move(job));
  REQUIRE(result.receipt);
  REQUIRE(eventually([&] { return gate->entered.load(); }));
  CHECK(result.receipt->state == RuntimeJobState::Executing);
  CHECK(f.owner->status().occupied == 1);
  auto another = std::make_unique<RuntimeJob>();
  another->cell = RuntimeCell::Retirement;
  CHECK(f.owner->submit(std::move(another)).result == RuntimeSubmission::Busy);
  CHECK(f.owner->close_exact(f.op, f.original));
  gate->release();
  REQUIRE(eventually([&] { return result.receipt->state == RuntimeJobState::Complete; }));
}

TEST_CASE("Protected media failure after Audio revokes candidate and retains Unknown cell", "[protected-runtime]") {
  Fixture f;
  auto h = f.transfer();
  auto audio = f.owner->submit(f.media(h, RuntimeCell::Audio));
  REQUIRE(audio.receipt);
  REQUIRE(eventually([&] { return audio.receipt->state == RuntimeJobState::Complete; }));
  CHECK(audio.receipt->media == ConsumerAdmission::Accepted);
  // Real Runtime tombstone causes actual next-kind admission refusal.
  auto ticket = f.composition->runtime()->revoke(f.next.connection);
  REQUIRE(ticket);
  auto video = f.owner->submit(f.media(h, RuntimeCell::Video));
  REQUIRE(video.receipt);
  REQUIRE(eventually([&] { return video.receipt->state == RuntimeJobState::Unknown; }));
  CHECK(f.owner->status().unknown);
  CHECK(f.owner->status().occupied == 1);
  CHECK_FALSE(f.owner->current());
  CHECK(f.owner->submit(f.selection()).result == RuntimeSubmission::Closed);
  REQUIRE(eventually([&] { return ticket->complete(); }));
  CHECK_FALSE(f.owner->current());
}

TEST_CASE("Protected start consumes cap on launch failure and cannot replace owner", "[protected-runtime]") {
  for (bool throws : {false, true}) {
    Fixture f(false);
    CHECK_FALSE(f.owner->start(f.bootstrap(), [throws](auto) -> std::thread {
      if (throws)
        throw std::runtime_error("injected launch failure");
      return {};
    }));
    CHECK(f.owner->status().unknown);
    CHECK_FALSE(f.owner->start(f.bootstrap()));
    ProtectedRuntimeOwner replacement(f.composition);
    CHECK_FALSE(replacement.start(f.bootstrap()));
    CHECK(replacement.status().unknown);
    CHECK(f.owner->join_when_exited() == RuntimeJoin::Refused);
  }
}

TEST_CASE("Protected actual exit future includes factory thread tail before join", "[protected-runtime]") {
  auto tail = std::make_shared<TestGate>();
  Fixture f(false);
  f.gates.push_back(tail);
  REQUIRE(f.owner->start(f.bootstrap(), [tail](auto work) {
    return std::thread([tail, work = std::move(work)] {
      work();
      tail->wait();
    });
  }));
  f.owner->request_stop();
  REQUIRE(eventually([&] { return tail->entered.load(); }));
  CHECK(f.owner->join_when_exited() == RuntimeJoin::Unknown);
  CHECK_FALSE(f.owner->status().joined);
  tail->release();
  REQUIRE(eventually([&] { return f.owner->join_when_exited() == RuntimeJoin::Joined; }));
}

TEST_CASE("Protected stale foreign null media cannot use current transfer", "[protected-runtime]") {
  Fixture f;
  auto h = f.transfer();
  for (bool null : {true, false}) {
    auto job = f.media(null ? nullptr : h, RuntimeCell::Audio);
    if (!null)
      job->producer = RunScope::fresh("L");
    auto r = f.owner->submit(std::move(job));
    REQUIRE(r.receipt);
    REQUIRE(eventually([&] { return r.receipt->state == RuntimeJobState::Refused; }));
    CHECK_FALSE(f.composition->runtime()->status(f.next.connection, ConsumerKind::Audio));
    CHECK(f.owner->current());
  }
}

TEST_CASE("Protected actual saved consumer ticket waits join and single deadline never resets", "[protected-runtime]") {
  Fixture f;
  auto gate = f.gate();
  ConsumerCorrelation correlation{"127.0.0.1", 9000, {}};
  REQUIRE(f.owner->runtime()->admit(f.initial.connection,
                                    f.initial.source,
                                    ConsumerKind::Video,
                                    correlation,
                                    [gate](auto &, const auto &) {
                                      gate->wait();
                                      return true;
                                    }) == ConsumerAdmission::Accepted);
  f.owner->runtime()->ping(ConsumerKind::Video, {"127.0.0.1", 9000, {}, {}});
  REQUIRE(eventually([&] { return gate->entered.load(); }));
  auto r = f.owner->submit(f.selection());
  REQUIRE(r.receipt);
  REQUIRE(eventually([&] { return r.receipt->state == RuntimeJobState::Complete; }));
  auto h = f.owner->transfer();
  REQUIRE(h);
  REQUIRE(h->saved_ticket());
  CHECK_FALSE(h->saved_ticket()->complete());
  CHECK_FALSE(f.composition->contains(f.initial.connection));
  CHECK(h->status() == TransferStatus::Pending);
  f.clock->store(16001);
  REQUIRE(eventually([&] { return h->status() == TransferStatus::Quarantined; }));
  CHECK_FALSE(f.owner->current());
  CHECK(f.owner->close_exact(f.op, f.original));
  gate->release();
  REQUIRE(eventually([&] { return h->saved_ticket()->complete(); }));
  CHECK(h->status() == TransferStatus::Quarantined);
  CHECK_FALSE(f.owner->current());
}

TEST_CASE("Protected F12 admission hook and sole Work cleanup keep preparation fenced", "[protected-runtime]") {
  Fixture f;
  auto hook_gate = f.gate(), destructor_gate = f.gate();
  auto h = f.transfer();
  struct Capture {
    std::shared_ptr<TestGate> gate;
    ProtectedRuntimeOwner *owner;
    ~Capture() {
      (void)owner->status();
      gate->wait();
    }
  };
  auto job = f.media(h, RuntimeCell::Audio);
  auto capture = std::make_shared<Capture>();
  capture->gate = destructor_gate;
  capture->owner = f.owner.get();
  job->work = [capture](auto &, const auto &) { return true; };
  capture.reset();
  job->before_cleanup = [hook_gate] { hook_gate->wait(); };
  auto r = f.owner->submit(std::move(job));
  REQUIRE(r.receipt);
  REQUIRE(eventually([&] { return hook_gate->entered.load(); }));
  auto ticket = f.owner->runtime()->revoke(f.next.connection);
  REQUIRE(ticket);
  REQUIRE(eventually([&] { return ticket->complete(); }));
  f.next.connection = ConnectionScope::fresh(42);
  f.next.peer = PeerToken::fresh(0);
  f.next.binding = BindingRevision::fresh(0);
  CHECK(f.owner->submit(f.selection()).result == RuntimeSubmission::Busy);
  hook_gate->release();
  REQUIRE(eventually([&] { return destructor_gate->entered.load(); }));
  CHECK(ticket->complete());
  CHECK(f.owner->status().occupied == 1);
  CHECK(f.owner->submit(f.selection()).result == RuntimeSubmission::Busy);
  destructor_gate->release();
  REQUIRE(eventually([&] { return r.receipt->state == RuntimeJobState::Complete; }));
  auto next = f.transfer();
  CHECK(next->prepared_grant->owner.connection.matches(f.next.connection));
}

TEST_CASE("Protected failure retirement remains charged and independent stop is responsive", "[protected-runtime]") {
  Fixture f;
  auto job = std::make_unique<RuntimeJob>();
  job->cell = RuntimeCell::Retirement;
  job->payload.resize(32000);
  job->retire = [] { throw std::runtime_error("unproved cleanup"); };
  auto r = f.owner->submit(std::move(job));
  REQUIRE(r.receipt);
  REQUIRE(eventually([&] { return r.receipt->state == RuntimeJobState::Unknown; }));
  CHECK(f.owner->status().occupied == 1);
  CHECK(f.owner->status().bytes >= 32000);
  f.owner->request_stop();
  CHECK(f.owner->status().stop);
  CHECK(f.owner->submit(f.selection()).result == RuntimeSubmission::Closed);
  REQUIRE(eventually([&] { return f.owner->join_when_exited() == RuntimeJoin::Joined; }));
  CHECK(f.owner->status().unknown); // join is not Unknown-resource recovery
}

TEST_CASE("Moonlight registration uses exact orchestration Runtime without starting services", "[protected-runtime]") {
  Fixture f;
  auto bus = std::make_shared<wolf::core::events::EventBusType>();
  auto app = immer::box<state::AppState>{
      state::AppState{.event_bus = bus,
                      .lobbies = std::make_shared<immer::atom<immer::vector<wolf::core::events::Lobby>>>(),
                      .running_sessions = f.running}};
  auto handlers = wolf::core::sessions::setup_moonlight_handlers(app, "", {}, f.owner->runtime());
  REQUIRE_FALSE(handlers.empty());
  auto h = f.transfer();
  auto audio = f.owner->submit(f.media(h, RuntimeCell::Audio));
  REQUIRE(audio.receipt);
  REQUIRE(eventually([&] { return audio.receipt->state == RuntimeJobState::Complete; }));
  bus->fire_event(immer::box<wolf::core::events::StopStreamEvent>{
      wolf::core::events::StopStreamEvent{.session_id = f.next.connection.id(),
                                          .connection_scope = f.next.connection}});
  REQUIRE(eventually([&] {
    auto status = f.owner->runtime()->status(f.next.connection, ConsumerKind::Audio);
    return status && status->joined;
  }));
  CHECK(f.owner->current()); // consumer Stop is not GameStop or protected authority handoff
}

TEST_CASE("Protected queue rejects oversized inputs and retirement can coexist with A and V", "[protected-runtime]") {
  Fixture f;
  auto h = f.transfer();
  auto gate = f.gate();
  auto hold = std::make_unique<RuntimeJob>();
  hold->cell = RuntimeCell::Retirement;
  hold->retire = [gate] { gate->wait(); };
  auto r = f.owner->submit(std::move(hold));
  REQUIRE(r.receipt);
  REQUIRE(eventually([&] { return gate->entered.load(); }));
  auto big = f.media(h, RuntimeCell::Audio);
  big->payload.resize(65536);
  CHECK(f.owner->submit(std::move(big)).result == RuntimeSubmission::Capacity);
  auto a = f.owner->submit(f.media(h, RuntimeCell::Audio));
  auto v = f.owner->submit(f.media(h, RuntimeCell::Video));
  REQUIRE(a.receipt);
  REQUIRE(v.receipt);
  CHECK(f.owner->status().occupied == 3);
  CHECK(f.owner->submit(f.selection()).result == RuntimeSubmission::Busy);
  CHECK(f.owner->submit(f.media(h, RuntimeCell::Audio)).result == RuntimeSubmission::Busy);
  CHECK(f.owner->submit({}).result == RuntimeSubmission::Invalid);
  gate->release();
  REQUIRE(eventually([&] { return v.receipt->state == RuntimeJobState::Complete; }));
  CHECK(a.receipt->state == RuntimeJobState::Complete);
  CHECK(a.receipt->media == ConsumerAdmission::Accepted);
  CHECK(v.receipt->media == ConsumerAdmission::Accepted);
}

TEST_CASE("Protected status decides exact deadline while actual supervisor validator is occupied",
          "[protected-runtime]") {
  for (bool backwards : {false, true}) {
    std::atomic_size_t entries{0}; // outlives fixture/supervisor on REQUIRE unwind
    Fixture f;
    auto gate = f.gate();
    auto submitted = f.owner->submit(f.selection([&entries, gate](auto &) {
      if (++entries >= 2)
        gate->wait(); // request first, actual supervisor second
      return true;
    }));
    REQUIRE(submitted.receipt);
    REQUIRE(eventually([&] { return gate->entered.load(); }));
    auto h = f.owner->transfer();
    REQUIRE(h);
    REQUIRE(h->status() == TransferStatus::Pending);
    REQUIRE(h->saved_ticket());
    f.clock->store(14999);
    CHECK_FALSE(f.owner->status().unknown);
    CHECK(h->status() == TransferStatus::Pending);
    CHECK(entries.load() == 2);
    f.clock->store(backwards ? 9999 : 15000);
    CHECK(f.owner->status().unknown);
    CHECK(h->status() == TransferStatus::Quarantined);
    CHECK(entries.load() == 2); // no additional callback from deadline-only decision
    CHECK_FALSE(f.owner->current());
    gate->release();
    CHECK_FALSE(f.owner->current());
    CHECK(h->status() == TransferStatus::Quarantined);
    CHECK(f.owner->submit(f.selection()).result == RuntimeSubmission::Closed);
  }
}

TEST_CASE("Protected deadline observation does not revoke absent or Granted transfer", "[protected-runtime]") {
  Fixture f;
  f.clock->store(100000);
  CHECK_FALSE(f.owner->status().unknown);
  CHECK(f.owner->current());
  auto h = f.transfer();
  f.clock->store(999999);
  CHECK_FALSE(f.owner->status().unknown);
  CHECK(h->status() == TransferStatus::Granted);
  CHECK(f.owner->current());
  auto another = std::make_shared<ConsumerComposition>(f.running);
  ProtectedRuntimeOwner never_started(another);
  CHECK_FALSE(never_started.status().started);
  CHECK_FALSE(never_started.status().unknown);
}

TEST_CASE("Protected trusted clock fault quarantines Pending without status exception", "[protected-runtime]") {
  std::atomic_bool fault{false};
  std::atomic_size_t entries{0};
  Fixture f(false);
  auto bootstrap = f.bootstrap();
  bootstrap.now = [&fault] {
    if (fault)
      throw std::runtime_error("clock fault");
    return Clock::time_point{} + 10s;
  };
  REQUIRE(f.owner->start(std::move(bootstrap)));
  REQUIRE(eventually([&] { return bool(f.owner->current()); }));
  auto gate = f.gate();
  auto submitted = f.owner->submit(f.selection([&entries, gate](auto &) {
    if (++entries >= 2)
      gate->wait();
    return true;
  }));
  REQUIRE(submitted.receipt);
  REQUIRE(eventually([&] { return gate->entered.load(); }));
  auto h = f.owner->transfer();
  REQUIRE(h);
  REQUIRE(h->status() == TransferStatus::Pending);
  fault = true;
  CHECK(f.owner->status().unknown);
  CHECK(h->status() == TransferStatus::Quarantined);
  CHECK(entries.load() == 2);
  gate->release();
}

TEST_CASE("Protected invalid bootstrap never falls back and consumes the domain reservation", "[protected-runtime]") {
  Fixture f(false);
  auto bootstrap = f.bootstrap();
  bootstrap.initial.destination = RunScope::fresh("foreign");
  CHECK_FALSE(f.owner->start(std::move(bootstrap)));
  CHECK(f.io->writes == 0);
  CHECK(f.owner->status().unknown);
  CHECK_FALSE(f.composition->contains(f.initial.connection));
  CHECK_FALSE(f.owner->start(f.bootstrap()));
  CHECK(f.owner->submit(f.selection()).result == RuntimeSubmission::Closed);
}

TEST_CASE("Shared composition preserves exact legacy recognition and unrelated running consumer",
          "[protected-runtime]") {
  std::atomic_bool cancelled{true}; // retained through fixture/Runtime destruction
  Fixture f;
  const auto other = ConnectionScope::fresh(77);
  const auto run = RunScope::fresh("77");
  f.running->store(immer::vector<wolf::core::events::StreamSession>{
      wolf::core::events::StreamSession{.session_id = 77, .connection_scope = other, .run_scope = run}});
  auto gate = f.gate();
  REQUIRE(f.owner->runtime()->admit(other,
                                    run,
                                    ConsumerKind::Video,
                                    {"127.0.0.1", 7788, {}},
                                    [gate, &cancelled](auto &job, const auto &) {
                                      cancelled = job.cancelled();
                                      gate->wait();
                                      cancelled = job.cancelled();
                                      return true;
                                    }) == ConsumerAdmission::Accepted);
  CHECK(f.owner->runtime()->admit(ConnectionScope::fresh(77), run, ConsumerKind::Audio, {}, [](auto &, const auto &) {
    return true;
  }) == ConsumerAdmission::Stale);
  f.owner->runtime()->ping(ConsumerKind::Video, {"127.0.0.1", 7788, {}, {}});
  REQUIRE(eventually([&] { return gate->entered.load(); }));
  auto h = f.transfer();
  auto audio = f.owner->submit(f.media(h, RuntimeCell::Audio));
  REQUIRE(audio.receipt);
  REQUIRE(eventually([&] { return audio.receipt->state == RuntimeJobState::Complete; }));
  // Actual new admission prunes obsolete old record; saved ticket remains independent.
  REQUIRE(h->saved_ticket());
  CHECK(h->saved_ticket()->complete());
  f.owner->request_stop();
  auto unrelated = f.owner->runtime()->status(other, ConsumerKind::Video);
  REQUIRE(unrelated);
  CHECK(unrelated->state == ConsumerState::Running);
  CHECK_FALSE(cancelled.load());
  gate->release();
  REQUIRE(eventually([&] {
    auto done = f.owner->runtime()->status(other, ConsumerKind::Video);
    return done && done->joined;
  }));
  CHECK_FALSE(cancelled.load());
  CHECK(run.matches(f.running->load()->front().run_scope));
}
