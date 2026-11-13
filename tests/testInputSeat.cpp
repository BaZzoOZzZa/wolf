#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <future>
#include <mutex>
#include <sessions/input_handoff.hpp>

using namespace control;
using namespace sessions;
using namespace streaming;
using namespace std::chrono_literals;
namespace {
using Clock = std::chrono::steady_clock;
bool eventually(const std::function<bool()> &fn) {
  auto end = Clock::now() + 2s;
  while (!fn() && Clock::now() < end)
    std::this_thread::sleep_for(1ms);
  return fn();
}
struct Gate {
  std::mutex mutex;
  std::condition_variable cv;
  std::atomic_bool entered{false};
  bool released = false;
  void wait() {
    std::unique_lock lock(mutex);
    entered = true;
    cv.wait_for(lock, 4s, [&] { return released; }); // bounded fallback even during constructor/unwind faults
  }
  void release() {
    std::lock_guard lock(mutex);
    released = true;
    cv.notify_all();
  }
};
struct IO {
  Gate write_gate;
  std::atomic_bool block_write{false}, fail_neutral{false}, fail_listener{false};
  std::mutex mutex;
  std::vector<input_event> writes;
  inputtino::checked::Xbox::Feedback callback;
  inputtino::checked::detail::Io ops() {
    return {
        [this](int fd, const input_event &event, size_t size) {
          if (fd != 42 || size != sizeof(input_event))
            throw std::runtime_error("unexpected device seam");
          if (block_write.exchange(false))
            write_gate.wait();
          if (fail_neutral && event.type == EV_KEY && event.code == BTN_START && event.value == 0)
            return inputtino::checked::IoResult{-1, EAGAIN};
          std::lock_guard lock(mutex);
          writes.push_back(event);
          return inputtino::checked::IoResult{static_cast<std::ptrdiff_t>(size), 0};
        },
        [this](int, int) {
          std::this_thread::sleep_for(1ms);
          return inputtino::checked::IoResult{fail_listener ? -1 : 0, EIO};
        },
        [](int, input_event &, size_t) -> inputtino::checked::IoResult { throw std::runtime_error("unexpected read"); },
        [](int, unsigned long, void *) -> int { throw std::runtime_error("unexpected ioctl"); },
        [] { return Clock::now(); }};
  }
  size_t size() {
    std::lock_guard lock(mutex);
    return writes.size();
  }
  bool contains(int code, int value) {
    std::lock_guard lock(mutex);
    return std::any_of(writes.begin(), writes.end(), [&](auto &e) { return e.code == code && e.value == value; });
  }
};
thread_local bool throw_current_query = false;
struct Current {
  std::mutex mutex;
  std::vector<ConnectionScope> scopes;
  bool contains(const ConnectionScope &scope) {
    if (std::exchange(throw_current_query, false))
      throw std::runtime_error("actual Runtime Current fault");
    std::lock_guard lock(mutex);
    return std::any_of(scopes.begin(), scopes.end(), [&](auto &s) { return s.matches(scope); });
  }
  void add(ConnectionScope scope) {
    std::lock_guard lock(mutex);
    scopes.push_back(std::move(scope));
  }
  void remove(const ConnectionScope &scope) {
    std::lock_guard lock(mutex);
    std::erase_if(scopes, [&](auto &s) { return s.matches(scope); });
  }
};
struct Fixture {
  std::shared_ptr<IO> io = std::make_shared<IO>();
  std::shared_ptr<Current> current = std::make_shared<Current>();
  std::shared_ptr<std::atomic_bool> authorized = std::make_shared<std::atomic_bool>(true);
  std::shared_ptr<std::atomic<int64_t>> time = std::make_shared<std::atomic<int64_t>>(10000);
  RunScope destination = RunScope::fresh("L");
  SeatCandidate initial{RunScope::fresh("42"),
                        destination,
                        ConnectionScope::fresh(42),
                        PeerToken::fresh(1),
                        BindingRevision::fresh(1),
                        0};
  SeatCandidate target{RunScope::fresh("42"),
                       destination,
                       ConnectionScope::fresh(42),
                       PeerToken::fresh(1),
                       BindingRevision::fresh(1),
                       0};
  std::unique_ptr<InputSeatDomain> domain = std::make_unique<InputSeatDomain>();
  std::shared_ptr<InputSeat> seat;
  std::shared_ptr<ConsumerRuntime> runtime;
  std::unique_ptr<InputHandoff> handoff;
  std::vector<std::shared_ptr<Gate>> gates;
  GrantHandle old;
  explicit Fixture(SeatPolicy policy = {}, bool coordinator = true, SeatValidation bootstrap = {}, bool wait_bootstrap = true) {
    current->add(initial.connection);
    current->add(target.connection);
    ConsumerPolicy consumer_policy;
    consumer_policy.poll_interval = 1ms;
    consumer_policy.max_records = 2;
    runtime = std::make_shared<ConsumerRuntime>([state = current](const auto &s) { return state->contains(s); },
                                                consumer_policy);
    seat = domain->create(
        initial,
        {true, true, true},
        bootstrap ? std::move(bootstrap) : SeatValidation{[](const auto &) { return true; }},
        [state = io](auto callback) {
          state->callback = callback;
          auto xbox = inputtino::checked::Xbox::adopt({state, 42}, state->ops(), std::move(callback), [](auto fn) {
            return std::thread(std::move(fn));
          });
          return std::make_shared<CheckedXboxBackend>(std::move(xbox));
        },
        policy);
    if (wait_bootstrap) {
      REQUIRE(eventually([&] { return bool(seat->current()); }));
      old = seat->current();
    }
    if (coordinator)
      handoff = std::make_unique<InputHandoff>(seat, runtime, [clock = time] {
        return Clock::time_point{} + std::chrono::milliseconds(clock->load());
      });
  }
  ~Fixture() {
    io->write_gate.release();
    for (auto &g : gates)
      g->release();
    handoff.reset();
    seat.reset();
    domain.reset();
    runtime.reset();
  }
  SeatCommand command(GrantHandle grant = {}, size_t bytes = 0) {
    SeatCommand c;
    c.grant = grant ? grant : old;
    c.frame.buttons = inputtino::Joypad::START;
    c.packet.resize(bytes);
    return c;
  }
  AuthorizedSelection selection() {
    return {old, target, [allowed = authorized, state = current](const auto &c) {
              return allowed->load() && state->contains(c.connection);
            }};
  }
  TransferResult transfer() {
    return handoff->request(TransferToken::fresh(1), selection());
  }
  std::shared_ptr<Gate> consumer(bool late_holder = false, ConnectionScope scope = {}, RunScope producer = {}) {
    auto gate = std::make_shared<Gate>();
    gates.push_back(gate);
    if (!scope)
      scope = initial.connection;
    if (!producer)
      producer = initial.source;
    struct Holder {
      std::shared_ptr<Gate> gate;
      explicit Holder(std::shared_ptr<Gate> value) : gate(std::move(value)) {}
      ~Holder() {
        gate->wait();
      }
    };
    std::shared_ptr<Holder> holder;
    if (late_holder)
      holder = std::make_shared<Holder>(gate);
    auto result = runtime->admit(scope,
                                 producer,
                                 ConsumerKind::Video,
                                 ConsumerCorrelation{"local", 1234, {}},
                                 [gate, holder, late_holder](ConsumerJob &job, const ConsumerPing &) {
                                   job.begin_drain(); // EOS is not resource completion
                                   if (!late_holder)
                                     gate->wait();
                                   return true;
                                 });
    if (result != ConsumerAdmission::Accepted)
      gate->release();
    REQUIRE(result == ConsumerAdmission::Accepted);
    holder.reset();
    runtime->ping(ConsumerKind::Video, ConsumerPing{"local", 1234, std::array<char, 16>{}, {}});
    REQUIRE(eventually([&] { return gate->entered.load(); }));
    return gate;
  }
};
} // namespace

