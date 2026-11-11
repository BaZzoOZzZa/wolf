#include <catch2/catch_test_macros.hpp>
#include <control/protected_feedback.hpp>

#include <control/control.hpp>
#include <cstring>
#include <events/events.hpp>
#include <future>
#include <moonlight/control.hpp>

using namespace control;
using namespace control::protected_feedback;
using namespace ::sessions;
using namespace streaming;
using namespace moonlight::control;
using namespace std::chrono_literals;
namespace {
const std::string key_hex = "00112233445566778899aabbccddeeff";
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
  bool open = false;
  void wait() {
    std::unique_lock lock(mutex);
    entered = true;
    cv.wait_for(lock, 4s, [&] { return open; }); // bounded even during assertion unwind
  }
  void release() {
    std::lock_guard lock(mutex);
    open = true;
    cv.notify_all();
  }
};
struct OwnerThread {
  std::vector<std::shared_ptr<Gate>> gates;
  std::future<void> result;
  std::thread worker;
  template <class Work> OwnerThread(std::vector<std::shared_ptr<Gate>> g, Work work) : gates(std::move(g)) {
    std::promise<void> done;
    result = done.get_future();
    worker = std::thread([done = std::move(done), work = std::move(work)]() mutable {
      try {
        work();
        done.set_value();
      } catch (...) {
        done.set_exception(std::current_exception());
      }
    });
  }
  ~OwnerThread() {
    for (auto &g : gates)
      g->release();
    if (worker.joinable())
      worker.join();
  }
  void finish() {
    for (auto &g : gates)
      g->release();
    worker.join();
    result.get();
  }
};
struct IO {
  std::atomic_size_t writes{0};
  std::atomic_bool fail{false};
  inputtino::checked::detail::Io ops() {
    return {
        [this](int fd, const input_event &, size_t n) -> inputtino::checked::IoResult {
          if (fd != 42 || n != sizeof(input_event))
            throw std::runtime_error("unexpected device IO");
          ++writes;
          return fail ? inputtino::checked::IoResult{-1, EAGAIN}
                      : inputtino::checked::IoResult{static_cast<std::ptrdiff_t>(n), 0};
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
// Fake only transport decisions/clock gates. Every allocated packet and every
// release below is the REAL pinned ENet create/destroy; no initialize/host/socket.
struct Transport {
  ENetPeer *expected = nullptr;
  ENetPacket *pending = nullptr; // ONLY execution owner touches this pointer
  bool reject = false, fail_create = false, throw_send = false, no_release = false, throw_after_free = false;
  std::atomic_size_t creates{0}, sends{0}, destroys{0}, resets{0}, services{0};
  std::atomic_bool bad_peer{false};
  std::vector<std::string> wire;
  std::shared_ptr<Gate> before_callback, after_callback, after_free, service_tail;
  std::function<void()> nested;
  ENetPacketFreeCallback original = nullptr;
  static thread_local Transport *destroying;
  static void bridge(ENetPacket *packet) {
    auto &self = *destroying;
    if (self.before_callback)
      self.before_callback->wait();
    self.original(packet); // actual production marker, still BEFORE free(data/packet)
    if (self.after_callback)
      self.after_callback->wait();
  }
  void destroy(ENetPacket *packet) {
    original = packet->freeCallback;
    auto *previous = destroying;
    destroying = this;
    packet->freeCallback = bridge;
    enet_packet_destroy(packet); // genuine frees after bridge returns
    destroying = previous;
    ++destroys;
    if (throw_after_free)
      throw std::runtime_error("destroy completed, operation threw");
    if (after_free)
      after_free->wait(); // actual destroy completed, enclosing op has NOT returned
  }
  void release(bool sent) {
    if (pending && !no_release) {
      auto *packet = std::exchange(pending, nullptr);
      if (sent)
        packet->flags |= ENET_PACKET_FLAG_SENT;
      destroy(packet);
    }
  }
  EnetOperations ops(std::shared_ptr<Transport> self) {
    return {std::move(self),
            [](void *p, const void *data, size_t n, enet_uint32 flags) {
              auto &s = *static_cast<Transport *>(p);
              ++s.creates;
              if (s.fail_create)
                return static_cast<ENetPacket *>(nullptr);
              return enet_packet_create(data, n, flags);
            },
            [](void *p, ENetPeer *peer, enet_uint8 channel, ENetPacket *packet) {
              auto &s = *static_cast<Transport *>(p);
              ++s.sends;
              if (peer != s.expected || channel != 0)
                s.bad_peer = true;
              s.wire.emplace_back(reinterpret_cast<const char *>(packet->data), packet->dataLength);
              if (s.reject)
                return -1;
              s.pending = packet;
              if (s.throw_send)
                throw std::runtime_error("uncertain transfer after enqueue");
              return 0;
            },
            [](void *p, ENetPacket *packet) { static_cast<Transport *>(p)->destroy(packet); },
            [](void *p, ENetPeer *peer) {
              auto &s = *static_cast<Transport *>(p);
              ++s.resets;
              if (peer != s.expected)
                s.bad_peer = true;
              s.release(false);
            },
            [](void *p) {
              auto &s = *static_cast<Transport *>(p);
              ++s.services;
              if (s.nested)
                s.nested();
              s.release(true);
              if (s.service_tail)
                s.service_tail->wait();
            }};
  }
};
thread_local Transport *Transport::destroying = nullptr;
struct LiveOwnerFacts {
  ENetPeer *raw;
  PeerToken token;
  std::shared_ptr<ControlPeer> sender;
  wolf::core::events::StreamSession session;
  std::shared_ptr<ControlKeyRegistry> registry;
  ControlKeyState *key;
};
struct Fixture {
  std::shared_ptr<IO> io = std::make_shared<IO>();
  InputSeatDomain seat_domain;
  std::shared_ptr<InputSeat> seat;
  std::shared_ptr<ConsumerRuntime> runtime;
  std::shared_ptr<std::atomic<int64_t>> time = std::make_shared<std::atomic<int64_t>>(10000);
  std::unique_ptr<InputHandoff> handoff;
  std::shared_ptr<std::atomic_bool> runtime_current = std::make_shared<std::atomic_bool>(true);
  RunScope destination = RunScope::fresh("L");
  SeatCandidate initial{RunScope::fresh("42"),
                        destination,
                        ConnectionScope::fresh(42),
                        PeerToken::fresh(0),
                        BindingRevision::fresh(0),
                        0};
  SeatCandidate target{RunScope::fresh("42"),
                       destination,
                       ConnectionScope::fresh(42),
                       PeerToken::fresh(0),
                       BindingRevision::fresh(0),
                       15};
  GrantHandle old;
  wolf::core::events::StreamSession associated{}, current{};
  ENetPeer raw{}; // never given to real send/reset (injected only), no ENet host
  std::shared_ptr<ControlPeer> sender = std::make_shared<ControlPeer>();
  std::shared_ptr<ControlKeyRegistry> keys = std::make_shared<ControlKeyRegistry>();
  ControlKeyState *key = keys->find_or_insert(crypto::hex_to_str(key_hex, true));
  std::shared_ptr<ControlKeyRegistry> retained_keys =
      keys; // key borrow remains alive even in null-registry refusal test
  std::shared_ptr<LiveOwnerFacts> live_facts;
  PeerToken live_token = initial.peer;
  std::shared_ptr<Transport> transport = std::make_shared<Transport>();
  std::unique_ptr<FeedbackDomain> domain = std::make_unique<FeedbackDomain>();
  std::optional<ProtectedFeedbackExecutor> executor;
  bool bound_main = false;
  Fixture(bool bind = true) {
    REQUIRE(key_hex.size() == 32);
    REQUIRE(key);
    seat = seat_domain.create(
        initial,
        {true, true, true},
        [](const auto &) { return true; },
        [state = io](auto cb) {
          return std::make_shared<CheckedXboxBackend>(
              inputtino::checked::Xbox::adopt({state, 42}, state->ops(), std::move(cb), start_seat_thread));
        });
    REQUIRE(seat);
    REQUIRE(eventually([&] { return bool(seat->current()); }));
    old = seat->current();
    runtime = std::make_shared<ConsumerRuntime>(
        [ok = runtime_current](const auto &scope) { return ok->load() && bool(scope); });
    handoff = std::make_unique<InputHandoff>(seat, runtime, [clock = time] {
      return Clock::time_point{} + std::chrono::milliseconds(clock->load());
    });
    associated = {.aes_key = key_hex,
                  .aes_iv = "iv",
                  .session_id = 42,
                  .connection_scope = initial.connection,
                  .run_scope = initial.source};
    associated.control_crypto->mode = EncryptionMode::v2;
    current = associated;
    sender->send = [](std::string_view) -> bool { throw std::runtime_error("LEGACY SEND MUST NOT RUN"); };
    transport->expected = &raw;
    executor = domain->create(seat, transport->ops(transport));
    REQUIRE(executor);
    if (bind) {
      REQUIRE(executor->bind_current_thread());
      bound_main = true;
    }
  }
  ~Fixture() {
    // Tests with packets MUST retire them via their bound owner path before this
    // destructor. No cross-thread emergency destroy or forged completion here.
    if (bound_main && executor) {
      transport->no_release = false;
      for (auto g :
           {transport->before_callback, transport->after_callback, transport->after_free, transport->service_tail})
        if (g)
          g->release();
      transport->nested = {};
      executor->service(); // safe unwind via the SAME bound owner, never guessed packet destroy
    }
    executor.reset();
    domain.reset();
    handoff.reset();
    runtime.reset();
  }
  CurrentOwnerSource source() {
    live_facts =
        std::make_shared<LiveOwnerFacts>(LiveOwnerFacts{&raw, live_token, sender, current, retained_keys, key});
    return {live_facts,
            [](void *p,
               ENetPeer &raw,
               const PeerToken &token,
               const std::shared_ptr<ControlPeer> &sender,
               const wolf::core::events::StreamSession &session,
               ControlKeyState &key) {
              const auto &live = *static_cast<LiveOwnerFacts *>(p);
              return live.raw == &raw && live.token.matches(token) && live.sender == sender && live.key == &key &&
                     same_control_generation(session, live.session);
            }};
  }
  OwnerPeerView view(GrantHandle grant = {}) {
    if (!grant)
      grant = old;
    return OwnerPeerView(raw,
                         associated,
                         current,
                         grant->owner.peer,
                         sender,
                         sender,
                         keys,
                         *key,
                         grant,
                         seat->generation(),
                         source());
  }
  TransferResult transfer() {
    return handoff->request(TransferToken::fresh(1), {old, target, [](const auto &) { return true; }});
  }
  void effect(int low = 1, int high = 2) {
    seat->router()->intake(low, high);
  }
};
} // namespace

TEST_CASE("Protected feedback uses actual crypto and retires only after real packet free", "[feedback]") {
  Fixture f;
  auto backend = f.seat->backend_identity();
  auto device = f.seat->generation();
  f.effect(0, 65535);
  auto view = f.view();
  REQUIRE(f.executor->pump_one(view) == PumpResult::Queued);
  REQUIRE(f.executor->status().occupied);
  REQUIRE(f.transport->wire.size() == 1);
  auto plain = decrypt_packet(f.transport->wire[0], key_hex, EncryptionMode::v2, Direction::host);
  REQUIRE(crypto::str_to_hex(plain) == "0B010A000000000000000000FFFF");
  REQUIRE_THROWS(decrypt_packet(f.transport->wire[0], key_hex, EncryptionMode::v2, Direction::client));
  REQUIRE(f.key->next_sequence == 1);
  auto transfer = f.transfer();
  REQUIRE(transfer.handle);
  REQUIRE(InputHandoff::drain_complete(transfer.handle->saved_ticket(), f.initial.connection));
  REQUIRE(transfer.handle->status() == TransferStatus::Pending);
  REQUIRE_FALSE(f.seat->neutral_complete());
  REQUIRE(f.executor->service());
  REQUIRE(f.transport->destroys == 1);
  REQUIRE(f.executor->status().outcome == Outcome::SentReleased);
  REQUIRE(eventually([&] { return transfer.handle->status() == TransferStatus::Granted; }));
  REQUIRE(f.seat->generation().matches(device));
  REQUIRE(f.seat->backend_identity() == backend);
  // Fresh connection AND fresh token, same ENet pointer and SAME actual key registry.
  auto fresh = f.seat->current();
  // Old view reuses the exact raw address: it must neither send nor revoke fresh.
  REQUIRE(f.executor->pump_one(view) == PumpResult::Refused);
  REQUIRE_FALSE(f.seat->quarantined());
  REQUIRE(f.seat->current()->token.matches(fresh->token));
  REQUIRE(f.key->next_sequence == 1);
  f.live_token = f.target.peer;
  f.associated.connection_scope = f.target.connection;
  f.associated.run_scope = f.target.source;
  f.current = f.associated;
  f.effect(65535, 0);
  auto next = f.view(fresh);
  REQUIRE(f.executor->pump_one(next) == PumpResult::Queued);
  REQUIRE(f.key->next_sequence == 2);
  REQUIRE(crypto::str_to_hex(decrypt_packet(f.transport->wire.back(), key_hex, EncryptionMode::v2, Direction::host)) ==
          "0B010A00000000000F00FFFF0000");
  REQUIRE_FALSE(f.executor->cancel_for_transfer(next, transfer.handle)); // actual old Granted handle, fresh active cell
  REQUIRE(f.executor->service());
  REQUIRE_FALSE(f.transport->bad_peer);
}

TEST_CASE("Protected feedback refuses stale owner fields without sending", "[feedback]") {
  Fixture f;
  auto grant = f.old;
  auto token = grant->owner.peer;
  auto sender = f.sender;
  auto device = f.seat->generation();
  bool foreign_grant = false;
  SECTION("fresh token at identical raw peer") {
    token = PeerToken::fresh(0);
  }
  SECTION("missing token") {
    token = {};
  }
  SECTION("missing sender") {
    sender.reset();
  }
  SECTION("different sender identity") {
    sender = std::make_shared<ControlPeer>();
  }
  SECTION("missing device") {
    device = {};
  }
  SECTION("stale live connection") {
    f.current.connection_scope = ConnectionScope::fresh(42);
  }
  SECTION("stale source run") {
    f.current.run_scope = RunScope::fresh("42");
  }
  SECTION("new binding") {
    foreign_grant = true;
    auto c = grant->owner;
    c.binding = BindingRevision::fresh(0);
    grant = std::make_shared<SeatGrant>(c);
  }
  SECTION("other destination") {
    foreign_grant = true;
    auto c = grant->owner;
    c.destination = RunScope::fresh("other");
    grant = std::make_shared<SeatGrant>(c);
  }
  SECTION("legacy crypto") {
    f.associated.control_crypto->mode = EncryptionMode::legacy;
  }
  SECTION("missing registry") {
    f.keys.reset();
  }
  f.effect();
  OwnerPeerView
      view(f.raw, f.associated, f.current, token, f.sender, sender, f.keys, *f.key, grant, device, f.source());
  REQUIRE(f.executor->pump_one(view) == PumpResult::Refused);
  REQUIRE(f.transport->creates == 0);
  REQUIRE(f.transport->sends == 0);
  REQUIRE(f.key->next_sequence == 0);
  if (foreign_grant) {
    REQUIRE_FALSE(f.seat->quarantined());
    REQUIRE(f.seat->current()->token.matches(f.old->token));
  } else {
    REQUIRE(f.seat->quarantined());
  }
}

TEST_CASE("Protected feedback validates ranges and spends failed attempt nonces", "[feedback]") {
  Fixture f;
  bool expect_nonce = true;
  AttemptHook hook;
  SECTION("negative low") {
    f.effect(-1, 2);
    expect_nonce = false;
  }
  SECTION("oversize high") {
    f.effect(1, 65536);
    expect_nonce = false;
  }
  SECTION("encryption boundary throws") {
    f.effect();
    hook = [](HookPoint p) {
      if (p == HookPoint::BeforeEncrypt)
        throw std::runtime_error("encrypt fault");
    };
  }
  SECTION("real encrypt_packet rejects invalid key after nonce reservation") {
    f.effect();
    hook = [&](HookPoint p) {
      if (p == HookPoint::BeforeEncrypt)
        f.associated.aes_key = "00";
    };
  }
  SECTION("fresh owner source changes after encryption") {
    f.effect();
    hook = [&](HookPoint p) {
      if (p == HookPoint::AfterEncrypt)
        f.live_facts->token = PeerToken::fresh(0);
    };
  }
  SECTION("create fails") {
    f.effect();
    f.transport->fail_create = true;
  }
  SECTION("send rejects caller still owns") {
    f.effect();
    f.transport->reject = true;
  }
  SECTION("throw after genuine packet acquired") {
    f.effect();
    hook = [](HookPoint p) {
      if (p == HookPoint::BeforeSend)
        throw std::runtime_error("before send");
    };
  }
  auto view = f.view();
  REQUIRE(f.executor->pump_one(view, std::move(hook)) == PumpResult::Failed);
  REQUIRE(f.key->next_sequence == (expect_nonce ? 1 : 0));
  REQUIRE(f.seat->quarantined());
  REQUIRE(f.executor->status().outcome == Outcome::FailedReleased);
  REQUIRE_FALSE(f.executor->status().packet_live);
}

TEST_CASE("Protected feedback actual registry exhaustion cannot wrap or reset", "[feedback]") {
  Fixture f;
  f.key->next_sequence = UINT32_MAX;
  auto view = f.view();
  f.effect();
  REQUIRE(f.executor->pump_one(view) == PumpResult::Queued);
  REQUIRE(f.executor->service());
  REQUIRE(f.key->next_sequence == uint64_t(UINT32_MAX) + 1);
  f.effect();
  REQUIRE(f.executor->pump_one(view) == PumpResult::Failed);
  REQUIRE(f.key->next_sequence == uint64_t(UINT32_MAX) + 1);
  REQUIRE(f.transport->sends == 1);
  for (uint32_t i = 0; i < ControlKeyRegistry::capacity - 1; ++i) {
    std::string key(16, '\0');
    std::memcpy(key.data(), &i, sizeof(i));
    REQUIRE(f.keys->find_or_insert(key));
  }
  REQUIRE_FALSE(f.keys->find_or_insert(std::string(16, 'z')));
  REQUIRE(f.keys->find_or_insert(crypto::hex_to_str(key_hex, true)) == f.key);
}

TEST_CASE("Healthy feedback cancellation requires real Pending handoff and actual reset retirement", "[feedback]") {
  Fixture f;
  auto view = f.view();
  f.effect();
  REQUIRE(f.executor->pump_one(view) == PumpResult::Queued);
  REQUIRE_FALSE(f.executor->cancel_for_transfer(view, {}));
  auto transaction = f.transfer();
  REQUIRE(transaction.handle);
  SECTION("exact healthy cancellation") {
    REQUIRE(f.executor->cancel_for_transfer(view, transaction.handle));
    REQUIRE(f.transport->resets == 1);
    REQUIRE(f.executor->status().outcome == Outcome::CancelledReleased);
    REQUIRE(eventually([&] { return transaction.handle->status() == TransferStatus::Granted; }));
    REQUIRE_FALSE(f.executor->cancel_for_transfer(view, transaction.handle)); // granted, cell retired
  }
  SECTION("foreign transfer is not authority") {
    Fixture foreign;
    auto other = foreign.transfer();
    REQUIRE(other.handle);
    REQUIRE_FALSE(f.executor->cancel_for_transfer(view, other.handle));
    REQUIRE(f.transport->resets == 0);
    REQUIRE(f.executor->cancel_for_transfer(view, transaction.handle));
  }
  SECTION("replaced raw peer token refused") {
    OwnerPeerView wrong(f.raw,
                        f.associated,
                        f.current,
                        PeerToken::fresh(0),
                        f.sender,
                        f.sender,
                        f.keys,
                        *f.key,
                        f.old,
                        f.seat->generation(),
                        f.source());
    REQUIRE_FALSE(f.executor->cancel_for_transfer(wrong, transaction.handle));
    REQUIRE(f.executor->cancel_for_transfer(view, transaction.handle));
  }
  SECTION("normal revoke cannot create healthy cancellation") {
    REQUIRE(f.seat->revoke(f.old, f.initial.peer));
    f.handoff->poll();
    REQUIRE_FALSE(f.executor->cancel_for_transfer(view, transaction.handle));
    REQUIRE(f.executor->service());
    REQUIRE(transaction.handle->status() == TransferStatus::Quarantined);
    REQUIRE(f.executor->status().sealed);
  }
}

TEST_CASE("Unknown send ownership and missing completion retain cap until real owner release", "[feedback]") {
  Fixture f;
  auto view = f.view();
  f.effect();
  SECTION("send throw after transfer") {
    f.transport->throw_send = true;
    REQUIRE(f.executor->pump_one(view) == PumpResult::Unknown);
    REQUIRE(f.executor->status().sealed);
  }
  SECTION("accepted queue with no completion until deadline") {
    REQUIRE(f.executor->pump_one(view) == PumpResult::Queued);
    f.transport->no_release = true;
    REQUIRE(f.executor->service());
    auto transfer = f.transfer();
    REQUIRE(transfer.handle);
    *f.time = 15000;
    f.handoff->poll();
    REQUIRE(transfer.handle->status() == TransferStatus::Quarantined);
    REQUIRE(f.executor->status().occupied);
    f.transport->no_release = false;
  }
  REQUIRE_FALSE(f.domain->create(f.seat, f.transport->ops(f.transport)));
  REQUIRE(f.executor->status().packet_live);
  REQUIRE(f.executor->service()); // proper owner path, genuine destroy, never guessed free
  REQUIRE_FALSE(f.executor->status().packet_live);
  REQUIRE(f.executor->status().sealed);
  REQUIRE(f.executor->status().occupied);
  REQUIRE(f.executor->pump_one(view) == PumpResult::Unknown);
}

TEST_CASE("Callback entry actual free and outer payload cleanup are separate feedback fences", "[feedback]") {
  Fixture f(false);
  auto before = std::make_shared<Gate>(), after = std::make_shared<Gate>(), freed = std::make_shared<Gate>();
  auto payload = std::make_shared<Gate>(), destructor = std::make_shared<Gate>(), resume = std::make_shared<Gate>();
  f.transport->before_callback = before;
  f.transport->after_callback = after;
  f.transport->after_free = freed;
  struct Holder {
    std::shared_ptr<Gate> gate;
    std::shared_ptr<InputSeat> seat;
    ~Holder() {
      (void)seat->current();
      gate->wait();
    }
  };
  auto holder = std::make_shared<Holder>();
  holder->gate = destructor;
  holder->seat = f.seat;
  std::weak_ptr<Holder> weak = holder;
  AttemptHook hook = [owner = std::move(holder), payload](HookPoint p) {
    if (p == HookPoint::BeforePayloadRelease)
      payload->wait();
  };
  f.effect();
  std::atomic_bool queued = false;
  OwnerThread owner({before, after, freed, payload, destructor, resume},
                    [&f, &queued, resume, hook = std::move(hook)]() mutable {
                      if (!f.executor->bind_current_thread())
                        throw std::runtime_error("bind");
                      auto view = f.view();
                      queued = f.executor->pump_one(view, std::move(hook)) == PumpResult::Queued;
                      resume->wait();
                      f.executor->service();
                    });
  REQUIRE(eventually([&] { return queued.load(); }));
  auto transfer = f.transfer();
  REQUIRE(transfer.handle);
  resume->release();
  auto pending = [&] {
    f.handoff->poll();
    CHECK(transfer.handle->status() == TransferStatus::Pending);
    CHECK_FALSE(f.seat->neutral_complete());
    CHECK(f.executor->status().occupied);
  };
  REQUIRE(eventually([&] { return before->entered.load(); }));
  pending();
  before->release();
  REQUIRE(eventually([&] { return after->entered.load(); }));
  pending();
  after->release();
  REQUIRE(eventually([&] { return freed->entered.load(); }));
  pending();
  freed->release();
  REQUIRE(eventually([&] { return payload->entered.load(); }));
  pending();
  CHECK(f.executor->status().cipher_live);
  payload->release();
  REQUIRE(eventually([&] { return destructor->entered.load(); }));
  pending();
  CHECK_FALSE(f.executor->status().cipher_live);
  CHECK(weak.expired());
  destructor->release();
  owner.finish();
  REQUIRE(eventually([&] { return transfer.handle->status() == TransferStatus::Granted; }));
}

TEST_CASE("Nested owner frames and depth bound cannot prematurely retire feedback", "[feedback]") {
  Fixture f;
  auto view = f.view();
  f.effect();
  REQUIRE(f.executor->pump_one(view) == PumpResult::Queued);
  auto transfer = f.transfer();
  REQUIRE(transfer.handle);
  unsigned depth = 0;
  bool nested_pending = false;
  SECTION("outer service still alive after nested actual destruction") {
    f.transport->nested = [&] {
      if (depth++ == 0) {
        f.executor->service();
        nested_pending = transfer.handle->status() == TransferStatus::Pending && !f.seat->neutral_complete();
      }
    };
    REQUIRE(f.executor->service());
    REQUIRE(nested_pending);
    REQUIRE(eventually([&] { return transfer.handle->status() == TransferStatus::Granted; }));
  }
  SECTION("depth eight refuses recursion fail closed") {
    f.transport->nested = [&] {
      ++depth;
      f.executor->service();
    };
    REQUIRE(f.executor->service());
    REQUIRE(depth == 8);
    REQUIRE(f.executor->status().sealed);
    REQUIRE(f.seat->quarantined());
    REQUIRE_FALSE(f.executor->status().packet_live);
  }
  f.transport->nested = {};
}

TEST_CASE("Feedback domain retains callbacks after facade drop and shutdown waits for actual owner", "[feedback]") {
  Fixture f(false);
  auto ready = std::make_shared<Gate>(), release = std::make_shared<Gate>(), after = std::make_shared<Gate>();
  f.transport->after_callback = after;
  f.effect();
  auto facade = *f.executor;
  OwnerThread owner({ready, release, after}, [&f, facade = std::move(facade), ready, release]() mutable {
    facade.bind_current_thread();
    auto view = f.view();
    facade.pump_one(view);
    ready->release();
    release->wait();
    facade.service();
  });
  REQUIRE(eventually([&] {
    std::lock_guard lock(ready->mutex);
    return ready->open;
  }));
  f.executor.reset();
  std::atomic_bool returned = false;
  auto domain = std::move(f.domain);
  OwnerThread lifecycle({release, after}, [domain = std::move(domain), &returned]() mutable {
    domain.reset();
    returned = true;
  });
  REQUIRE(eventually([&] { return f.seat->quarantined(); }));
  REQUIRE_FALSE(returned);
  release->release();
  REQUIRE(eventually([&] { return after->entered.load(); }));
  REQUIRE_FALSE(returned);
  after->release();
  owner.finish();
  lifecycle.finish();
  REQUIRE(returned);
}

TEST_CASE("Feedback logical budget and queue overflow preserve reserved retirement", "[feedback]") {
  REQUIRE(FeedbackDomain::logical_charge() <= 64 * 1024);
  SECTION("bytes rejected before state or packet allocation and domain cannot retry") {
    Fixture f;
    FeedbackDomain another;
    REQUIRE_FALSE(another.create(f.seat, f.transport->ops(f.transport), {FeedbackDomain::logical_charge() - 1, 8}));
    REQUIRE_FALSE(another.create(f.seat, f.transport->ops(f.transport)));
    REQUIRE(f.transport->creates == 0);
  }
  SECTION("256 queued plus one active, overflow closes admission but not active obligation") {
    Fixture f;
    auto view = f.view();
    f.effect();
    REQUIRE(f.executor->pump_one(view) == PumpResult::Queued);
    for (int i = 0; i < 256; ++i)
      f.effect(i, i);
    REQUIRE_FALSE(f.seat->quarantined());
    REQUIRE(f.executor->pump_one(view) == PumpResult::Refused);
    f.effect();
    REQUIRE(f.seat->quarantined());
    REQUIRE(f.executor->status().packet_live);
    REQUIRE_FALSE(f.seat->neutral_complete());
    REQUIRE(f.executor->service());
    REQUIRE(f.executor->status().sealed);
  }
}

TEST_CASE("Cancellation cannot borrow a missing Runtime ticket or an absent owner", "[feedback]") {
  SECTION("actual revoke cannot resolve scope so saved ticket is missing") {
    Fixture f;
    auto view = f.view();
    f.effect();
    REQUIRE(f.executor->pump_one(view) == PumpResult::Queued);
    *f.runtime_current = false;
    auto transaction = f.transfer();
    REQUIRE(transaction.handle);
    REQUIRE_FALSE(transaction.handle->saved_ticket());
    REQUIRE(transaction.handle->status() == TransferStatus::Quarantined);
    REQUIRE_FALSE(f.executor->cancel_for_transfer(view, transaction.handle));
    REQUIRE(f.transport->resets == 0);
    REQUIRE(f.executor->service());
    REQUIRE(f.executor->status().sealed);
  }
  SECTION("execution owner never bound") {
    Fixture f(false);
    auto view = f.view();
    f.effect();
    REQUIRE(f.executor->pump_one(view) == PumpResult::Unknown);
    REQUIRE(f.transport->creates == 0);
    REQUIRE(f.seat->quarantined());
  }
}

TEST_CASE("Known callback followed by throwing enclosing operation is still Unknown", "[feedback]") {
  Fixture f;
  auto view = f.view();
  f.effect();
  REQUIRE(f.executor->pump_one(view) == PumpResult::Queued);
  f.transport->throw_after_free = true;
  REQUIRE_FALSE(f.executor->service());
  REQUIRE(f.transport->destroys == 1);
  REQUIRE_FALSE(f.executor->status().packet_live);
  REQUIRE(f.executor->status().sealed);
  REQUIRE(f.executor->status().occupied);
}

TEST_CASE("Quarantine racing healthy reset and reentrant hook cleanup cannot grant", "[feedback]") {
  Fixture f(false);
  auto resetting = std::make_shared<Gate>(), resume = std::make_shared<Gate>();
  f.transport->after_free = resetting;
  f.effect();
  std::atomic_bool queued = false;
  std::shared_ptr<const TransferHandle> handle;
  OwnerThread owner({resetting, resume}, [&] {
    f.executor->bind_current_thread();
    auto view = f.view();
    queued = f.executor->pump_one(view) == PumpResult::Queued;
    resume->wait();
    f.executor->cancel_for_transfer(view, handle);
  });
  REQUIRE(eventually([&] { return queued.load(); }));
  auto transaction = f.transfer();
  REQUIRE(transaction.handle);
  handle = transaction.handle;
  resume->release();
  REQUIRE(eventually([&] { return resetting->entered.load(); }));
  *f.time = 15000;
  f.handoff->poll();
  REQUIRE(handle->status() == TransferStatus::Quarantined);
  resetting->release();
  owner.finish();
  REQUIRE(handle->status() == TransferStatus::Quarantined);
  REQUIRE(f.seat->quarantined());
  REQUIRE_FALSE(f.seat->current());
}

TEST_CASE("Feedback release waits for real consumer holder retirement and preserves unrelated Work", "[feedback]") {
  Fixture f;
  auto holder_gate = std::make_shared<Gate>();
  // Declared after fixture, before any assertion that could fail with a live
  // worker. Always opens the actual holder destructor gate before Runtime joins.
  struct Release {
    std::shared_ptr<Gate> gate;
    ~Release() {
      gate->release();
    }
  } release{holder_gate};
  struct Holder {
    std::shared_ptr<Gate> gate;
    ~Holder() {
      gate->wait();
    }
  };
  auto holder = std::make_shared<Holder>();
  holder->gate = holder_gate;
  auto began = std::make_shared<std::atomic_bool>(false);
  auto unrelated_started = std::make_shared<std::atomic_bool>(false);
  auto unrelated_stopped = std::make_shared<std::atomic_bool>(false);
  auto other_connection = ConnectionScope::fresh(43);
  auto other_run = RunScope::fresh("43");
  REQUIRE(f.runtime->admit(other_connection,
                           other_run,
                           ConsumerKind::Audio,
                           {"other", 1235, {}},
                           [unrelated_started, unrelated_stopped](ConsumerJob &job, const ConsumerPing &) {
                             *unrelated_started = true;
                             while (!job.cancelled())
                               std::this_thread::sleep_for(1ms);
                             *unrelated_stopped = true;
                             return true;
                           }) == ConsumerAdmission::Accepted);
  f.runtime->ping(ConsumerKind::Audio, {"other", 1235, std::array<char, 16>{}, {}});
  REQUIRE(eventually([&] { return unrelated_started->load(); }));
  REQUIRE(f.runtime->admit(f.initial.connection,
                           f.initial.source,
                           ConsumerKind::Video,
                           {"old", 1234, {}},
                           [h = std::move(holder), began](ConsumerJob &job, const ConsumerPing &) {
                             job.begin_drain(); // actual EOS decision; holder retirement is later
                             *began = true;
                             return true;
                           }) == ConsumerAdmission::Accepted);
  f.runtime->ping(ConsumerKind::Video, {"old", 1234, std::array<char, 16>{}, {}});
  REQUIRE(eventually([&] { return began->load() && holder_gate->entered.load(); }));
  auto backend = f.seat->backend_identity();
  f.effect();
  auto view = f.view();
  REQUIRE(f.executor->pump_one(view) == PumpResult::Queued);
  auto transfer = f.transfer();
  REQUIRE(transfer.handle);
  REQUIRE(f.executor->service());
  REQUIRE(f.executor->status().outcome == Outcome::SentReleased);
  REQUIRE(eventually([&] { return f.seat->neutral_complete(); }));
  REQUIRE_FALSE(InputHandoff::drain_complete(transfer.handle->saved_ticket(), f.initial.connection));
  REQUIRE(transfer.handle->status() == TransferStatus::Pending);
  REQUIRE_FALSE(unrelated_stopped->load());
  holder_gate->release();
  REQUIRE(eventually([&] { return transfer.handle->status() == TransferStatus::Granted; }));
  REQUIRE(f.seat->backend_identity() == backend);
  REQUIRE_FALSE(unrelated_stopped->load());
  auto other_ticket = f.runtime->revoke(other_connection);
  REQUIRE(other_ticket);
  REQUIRE(eventually([&] { return other_ticket->complete(); }));
  REQUIRE(unrelated_stopped->load());
}

TEST_CASE("Feedback cleanup reentry cannot publish a healthy release after revoke", "[feedback]") {
  Fixture f;
  struct Capture {
    std::shared_ptr<InputSeat> seat;
    GrantHandle grant;
    ~Capture() {
      seat->revoke(grant, grant->owner.peer);
    }
  };
  auto capture = std::make_shared<Capture>();
  capture->seat = f.seat;
  capture->grant = f.old;
  f.effect();
  auto view = f.view();
  REQUIRE(f.executor->pump_one(view, [capture = std::move(capture)](HookPoint) {}) == PumpResult::Queued);
  REQUIRE(f.executor->service());
  REQUIRE(f.seat->quarantined());
  REQUIRE(f.executor->status().outcome == Outcome::Unknown);
  REQUIRE(f.executor->status().sealed);
  REQUIRE_FALSE(f.executor->status().packet_live);
}

TEST_CASE("3B1-R1 old pump cannot dequeue feedback after real handoff grants fresh owner", "[feedback][3B1-r1]") {
  Fixture f;
  auto old_view = f.view();
  f.effect(1, 2);
  TransferResult transfer;
  bool granted_inside_boundary = false;
  auto result = f.executor->pump_one(old_view, [&](HookPoint point) {
    if (point == HookPoint::BeforeReserve) {
      transfer = f.transfer();
      granted_inside_boundary = transfer.handle &&
                                eventually([&] { return transfer.handle->status() == TransferStatus::Granted; });
      if (granted_inside_boundary)
        f.effect(123, 456);
    }
  });
  REQUIRE(granted_inside_boundary);
  CHECK(result == PumpResult::Idle); // no operation, not authority/release proof
  CHECK_FALSE(f.seat->quarantined());
  CHECK(f.key->next_sequence == 0);
  CHECK(f.transport->creates == 0);
  auto fresh = f.seat->current();
  REQUIRE(fresh);
  REQUIRE(fresh->owner.connection.matches(f.target.connection));
  f.live_token = f.target.peer;
  f.associated.connection_scope = f.target.connection;
  f.associated.run_scope = f.target.source;
  f.current = f.associated;
  auto fresh_view = f.view(fresh);
  REQUIRE(f.executor->pump_one(fresh_view) == PumpResult::Queued);
  REQUIRE(f.key->next_sequence == 1);
  REQUIRE(f.transport->wire.size() == 1);
  REQUIRE(crypto::str_to_hex(decrypt_packet(f.transport->wire[0], key_hex, EncryptionMode::v2, Direction::host)) ==
          "0B010A00000000000F007B00C801");
  REQUIRE(f.executor->service());
  REQUIRE_FALSE(f.seat->quarantined());
}

TEST_CASE("3B1-R1 null or stale expected reservation preserves actual current queue", "[feedback][3B1-r1]") {
  Fixture f;
  GrantHandle expected;
  SECTION("null is not wildcard") {}
  SECTION("same fields fresh grant allocation is stale") {
    expected = std::make_shared<SeatGrant>(f.old->owner);
  }
  f.effect(123, 456);
  auto rejected = f.seat->router()->reserve_send(expected);
  CHECK_FALSE(rejected);
  // RED teardown only: test owns any accidentally issued reservation; there are
  // no packet/IO resources. This is not an executor completion shortcut.
  if (rejected)
    rejected->retire(true);
  CHECK_FALSE(f.seat->quarantined());
  REQUIRE(f.seat->current()->token.matches(f.old->token));
  REQUIRE(f.key->next_sequence == 0);
  auto view = f.view();
  REQUIRE(f.executor->pump_one(view) == PumpResult::Queued);
  REQUIRE(crypto::str_to_hex(decrypt_packet(f.transport->wire[0], key_hex, EncryptionMode::v2, Direction::host)) ==
          "0B010A000000000000007B00C801");
  REQUIRE(f.executor->service());
}
