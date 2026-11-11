#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <future>
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
  auto large = f.media({}, RuntimeCell::Audio);
  large->payload.resize(30000);
  CHECK(f.owner->submit(std::move(large)).result == RuntimeSubmission::Capacity);
  auto audio = f.owner->submit(f.media({}, RuntimeCell::Audio));
  auto video = f.owner->submit(f.media({}, RuntimeCell::Video));
  auto retirement = std::make_unique<RuntimeJob>();
  retirement->cell = RuntimeCell::Retirement;
  retirement->retire = [] {};
  auto r = f.owner->submit(std::move(retirement));
  REQUIRE(audio.receipt);
  REQUIRE(video.receipt);
  REQUIRE(r.receipt);
  CHECK(f.owner->status().occupied == 4);
  CHECK(f.owner->close_exact(f.op, f.original));
  f.owner->request_stop();
  CHECK(f.owner->status().stop);
  CHECK(f.owner->join_when_exited() == RuntimeJoin::Unknown);
  gate->release();
  REQUIRE(eventually([&] { return f.owner->join_when_exited() == RuntimeJoin::Joined; }));
  CHECK_FALSE(f.owner->current());
  CHECK(audio.receipt->state == RuntimeJobState::Refused);
  CHECK(video.receipt->state == RuntimeJobState::Refused);
  CHECK(r.receipt->state == RuntimeJobState::Refused);
  CHECK(f.owner->status().occupied == 0);
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