TEST_CASE("InputSeat held IO stale FIFO and exact joint consumer retirement", "[input-seat]") {
  Fixture f;
  auto backend = f.seat->backend_identity();
  auto generation = f.seat->generation();
  auto consumer = f.consumer();
  f.io->block_write = true;
  REQUIRE(f.seat->submit(f.command()) == SeatAdmission::Accepted);
  REQUIRE(eventually([&] { return f.io->write_gate.entered.load(); }));
  auto stale = f.command();
  stale.frame.lx = 777;
  REQUIRE(f.seat->submit(std::move(stale)) == SeatAdmission::Accepted);
  auto transaction = f.transfer();
  REQUIRE(transaction.admission == TransferAdmission::Accepted);
  REQUIRE(transaction.handle->saved_ticket());
  CHECK_FALSE(transaction.handle->saved_ticket()->complete());
  CHECK_FALSE(f.handoff->prepared_admission(transaction.handle, f.target.connection, f.target.peer));
  CHECK(f.seat->submit(f.command()) == SeatAdmission::Stale);
  CHECK(transaction.handle->status() == TransferStatus::Pending);
  f.io->write_gate.release();
  REQUIRE(eventually([&] { return f.seat->neutral_complete(); }));
  CHECK_FALSE(f.io->contains(ABS_X, 777));
  CHECK(transaction.handle->status() == TransferStatus::Pending); // neutral despite stuck consumer
  consumer->release();
  REQUIRE(eventually([&] { return transaction.handle->status() == TransferStatus::Granted; }));
  CHECK(f.seat->backend_identity() == backend);
  CHECK(f.seat->generation().matches(generation));
  CHECK(f.seat->current()->owner.destination.matches(f.destination));
  CHECK(f.handoff->prepared_admission(transaction.handle, f.target.connection, f.target.peer));
  CHECK_FALSE(f.handoff->prepared_admission(transaction.handle, f.target.connection, f.old->owner.peer));
  CHECK(f.seat->submit(f.command()) == SeatAdmission::Stale);
  CHECK(f.seat->submit(f.command(f.seat->current())) == SeatAdmission::Accepted);
  CHECK(f.runtime->admit(f.initial.connection,
                         f.initial.source,
                         ConsumerKind::Audio,
                         {"local", 1234, {}},
                         [](auto &, const auto &) { return true; }) == ConsumerAdmission::Revoked);
  CHECK(f.handoff->admit_prepared(transaction.handle,
                                  f.target.connection,
                                  f.target.peer,
                                  ConsumerKind::Audio,
                                  {"local", 1234, {}},
                                  [](auto &, const auto &) { return true; }) == ConsumerAdmission::Accepted);
  CHECK(f.handoff->admit_prepared(transaction.handle,
                                  f.target.connection,
                                  f.old->owner.peer,
                                  ConsumerKind::Video,
                                  {"local", 1234, {}},
                                  [](auto &, const auto &) { return true; }) == ConsumerAdmission::Revoked);
}
TEST_CASE("InputSeat count and byte overflow retain reserved neutral lane and sole domain slot", "[input-seat]") {
  SeatPolicy policy;
  SECTION("count including reserved write") {
    policy.packets = 2;
  }
  SECTION("bytes including reserved write") {
    policy.bytes = 2 * sizeof(SeatCommand) + 16;
  }
  Fixture f(policy);
  f.io->block_write = true;
  REQUIRE(f.seat->submit(f.command()) == SeatAdmission::Accepted);
  REQUIRE(eventually([&] { return f.io->write_gate.entered.load(); }));
  if (policy.packets == 2)
    REQUIRE(f.seat->submit(f.command()) == SeatAdmission::Accepted);
  CHECK(f.seat->submit(f.command({}, 64)) == SeatAdmission::Overflow);
  CHECK(f.seat->quarantined());
  CHECK(f.seat->revoke(f.old, f.old->owner.peer));
  CHECK_FALSE(f.domain->create(f.target, {true, true, true}, [](auto &) { return true; }, {}));
  f.io->write_gate.release();
  REQUIRE(eventually([&] { return f.seat->neutral_complete(); }));
  CHECK_FALSE(f.seat->current());
  CHECK(f.transfer().admission == TransferAdmission::Quarantined);
}
TEST_CASE("InputSeat explicit protected inventory nonpad denial and failed publication", "[input-seat]") {
  SECTION("nonpad routes and selected slot") {
    Fixture f;
    for (auto kind : {InputKind::Keyboard,
                      InputKind::Mouse,
                      InputKind::Text,
                      InputKind::Touch,
                      InputKind::Pen,
                      InputKind::PadExtension}) {
      auto c = f.command();
      c.kind = kind;
      CHECK(f.seat->submit(std::move(c)) == SeatAdmission::Denied);
    }
    auto c = f.command();
    c.slot = 15;
    CHECK(f.seat->submit(std::move(c)) == SeatAdmission::Denied);
    c = f.command();
    c.slot = 16;
    CHECK(f.seat->submit(std::move(c)) == SeatAdmission::Denied);
    c = f.command();
    c.grant = {};
    CHECK(f.seat->submit(std::move(c)) == SeatAdmission::Stale);
    CHECK_FALSE(f.seat->revoke({}, {}));
    CHECK_FALSE(f.seat->revoke(f.old, PeerToken::fresh(1)));
  }
  SECTION("inventory must be explicit and allocation/factory failure consumes cap") {
    InputSeatDomain domain;
    SeatCandidate c{RunScope::fresh("1"),
                    RunScope::fresh("L"),
                    ConnectionScope::fresh(1),
                    PeerToken::fresh(1),
                    BindingRevision::fresh(1),
                    0};
    CHECK_THROWS(domain.create(
        c,
        {},
        [](auto &) { return true; },
        [](auto) -> std::shared_ptr<CheckedXboxBackend> { throw std::bad_alloc(); }));
    CHECK_FALSE(domain.create(c, {true, true, true}, [](auto &) { return true; }, {}));
    InputSeatDomain absent;
    CHECK_THROWS(absent.create(
        c,
        {true, true, true},
        [](auto &) { return true; },
        [](auto) { return std::shared_ptr<CheckedXboxBackend>{}; }));
    InputSeatDomain fault;
    CHECK_THROWS(fault.create(
        c,
        {true, true, true},
        [](auto &) { return true; },
        [](auto) -> std::shared_ptr<CheckedXboxBackend> { throw std::bad_alloc(); }));
    CHECK_FALSE(fault.create(c, {true, true, true}, [](auto &) { return true; }, {}));
  }
}
TEST_CASE("Handoff duplicate busy same connection and old peer identities", "[input-seat]") {
  Fixture f;
  auto consumer = f.consumer();
  auto same = f.selection();
  same.candidate.connection = f.initial.connection;
  CHECK(f.handoff->request(TransferToken::fresh(1), same).admission == TransferAdmission::Invalid);
  CHECK(f.seat->current() == f.old);
  CHECK(f.io->size() == 20);
  auto oldpeer = f.selection();
  oldpeer.candidate.peer = f.old->owner.peer;
  CHECK(f.handoff->request(TransferToken::fresh(1), oldpeer).admission == TransferAdmission::Invalid);
  auto token = TransferToken::fresh(1);
  auto a = f.handoff->request(token, f.selection());
  REQUIRE(a.handle);
  auto b = f.handoff->request(token, f.selection());
  CHECK(b.admission == TransferAdmission::Duplicate);
  CHECK(a.handle == b.handle);
  CHECK(f.transfer().admission == TransferAdmission::Busy);
  CHECK_FALSE(f.handoff->candidate_gone(f.target.connection, f.old->owner.peer));
  consumer->release();
  REQUIRE(eventually([&] { return a.handle->status() == TransferStatus::Granted; }));
  CHECK_FALSE(f.seat->revoke(f.old, f.old->owner.peer));
  CHECK_FALSE(f.seat->revoke(f.seat->current(), f.old->owner.peer));
  CHECK(f.seat->current()->owner.peer.matches(f.target.peer));
}
TEST_CASE("Handoff auth binding pending disconnect and backend faults deny delayed grant", "[input-seat]") {
  Fixture f;
  auto consumer = f.consumer();
  auto selection = f.selection();
  auto binding_valid = std::make_shared<std::atomic_bool>(true);
  selection.validate = [binding_valid, auth = f.authorized](auto &) { return binding_valid->load() && auth->load(); };
  auto transaction = f.handoff->request(TransferToken::fresh(1), selection);
  REQUIRE(transaction.handle);
  REQUIRE(eventually([&] { return f.seat->neutral_complete(); }));
  SECTION("auth revocation") {
    *f.authorized = false;
  }
  SECTION("binding lifecycle revision invalidated") {
    *binding_valid = false;
  }
  SECTION("pending target disconnect exact token") {
    CHECK(f.handoff->candidate_gone(f.target.connection, f.target.peer));
  }
  SECTION("listener fails after checked neutral before consumer completion") {
    f.io->fail_listener = true;
    REQUIRE(eventually([&] { return f.seat->quarantined(); }));
  }
  f.handoff->poll();
  consumer->release();
  REQUIRE(eventually([&] { return transaction.handle->status() == TransferStatus::Quarantined; }));
  CHECK_FALSE(f.seat->current());
  CHECK_FALSE(f.handoff->prepared_admission(transaction.handle, f.target.connection, f.target.peer));
}
TEST_CASE("Handoff single fake clock deadline retains Unknown and never late grants", "[input-seat]") {
  Fixture f;
  auto consumer = f.consumer();
  auto transaction = f.transfer();
  REQUIRE(transaction.handle);
  REQUIRE(eventually([&] { return f.seat->neutral_complete(); }));
  *f.time = 14999;
  f.handoff->poll();
  CHECK(transaction.handle->status() == TransferStatus::Pending);
  *f.time = 15000;
  f.handoff->poll();
  CHECK(transaction.handle->status() == TransferStatus::Quarantined);
  consumer->release();
  REQUIRE(eventually([&] { return transaction.handle->saved_ticket()->complete(); }));
  f.handoff->poll();
  CHECK(transaction.handle->status() == TransferStatus::Quarantined);
  CHECK_FALSE(f.seat->current());
  CHECK(f.transfer().admission == TransferAdmission::Quarantined);
  CHECK_FALSE(f.domain->create(f.target, {true, true, true}, [](auto &) { return true; }, {}));
}
TEST_CASE("Handoff neutral failure missing and broken tickets fail closed", "[input-seat]") {
  SECTION("failed neutral despite completed real ticket") {
    Fixture f;
    f.io->fail_neutral = true;
    auto transaction = f.transfer();
    REQUIRE(transaction.handle);
    REQUIRE(eventually([&] { return transaction.handle->saved_ticket()->complete(); }));
    REQUIRE(eventually([&] { return transaction.handle->status() == TransferStatus::Quarantined; }));
    CHECK_FALSE(f.seat->current());
  }
  SECTION("missing real ticket never infers idle") {
    Fixture f;
    f.current->remove(f.initial.connection);
    auto transaction = f.transfer();
    REQUIRE(transaction.handle);
    CHECK_FALSE(transaction.handle->saved_ticket());
    CHECK(transaction.handle->status() == TransferStatus::Quarantined);
    REQUIRE(eventually([&] { return f.seat->neutral_complete(); }));
  }
  SECTION("actual commit predicate rejects broken null and mismatched ticket") {
    auto scope = ConnectionScope::fresh(1);
    std::shared_future<void> broken;
    {
      std::promise<void> promise;
      broken = promise.get_future().share();
    }
    CHECK_FALSE(InputHandoff::drain_complete(ConsumerDrainTicket{scope, broken}, scope));
    CHECK_FALSE(InputHandoff::drain_complete({}, scope));
    std::promise<void> ok;
    auto future = ok.get_future().share();
    ok.set_value();
    CHECK_FALSE(InputHandoff::drain_complete(ConsumerDrainTicket{scope, future}, ConnectionScope::fresh(1)));
    CHECK_FALSE(InputHandoff::drain_complete(ConsumerDrainTicket{{}, future}, {}));
    CHECK(InputHandoff::drain_complete(ConsumerDrainTicket{scope, future}, scope));
  }
}
TEST_CASE("Handoff EOS holder retirement ticket pruning stop before registration and unrelated run", "[input-seat]") {
  Fixture f;
  auto holder = f.consumer(true);
  auto transaction = f.transfer();
  REQUIRE(transaction.handle);
  REQUIRE(eventually([&] { return f.seat->neutral_complete(); }));
  CHECK_FALSE(transaction.handle->saved_ticket()->complete());
  CHECK(transaction.handle->status() == TransferStatus::Pending);
  holder->release();
  REQUIRE(eventually([&] { return transaction.handle->status() == TransferStatus::Granted; }));
  f.current->remove(f.initial.connection);
  auto other = ConnectionScope::fresh(100);
  auto other_run = RunScope::fresh("100");
  f.current->add(other);
  auto other_job = f.consumer(false, other, other_run);
  CHECK_FALSE(f.runtime->status(f.initial.connection, ConsumerKind::Video)); // actual prune
  CHECK(transaction.handle->saved_ticket()->complete());
  CHECK_FALSE(f.runtime->status(other, ConsumerKind::Video)->joined);
  CHECK(f.seat->current()->owner.destination.matches(f.destination));
  other_job->release();
}
TEST_CASE("Handoff feedback captured queued and reserved send lifetimes", "[input-seat]") {
  Fixture f;
  auto router = f.seat->router();
  auto captured = router->capture(100, 200);
  REQUIRE(captured);
  f.io->callback(100, 200);
  auto send = router->reserve_send();
  REQUIRE(send);
  f.io->callback(300, 400);            // queued old grant must be purged
  CHECK_FALSE(router->reserve_send()); // one outstanding send reservation, including Unknown
  auto transaction = f.transfer();
  REQUIRE(transaction.handle);
  CHECK_FALSE(router->enqueue(*captured));
  CHECK_FALSE(router->reserve_send());
  CHECK_FALSE(f.seat->neutral_complete()); // reserved send has not retired
  CHECK(send->feedback.grant == f.old);
  SECTION("actual executor acknowledgement unlocks neutral and grant") {
    send->retire();
    REQUIRE(eventually([&] { return transaction.handle->status() == TransferStatus::Granted; }));
    CHECK_FALSE(router->enqueue(*captured));
    CHECK_FALSE(router->reserve_send());
    f.io->callback(500, 600);
    auto fresh = router->reserve_send();
    REQUIRE(fresh);
    CHECK(fresh->feedback.grant->owner.peer.matches(f.target.peer));
    fresh->retire();
  }
  SECTION("lost executor acknowledgement cannot fake fence") {
    send.reset();
    *f.time = 15000;
    f.handoff->poll();
    CHECK(transaction.handle->status() == TransferStatus::Quarantined);
    CHECK_FALSE(f.seat->neutral_complete());
    CHECK_FALSE(f.seat->current());
  }
  SECTION("failed send acknowledgement quarantines") {
    send->retire(false);
    REQUIRE(eventually([&] { return transaction.handle->status() == TransferStatus::Quarantined; }));
    REQUIRE(eventually([&] { return f.seat->neutral_complete(); }));
  }
}
TEST_CASE("InputSeat healthy held input has no idle timeout and ABA cannot write", "[input-seat]") {
  Fixture f;
  REQUIRE(f.seat->submit(f.command()) == SeatAdmission::Accepted);
  REQUIRE(eventually([&] { return f.io->size() == 40; }));
  *f.time = 1000000;
  f.handoff->poll();
  CHECK(f.seat->current() == f.old);
  CHECK(f.io->size() == 40);
  auto duplicate = std::make_shared<const SeatGrant>(f.initial);
  auto c = f.command(duplicate);
  CHECK(f.seat->submit(std::move(c)) == SeatAdmission::Stale);
  auto impostor = f.selection();
  impostor.expected_old = duplicate;
  CHECK(f.handoff->request(TransferToken::fresh(1), impostor).admission == TransferAdmission::Invalid);
  auto wrong = f.selection();
  wrong.candidate.destination = RunScope::fresh("L");
  CHECK(f.handoff->request(TransferToken::fresh(1), wrong).admission == TransferAdmission::Invalid);
  auto transaction = f.transfer();
  REQUIRE(transaction.handle);
  CHECK(f.runtime->admit(f.initial.connection,
                         f.initial.source,
                         ConsumerKind::Video,
                         {"local", 1234, {}},
                         [](auto &, const auto &) { return true; }) == ConsumerAdmission::Revoked);
  REQUIRE(eventually([&] { return transaction.handle->status() == TransferStatus::Granted; }));
}

TEST_CASE("InputSeat selected slot fifteen uses the same persistent device", "[input-seat]") {
  Fixture f;
  auto backend = f.seat->backend_identity();
  f.target.slot = 15;
  auto transaction = f.transfer();
  REQUIRE(transaction.handle);
  REQUIRE(eventually([&] { return transaction.handle->status() == TransferStatus::Granted; }));
  auto command = f.command(f.seat->current());
  CHECK(f.seat->submit(command) == SeatAdmission::Denied); // configured slot0 is no longer selected
  command.slot = 15;
  CHECK(f.seat->submit(std::move(command)) == SeatAdmission::Accepted);
  CHECK(f.seat->backend_identity() == backend);
}
TEST_CASE("InputSeat feedback overflow and validator exception are fail closed", "[input-seat]") {
  Fixture f;
  SECTION("fixed feedback queue overflow") {
    for (int i = 0; i < 257; ++i)
      f.io->callback(i, i);
    CHECK(f.seat->quarantined());
    CHECK_FALSE(f.seat->router()->reserve_send());
    REQUIRE(eventually([&] { return f.seat->neutral_complete(); }));
  }
  SECTION("exception in current authorization check") {
    auto candidate = f.selection();
    candidate.validate = [](const auto &) -> bool { throw std::bad_alloc(); };
    CHECK(f.handoff->request(TransferToken::fresh(1), std::move(candidate)).admission ==
          TransferAdmission::Quarantined);
    REQUIRE(eventually([&] { return f.seat->neutral_complete(); }));
    CHECK_FALSE(f.seat->current());
  }
}
TEST_CASE("Handoff exact notification and terminal backend fault after grant revoke future admission", "[input-seat]") {
  Fixture f;
  auto transaction = f.transfer();
  REQUIRE(transaction.handle);
  REQUIRE(eventually([&] { return transaction.handle->status() == TransferStatus::Granted; }));
  SECTION("notification racing just after commit") {
    CHECK_FALSE(f.handoff->candidate_gone(f.target.connection, f.old->owner.peer));
    CHECK(f.handoff->candidate_gone(f.target.connection, f.target.peer));
  }
  SECTION("terminal listener failure after healthy commit") {
    f.io->fail_listener = true;
    REQUIRE(eventually([&] { return f.seat->quarantined(); }));
  }
  CHECK_FALSE(f.seat->current());
  CHECK_FALSE(f.handoff->prepared_admission(transaction.handle, f.target.connection, f.target.peer));
  CHECK(f.handoff->admit_prepared(transaction.handle,
                                  f.target.connection,
                                  f.target.peer,
                                  ConsumerKind::Audio,
                                  {"local", 1234, {}},
                                  [](auto &, const auto &) { return true; }) == ConsumerAdmission::Revoked);
}

TEST_CASE("InputSeat owned worker launch failures roll back unpublished backend and retain cap", "[input-seat]") {
  InputSeatDomain domain;
  auto io = std::make_shared<IO>();
  std::weak_ptr<int> resource;
  SeatCandidate candidate{RunScope::fresh("1"),
                          RunScope::fresh("L"),
                          ConnectionScope::fresh(1),
                          PeerToken::fresh(1),
                          BindingRevision::fresh(1),
                          0};
  auto factory = [&](auto callback) {
    auto owned = std::make_shared<int>(42);
    resource = owned;
    auto xbox =
        inputtino::checked::Xbox::adopt({std::move(owned), 42}, io->ops(), std::move(callback), start_seat_thread);
    return std::make_shared<CheckedXboxBackend>(std::move(xbox));
  };
  SeatThreadFactory start;
  SECTION("thread launch exception") {
    start = [](auto) -> std::thread {
      throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
    };
  }
  SECTION("factory returns no thread") {
    start = [](auto) { return std::thread{}; };
  }
  REQUIRE_THROWS(
      domain.create(candidate, {true, true, true}, [](auto &) { return true; }, factory, {}, std::move(start)));
  CHECK(resource.expired());
  CHECK_FALSE(domain.create(candidate, {true, true, true}, [](auto &) { return true; }, factory));
}
TEST_CASE("Handoff supervisor launch failures quarantine retained seat without replacement", "[input-seat]") {
  Fixture f({}, false);
  SeatThreadFactory start;
  SECTION("thread launch exception") {
    start = [](auto) -> std::thread {
      throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
    };
  }
  SECTION("factory returns no thread") {
    start = [](auto) { return std::thread{}; };
  }
  CHECK_THROWS(InputHandoff(f.seat, f.runtime, [] { return Clock::now(); }, std::move(start)));
  CHECK(f.seat->quarantined());
  CHECK_FALSE(f.seat->current());
  REQUIRE(eventually([&] { return f.seat->neutral_complete(); }));
  CHECK_THROWS(InputHandoff(f.seat, f.runtime)); // reserved supervisor slot is not reusable
}
namespace {
// No Catch assertions in worker threads. Release-before-join is unconditional,
// including assertion unwind. RED never executes a known deadlocking reentry.
struct CleanupProbe {
  Gate gate;
  std::atomic_bool progress{false}, allow_reentry{false}, reentered{false},
      destroyed{false};
  std::function<void()> reenter;
};
struct CleanupHolder {
  std::shared_ptr<CleanupProbe> probe;
  explicit CleanupHolder(std::shared_ptr<CleanupProbe> p)
      : probe(std::move(p)) {}
  ~CleanupHolder() {
    probe->gate.wait();
    if (probe->allow_reentry) {
      probe->reenter();
      probe->reentered = true;
    }
    probe->destroyed = true;
  }
};
struct CleanupThreads {
  std::shared_ptr<CleanupProbe> probe;
  std::jthread caller, progress;
  explicit CleanupThreads(std::shared_ptr<CleanupProbe> p)
      : probe(std::move(p)) {}
  ~CleanupThreads() { probe->gate.release(); } // before jthread members join
  void finish() {
    probe->gate.release();
    if (caller.joinable())
      caller.join();
    if (progress.joinable())
      progress.join();
  }
};
void probe_cleanup(CleanupThreads &threads, Fixture &f,
                   bool neutral_progress = true) {
  auto p = threads.probe;
  CHECK(eventually([&] { return p->gate.entered.load(); }));
  threads.progress = std::jthread([&, p, neutral_progress] {
    auto current = f.seat->current();
    if (current)
      f.seat->revoke(current, current->owner.peer);
    if (f.handoff)
      f.handoff->poll();
    // Bootstrap cleanup runs on the seat worker itself; it cannot run another
    // neutral until its own destructor returns. Gate progress is still
    // required.
    p->progress = !neutral_progress ||
                  eventually([&] { return f.seat->neutral_complete(); });
  });
  const bool progressed_before_cleanup =
      eventually([&] { return p->progress.load(); });
  CHECK(progressed_before_cleanup);
  p->allow_reentry = progressed_before_cleanup;
  threads.finish();
  CHECK(eventually([&] { return p->destroyed.load(); }));
  CHECK(p->reentered.load());
}
std::shared_ptr<CleanupProbe> cleanup_probe(Fixture &f) {
  auto p = std::make_shared<CleanupProbe>();
  p->reenter = [&] {
    (void)f.seat->current();
    auto grant = f.seat->current();
    if (grant)
      f.seat->revoke(grant, grant->owner.peer);
    else
      f.seat->revoke(f.old, f.old->owner.peer);
  };
  return p;
}
} // namespace

TEST_CASE(
    "F12 prepared Work cleanup releases seat gate on actual Runtime paths",
    "[input-seat][f12]") {
  for (int path = 0; path != 4; ++path) {
    INFO("path: duplicate=0 early-refusal=1 Runtime-Current-throw=2 "
         "fast-success=3: "
         << path);
    Fixture f;
    auto selection = f.selection();
    selection.validate = [](const auto &) {
      return true;
    }; // fault belongs to Runtime Current, not validator
    auto transfer =
        f.handoff->request(TransferToken::fresh(1), std::move(selection));
    REQUIRE(eventually(
        [&] { return transfer.handle->status() == TransferStatus::Granted; }));
    const auto grant = f.seat->current();
    REQUIRE(grant);
    if (path == 0) {
      REQUIRE(f.handoff->admit_prepared(transfer.handle, f.target.connection,
                                        f.target.peer, ConsumerKind::Video,
                                        {"local", 1234, {}},
                                        [](auto &, const auto &) {
                                          return true;
                                        }) == ConsumerAdmission::Accepted);
    }
    auto p = cleanup_probe(f);
    std::atomic<ConsumerAdmission> result{ConsumerAdmission::Stale};
    std::atomic_bool ran{false};
    CleanupThreads threads(p);
    threads.caller = std::jthread(
        [&, holder = std::make_shared<CleanupHolder>(p)]() mutable {
          throw_current_query = path == 2;
          result = f.handoff->admit_prepared(
              transfer.handle, f.target.connection,
              path == 1 ? PeerToken::fresh(1) : f.target.peer,
              ConsumerKind::Video, {"local", 1234, {}},
              [owned = std::move(holder), &ran](auto &, const auto &) {
                ran = true;
                return true;
              });
        });
    if (path == 3) {
      CHECK(eventually([&] {
        f.runtime->ping(
            ConsumerKind::Video,
            ConsumerPing{"local", 1234, std::array<char, 16>{}, {}});
        return ran.load();
      }));
    }
    probe_cleanup(threads, f);
    CHECK(result.load() == (path == 0   ? ConsumerAdmission::Duplicate
                            : path == 3 ? ConsumerAdmission::Accepted
                                        : ConsumerAdmission::Revoked));
    CHECK(ran.load() == (path == 3));
    CHECK(f.seat->quarantined());
  }
}

TEST_CASE("F12 bootstrap validator cleanup releases gate before sole capture "
          "retirement",
          "[input-seat][f12]") {
  auto p = std::make_shared<CleanupProbe>();
  Fixture f(
      {}, false,
      [holder = std::make_shared<CleanupHolder>(p)](const auto &) {
        return true;
      },
      false);
  p->reenter = [&] {
    auto grant = f.seat->current();
    if (grant)
      f.seat->revoke(grant, grant->owner.peer);
    (void)f.seat->current();
  };
  CleanupThreads threads(p);
  probe_cleanup(threads, f, false);
  CHECK(eventually([&] { return f.seat->neutral_complete(); }));
}

TEST_CASE("F12 replacing sole terminal transfer retires validator outside gate",
          "[input-seat][f12]") {
  Fixture f;
  auto p = cleanup_probe(f);
  auto selection = f.selection();
  selection.validate = [holder = std::make_shared<CleanupHolder>(p)](
                           const auto &) { return true; };
  auto first =
      f.handoff->request(TransferToken::fresh(1), std::move(selection));
  REQUIRE(eventually(
      [&] { return first.handle->status() == TransferStatus::Granted; }));
  auto next = f.target;
  next.connection = ConnectionScope::fresh(42);
  next.peer = PeerToken::fresh(1);
  f.current->add(next.connection);
  AuthorizedSelection second{f.seat->current(), next,
                             [](const auto &) { return true; }};
  first.handle
      .reset(); // coordinator now holds the only TransferHandle/validator
  CleanupThreads threads(p);
  threads.caller = std::jthread([&, selection = std::move(second)]() mutable {
    (void)f.handoff->request(TransferToken::fresh(2), std::move(selection));
  });
  probe_cleanup(threads, f);
}

TEST_CASE(
    "F12 transaction preparation exception retains validator until unlock",
    "[input-seat][f12]") {
  Fixture f({}, false);
  f.handoff = std::make_unique<InputHandoff>(
      f.seat, f.runtime, []() -> Clock::time_point {
        throw std::runtime_error("actual transaction preparation clock fault");
      });
  auto p = cleanup_probe(f);
  std::atomic<TransferAdmission> result{TransferAdmission::Invalid};
  CleanupThreads threads(p);
  threads.caller =
      std::jthread([&, holder = std::make_shared<CleanupHolder>(p)]() mutable {
        auto selection = f.selection();
        selection.validate = [owned = std::move(holder)](const auto &) {
          return true;
        };
        result =
            f.handoff->request(TransferToken::fresh(1), std::move(selection))
                .admission;
      });
  probe_cleanup(threads, f);
  CHECK(result.load() == TransferAdmission::Quarantined);
}
namespace {
struct AdmissionCleanupGates {
  Gate hook;
  std::shared_ptr<CleanupProbe> callback = std::make_shared<CleanupProbe>();
  std::shared_ptr<CleanupProbe> work = std::make_shared<CleanupProbe>();
  std::jthread caller;
  ~AdmissionCleanupGates() {
    hook.release();
    callback->gate.release();
    work->gate.release();
  }
};
} // namespace
TEST_CASE("F12 real joined ticket cannot bypass owned admission cleanup",
          "[input-seat][f12-fence]") {
  for (bool timeout : {false, true}) {
    INFO("cancel-before-ping and decision timeout: " << timeout);
    Fixture f;
    auto first = f.transfer();
    REQUIRE(eventually(
        [&] { return first.handle->status() == TransferStatus::Granted; }));
    auto next = f.target;
    next.connection = ConnectionScope::fresh(42);
    next.peer = PeerToken::fresh(1);
    f.current->add(next.connection);
    std::atomic_bool ran{false};
    std::atomic<ConsumerAdmission> admitted{ConsumerAdmission::Stale};
    AdmissionCleanupGates gates;
    // Both callbacks reenter actual gate methods on their admission caller
    // thread.
    auto reenter = [&] {
      (void)f.seat->current();
      (void)f.seat->revoke(
          f.old, f.initial.peer); // exact stale grant; cannot revoke target
      f.handoff->poll();
    };
    gates.callback->reenter = reenter;
    gates.work->reenter = reenter;
    gates.caller = std::jthread([&] {
      admitted = f.handoff->admit_prepared(
          first.handle, f.target.connection, f.target.peer, ConsumerKind::Video,
          {"local", 1234, {}},
          [holder = std::make_shared<CleanupHolder>(gates.work),
           &ran](auto &, const auto &) {
            ran = true;
            return true;
          },
          [holder = std::make_shared<CleanupHolder>(gates.callback), &gates] {
            gates.hook.wait();
          });
    });
    REQUIRE(eventually([&] { return gates.hook.entered.load(); }));
    if (!timeout) {
      f.runtime->ping(ConsumerKind::Video,
                      ConsumerPing{"local", 1234, std::array<char, 16>{}, {}});
      REQUIRE(eventually([&] { return ran.load(); }));
    }
    auto ticket = f.runtime->revoke(f.target.connection);
    REQUIRE(ticket);
    REQUIRE(eventually([&] {
      return ticket->complete();
    })); // actual join, outer owners remain
    auto second = f.handoff->request(
        TransferToken::fresh(2),
        AuthorizedSelection{first.handle->prepared_grant, next,
                            [](const auto &) { return true; }});
    REQUIRE(second.handle);
    REQUIRE(eventually([&] { return f.seat->neutral_complete(); }));
    auto progress_and_pending = [&] {
      reenter();
      CHECK(second.handle->saved_ticket()->complete());
      CHECK(second.handle->status() == TransferStatus::Pending);
      CHECK_FALSE(f.seat->current());
    };
    progress_and_pending(); // invocation still retains both callables
    gates.hook.release();
    REQUIRE(eventually([&] { return gates.callback->gate.entered.load(); }));
    progress_and_pending(); // callback capture destructor itself is now blocked
    gates.callback->allow_reentry = true;
    gates.callback->gate.release();
    REQUIRE(eventually([&] { return gates.work->gate.entered.load(); }));
    CHECK(gates.callback->reentered.load());
    progress_and_pending(); // actual sole outer Work capture destructor blocked
    if (timeout) {
      *f.time = 15000;
      f.handoff->poll();
      CHECK(second.handle->status() == TransferStatus::Quarantined);
    }
    gates.work->allow_reentry = true;
    gates.work->gate.release();
    gates.caller.join();
    CHECK(gates.work->destroyed.load());
    CHECK(gates.work->reentered.load());
    CHECK(admitted.load() == ConsumerAdmission::Accepted);
    CHECK(ran.load() == !timeout);
    if (timeout) {
      f.handoff->poll();
      CHECK_FALSE(f.seat->current());
      CHECK(second.handle->status() == TransferStatus::Quarantined);
    } else {
      CHECK(eventually(
          [&] { return second.handle->status() == TransferStatus::Granted; }));
      CHECK(f.seat->current() == second.handle->prepared_grant);
    }
  }
}

TEST_CASE("F12 cleanup slot overflow and throwing hook fail closed",
          "[input-seat][f12-fence]") {
  for (bool overflow : {false, true}) {
    INFO("overflow: " << overflow);
    Fixture f;
    auto transfer = f.transfer();
    REQUIRE(eventually(
        [&] { return transfer.handle->status() == TransferStatus::Granted; }));
    auto hook = std::make_shared<CleanupProbe>();
    std::atomic<ConsumerAdmission> result{ConsumerAdmission::Stale};
    std::atomic_bool ran{false}, cancelled{false};
    CleanupThreads threads(hook);
    threads.caller = std::jthread([&] {
      result = f.handoff->admit_prepared(
          transfer.handle, f.target.connection, f.target.peer,
          ConsumerKind::Video, {"local", 1234, {}},
          [&](ConsumerJob &job, const auto &) {
            ran = true;
            cancelled = eventually([&] { return job.cancelled(); });
            return true;
          },
          [&] {
            hook->gate.wait();
            if (!overflow)
              throw std::runtime_error("injected after-unlock hook fault");
          });
    });
    REQUIRE(eventually([&] { return hook->gate.entered.load(); }));
    f.runtime->ping(ConsumerKind::Video,
                    ConsumerPing{"local", 1234, std::array<char, 16>{}, {}});
    REQUIRE(eventually([&] { return ran.load(); }));
    if (overflow) {
      CHECK(f.handoff->admit_prepared(transfer.handle, f.target.connection,
                                      f.target.peer, ConsumerKind::Audio,
                                      {"local", 1235, {}},
                                      [](auto &, const auto &) {
                                        return true;
                                      }) == ConsumerAdmission::Capacity);
    }
    threads.finish();
    f.handoff->poll();
    CHECK(eventually([&] { return f.seat->quarantined(); }));
    CHECK(eventually([&] { return cancelled.load(); }));
    CHECK(eventually([&] { return f.seat->neutral_complete(); }));
    CHECK_FALSE(f.seat->current());
    CHECK(result.load() == (overflow ? ConsumerAdmission::Accepted
                                     : ConsumerAdmission::Revoked));
  }
}
