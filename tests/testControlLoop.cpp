#include <control/control_loop.hpp>

#include <control/protected_feedback.hpp>

#include <catch2/catch_test_macros.hpp>
#include <control/control.hpp>
#include <deque>
using namespace control;
using namespace control::loop;
using namespace moonlight::control;
using namespace std::chrono_literals;
namespace {
const std::string key = "00112233445566778899aabbccddeeff";
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
    cv.wait_for(lock, 4s, [&] { return open; });
  }
  void release() {
    std::lock_guard lock(mutex);
    open = true;
    cv.notify_all();
  }
};
struct Fake : std::enable_shared_from_this<Fake> {
  struct Event {
    ENetEventType type;
    size_t slot;
    uint32_t data;
    std::string wire;
  };
  std::mutex mutex;
  std::deque<Event> events;
  std::vector<std::string> wire;
  // Raw host/packet fields accessed ONLY by actual execution owner callbacks.
  ENetHost *host = nullptr;
  std::vector<ENetPacket *> pending;
  std::thread::id owner;
  std::atomic_size_t creates{0}, services{0}, sends{0}, frees{0}, resets{0}, destroys{0};
  std::atomic_bool bad_thread{false}, hold{false}, throw_service{false}, throw_after_receive{false}, host_fail{false};
  std::shared_ptr<Gate> create_gate, service_gate, reset_gate, before_callback, after_callback, after_free;
  ENetPacketFreeCallback marker = nullptr;
  static thread_local Fake *freeing;
  void affinity() {
    if (owner != std::this_thread::get_id())
      bad_thread = true;
  }
  void push(ENetEventType type, size_t slot = 0, uint32_t data = 0, std::string bytes = {}) {
    std::lock_guard lock(mutex);
    events.push_back({type, slot, data, std::move(bytes)});
  }
  static void bridge(ENetPacket *p) {
    auto &s = *freeing;
    std::shared_ptr<Gate> before, after;
    {
      std::lock_guard lock(s.mutex);
      before = s.before_callback;
      after = s.after_callback;
    }
    if (before && (p->flags & ENET_PACKET_FLAG_RELIABLE))
      before->wait();
    if (s.marker)
      s.marker(p);
    if (after && (p->flags & ENET_PACKET_FLAG_RELIABLE))
      after->wait();
  }
  void destroy_packet(ENetPacket *p) {
    affinity();
    const bool gated = p->flags & ENET_PACKET_FLAG_RELIABLE;
    marker = p->freeCallback;
    auto *prev = freeing;
    freeing = this;
    p->freeCallback = bridge;
    enet_packet_destroy(p);
    freeing = prev;
    ++frees;
    std::shared_ptr<Gate> after;
    {
      std::lock_guard lock(mutex);
      after = after_free;
    }
    if (after && gated)
      after->wait();
  }
  void release(bool sent) {
    auto list = std::move(pending);
    pending.clear();
    for (auto *p : list) {
      if (sent)
        p->flags |= ENET_PACKET_FLAG_SENT;
      destroy_packet(p);
    }
  }
  void open_gates() {
    for (auto &g : {create_gate, service_gate, reset_gate, before_callback, after_callback, after_free})
      if (g)
        g->release();
  }
  Operations ops() {
    return {shared_from_this(),
            [](void *p, const char *, uint16_t, size_t n) {
              auto &s = *static_cast<Fake *>(p);
              s.owner = std::this_thread::get_id();
              ++s.creates;
              if (s.create_gate)
                s.create_gate->wait();
              if (s.host_fail)
                return static_cast<ENetHost *>(nullptr);
              auto h = std::make_unique<ENetHost>();
              auto peers = std::make_unique<ENetPeer[]>(n);
              h->peerCount = n;
              h->peers = peers.release();
              s.host = h.release();
              return s.host;
            },
            [](void *p, ENetHost *h, ENetEvent *e, uint32_t) {
              auto &s = *static_cast<Fake *>(p);
              s.affinity();
              ++s.services;
              if (s.service_gate)
                s.service_gate->wait();
              if (s.throw_service.exchange(false))
                throw std::runtime_error("service failure before effect");
              if (!s.hold)
                s.release(true);
              std::optional<Event> next;
              {
                std::lock_guard lock(s.mutex);
                if (!s.events.empty()) {
                  next = std::move(s.events.front());
                  s.events.pop_front();
                }
              }
              if (!next) {
                std::this_thread::sleep_for(1ms);
                return 0;
              }
              *e = {};
              e->type = next->type;
              e->peer = &h->peers[next->slot];
              e->data = next->data;
              if (e->type == ENET_EVENT_TYPE_RECEIVE)
                e->packet = enet_packet_create(next->wire.data(), next->wire.size(), 0);
              if (s.throw_after_receive.exchange(false))
                throw std::runtime_error("valid transfer-full output then failure");
              return 1;
            },
            [](void *p, const void *b, size_t n, uint32_t f) {
              auto &s = *static_cast<Fake *>(p);
              s.affinity();
              return enet_packet_create(b, n, f);
            },
            [](void *p, ENetPeer *, uint8_t, ENetPacket *b) {
              auto &s = *static_cast<Fake *>(p);
              s.affinity();
              s.pending.push_back(b);
              {
                std::lock_guard lock(s.mutex);
                s.wire.emplace_back(reinterpret_cast<char *>(b->data), b->dataLength);
              }
              ++s.sends;
              return 0;
            },
            [](void *p, ENetPacket *b) { static_cast<Fake *>(p)->destroy_packet(b); },
            [](void *p, ENetPeer *) {
              auto &s = *static_cast<Fake *>(p);
              s.affinity();
              ++s.resets;
              if (s.reset_gate)
                s.reset_gate->wait();
              s.release(false);
            },
            [](void *p, ENetPeer *) {
              auto &s = *static_cast<Fake *>(p);
              s.affinity();
              s.release(false);
            },
            [](void *p, ENetHost *) { static_cast<Fake *>(p)->affinity(); },
            [](void *p, ENetHost *h) {
              auto &s = *static_cast<Fake *>(p);
              s.affinity();
              s.release(false);
              delete[] h->peers;
              delete h;
              s.host = nullptr;
              ++s.destroys;
            }};
  }
};
thread_local Fake *Fake::freeing = nullptr;
struct Fixture {
  std::shared_ptr<Fake> fake = std::make_shared<Fake>();
  state::SessionsAtoms sessions = std::make_shared<immer::atom<immer::vector<wolf::core::events::StreamSession>>>();
  std::shared_ptr<wolf::core::events::EventBusType> bus = std::make_shared<wolf::core::events::EventBusType>();
  wolf::core::events::StreamSession session{.aes_key = key,
                                            .aes_iv = "iv",
                                            .session_id = 42,
                                            .enet_secret_payload = 123,
                                            .connection_scope = wolf::core::sessions::ConnectionScope::fresh(42),
                                            .run_scope = wolf::core::sessions::RunScope::fresh("42")};
  std::shared_ptr<std::atomic<int64_t>> now = std::make_shared<std::atomic<int64_t>>(10000);
  ControlLoopDomain domain;
  std::shared_ptr<ControlLoopOwner> loop;
  Config config;
  Fixture() {
    session.control_crypto->mode = EncryptionMode::v2;
    sessions->store(immer::vector<wolf::core::events::StreamSession>{session});
    config.now = [clock = now] { return Clock::time_point{} + std::chrono::milliseconds(clock->load()); };
  }
  ~Fixture() {
    fake->hold = false;
    fake->open_gates();
    if (loop) {
      loop->request_stop();
      while (loop->join_when_exited() != JoinResult::Joined)
        std::this_thread::sleep_for(1ms);
    }
  }
  void start(ThreadFactory factory = [](std::function<void()> work) { return std::thread(std::move(work)); }) {
    loop = domain.start(config, sessions, bus, fake->ops(), std::move(factory));
    REQUIRE(loop);
  }
  void ready() {
    REQUIRE(eventually([&] { return loop->status().ready; }));
  }
  std::string encrypted(uint32_t sequence, std::string payload = crypto::hex_to_str("020302000000")) {
    auto packet = encrypt_packet(key, sequence, payload, EncryptionMode::v2, Direction::client);
    return {reinterpret_cast<char *>(packet.get()), packet->full_size()};
  }
  PeerLease connect(uint32_t sequence = 0) {
    fake->push(ENET_EVENT_TYPE_CONNECT, 0, 123);
    REQUIRE(eventually([&] { return loop->peers().size() == 1; }));
    fake->push(ENET_EVENT_TYPE_RECEIVE, 0, 0, encrypted(sequence));
    REQUIRE(eventually([&] {
      auto p = loop->peers();
      return p.size() == 1 && p[0].authenticated;
    }));
    return loop->peers()[0];
  }
  void finish() {
    loop->request_stop();
    REQUIRE(eventually([&] { return loop->join_when_exited() == JoinResult::Joined; }));
    REQUIRE_FALSE(fake->bad_thread);
  }
  std::string sent(size_t i) {
    std::lock_guard lock(fake->mutex);
    return fake->wire.at(i);
  }
};
} // namespace
TEST_CASE("Owned control actual CONNECT auth replay and reused peer allocation preserve key nonce", "[control-loop]") {
  Fixture f;
  f.start();
  f.ready();
  auto first = f.connect();
  REQUIRE(first.token);
  REQUIRE(first.connection.matches(f.session.connection_scope));
  REQUIRE(first.sender->send("first"));
  REQUIRE(eventually([&] { return f.fake->sends == 1; }));
  REQUIRE(decrypt_packet(f.sent(0), key, EncryptionMode::v2, Direction::host) == "first");
  REQUIRE(read_packet<uint32_t>(std::string_view(f.sent(0)).substr(sizeof(ControlPacket))) == 0);
  f.fake->push(ENET_EVENT_TYPE_DISCONNECT);
  REQUIRE(eventually([&] { return f.loop->peers().empty(); }));
  REQUIRE_FALSE(first.sender->send("stale"));
  f.fake->push(ENET_EVENT_TYPE_CONNECT, 0, 123);
  REQUIRE(eventually([&] { return f.loop->peers().size() == 1; }));
  auto replacement = f.loop->peers()[0];
  REQUIRE_FALSE(replacement.token.matches(first.token));
  auto before = f.fake->frees.load();
  f.fake->push(ENET_EVENT_TYPE_RECEIVE, 0, 0, f.encrypted(0));
  REQUIRE(eventually([&] { return f.fake->frees > before; }));
  REQUIRE_FALSE(f.loop->peers()[0].authenticated);
  f.fake->push(ENET_EVENT_TYPE_RECEIVE, 0, 0, f.encrypted(1));
  REQUIRE(eventually([&] { return f.loop->peers()[0].authenticated; }));
  REQUIRE(replacement.sender->send("second"));
  REQUIRE(eventually([&] { return f.fake->sends == 2; }));
  REQUIRE(read_packet<uint32_t>(std::string_view(f.sent(1)).substr(sizeof(ControlPacket))) == 1);
  REQUIRE(decrypt_packet(f.sent(1), key, EncryptionMode::v2, Direction::host) == "second");
  f.finish();
  REQUIRE_FALSE(replacement.sender->send("closed"));
  REQUIRE(f.fake->destroys == 1);
  REQUIRE_FALSE(f.domain.start(f.config, f.sessions, f.bus, f.fake->ops()));
}
TEST_CASE("Owned control refuses token mismatch and bad cipher before sender authentication", "[control-loop]") {
  Fixture f;
  f.config.peers = 2;
  f.start();
  f.ready();
  f.fake->push(ENET_EVENT_TYPE_CONNECT, 0, 999);
  auto n = f.fake->services.load();
  REQUIRE(eventually([&] { return f.fake->services > n + 2; }));
  REQUIRE(f.loop->peers().empty());
  f.fake->push(ENET_EVENT_TYPE_CONNECT, 0, 123);
  REQUIRE(eventually([&] { return f.loop->peers().size() == 1; }));
  auto bad = f.encrypted(0);
  bad.back() ^= 1;
  auto frees = f.fake->frees.load();
  f.fake->push(ENET_EVENT_TYPE_RECEIVE, 0, 0, bad);
  REQUIRE(eventually([&] { return f.fake->frees > frees; }));
  REQUIRE_FALSE(f.loop->peers()[0].authenticated);
  f.fake->push(ENET_EVENT_TYPE_RECEIVE, 0, 0, f.encrypted(0));
  REQUIRE(eventually([&] { return f.loop->peers()[0].authenticated; }));
  auto first = f.loop->peers()[0];
  f.fake->push(ENET_EVENT_TYPE_CONNECT, 1, 123);
  REQUIRE(eventually([&] { return f.loop->peers().size() == 2; }));
  auto free_before = f.fake->frees.load();
  f.fake->push(ENET_EVENT_TYPE_RECEIVE, 1, 0, f.encrypted(1));
  REQUIRE(eventually([&] { return f.fake->frees > free_before; }));
  auto peers = f.loop->peers();
  REQUIRE(std::count_if(peers.begin(), peers.end(), [](auto &p) { return p.authenticated; }) == 1);
  f.finish();
}
TEST_CASE("Owned control start failures consume slot without escaped worker", "[control-loop]") {
  Fixture f;
  SECTION("thread factory throws") {
    f.start([](std::function<void()>) -> std::thread { throw std::runtime_error("launch"); });
  }
  SECTION("thread factory nonjoinable") {
    f.start([](std::function<void()>) { return std::thread{}; });
  }
  SECTION("host create returns null") {
    f.fake->host_fail = true;
    f.start();
  }
  SECTION("host create throws with unpublished resource rollback") {
    auto ops = f.fake->ops();
    ops.create = [](void *, const char *, uint16_t, size_t) -> ENetHost * {
      auto unpublished = std::make_unique<ENetHost>();
      throw std::runtime_error("host init");
    };
    f.loop = f.domain.start(f.config, f.sessions, f.bus, std::move(ops));
    REQUIRE(f.loop);
  }
  REQUIRE(eventually([&] { return f.loop->status().exited; }));
  REQUIRE(f.loop->status().failed);
  REQUIRE_FALSE(f.loop->status().ready);
  REQUIRE(f.loop->join_when_exited() == JoinResult::Joined);
  REQUIRE_FALSE(f.domain.start(f.config, f.sessions, f.bus, f.fake->ops()));
}
TEST_CASE("Owned control stop before readiness and bounded Unknown retain one domain", "[control-loop]") {
  Fixture f;
  auto gate = std::make_shared<Gate>();
  f.fake->create_gate = gate;
  f.start();
  REQUIRE(eventually([&] { return gate->entered.load(); }));
  REQUIRE_FALSE(f.loop->status().ready);
  f.loop->request_stop();
  *f.now = 15000;
  REQUIRE(f.loop->status().unknown);
  REQUIRE(f.loop->join_when_exited() == JoinResult::Unknown);
  REQUIRE_FALSE(f.domain.start(f.config, f.sessions, f.bus, f.fake->ops()));
  gate->release();
  REQUIRE(eventually([&] { return f.loop->join_when_exited() == JoinResult::Joined; }));
  REQUIRE(f.loop->status().unknown);
  REQUIRE(f.fake->destroys == 1);
}
TEST_CASE("Owned control service exception frees valid received output and joins", "[control-loop]") {
  Fixture f;
  SECTION("failure without output") {
    f.fake->throw_service = true;
  }
  SECTION("valid received packet acquired before service throws") {
    f.fake->push(ENET_EVENT_TYPE_RECEIVE, 0, 0, "test");
    f.fake->throw_after_receive = true;
  }
  f.start();
  REQUIRE(eventually([&] { return f.loop->status().exited; }));
  REQUIRE(f.loop->status().failed);
  REQUIRE(f.loop->join_when_exited() == JoinResult::Joined);
  REQUIRE(f.fake->destroys == 1);
  REQUIRE_FALSE(f.fake->bad_thread);
}
TEST_CASE("Owned control real packet callback and enclosing reset block exit while stop remains nonblocking",
          "[control-loop]") {
  Fixture f;
  f.fake->hold = true;
  f.start();
  f.ready();
  auto peer = f.connect();
  REQUIRE(peer.sender->send("pending"));
  REQUIRE(eventually([&] { return f.fake->sends == 1; }));
  auto before = std::make_shared<Gate>(), after = std::make_shared<Gate>(), freed = std::make_shared<Gate>();
  // No callback is currently running: outgoing held, incoming authentication free complete.
  REQUIRE(eventually([&] { return f.fake->frees >= 1; }));
  {
    std::lock_guard lock(f.fake->mutex);
    f.fake->before_callback = before;
    f.fake->after_callback = after;
    f.fake->after_free = freed;
  }
  f.loop->request_stop();
  REQUIRE(eventually([&] { return before->entered.load(); }));
  REQUIRE_FALSE(f.loop->status().exited);
  REQUIRE(f.loop->join_when_exited() == JoinResult::Refused);
  before->release();
  REQUIRE(eventually([&] { return after->entered.load(); }));
  REQUIRE_FALSE(f.loop->status().exited);
  after->release();
  REQUIRE(eventually([&] { return freed->entered.load(); }));
  REQUIRE_FALSE(f.loop->status().exited);
  *f.now = 15000;
  REQUIRE(f.loop->status().unknown);
  REQUIRE(f.loop->join_when_exited() == JoinResult::Unknown);
  freed->release();
  f.finish();
  REQUIRE(f.loop->status().unknown);
  REQUIRE(f.fake->destroys == 1);
}
TEST_CASE("Owned control join waits actual thread TLS and callable capture retirement", "[control-loop]") {
  Fixture f;
  auto gate = std::make_shared<Gate>();
  struct Lifetime {
    std::shared_ptr<Gate> gate;
    ~Lifetime() {
      gate->wait();
    }
  };
  SECTION("TLS after run function returned") {
    f.start([gate](std::function<void()> work) {
      return std::thread([gate, work = std::move(work)]() mutable {
        thread_local std::unique_ptr<Lifetime> life;
        life = std::make_unique<Lifetime>();
        life->gate = gate;
        work();
      });
    });
  }
  SECTION("outer callable sole capture after inner run returned") {
    auto payload = std::make_shared<Lifetime>();
    payload->gate = gate;
    f.start([payload = std::move(payload)](std::function<void()> work) mutable {
      return std::thread([payload = std::move(payload), work = std::move(work)]() mutable {
        work();
        payload.reset();
      });
    });
  }
  // Gate lives outside fixture: release also on assertion failure before Fixture joins.
  struct Open {
    std::shared_ptr<Gate> g;
    ~Open() {
      g->release();
    }
  } open{gate};
  f.ready();
  f.loop->request_stop();
  REQUIRE(eventually([&] { return gate->entered.load(); }));
  REQUIRE_FALSE(f.loop->status().exited);
  REQUIRE(f.loop->join_when_exited() == JoinResult::Refused);
  gate->release();
  f.finish();
  REQUIRE(f.loop->status().exited);
  REQUIRE(f.loop->status().joined);
}

TEST_CASE("Owned control legacy credit cap includes popped and ENet-owned packets", "[control-loop]") {
  Fixture f;
  f.fake->hold = true;
  f.start();
  f.ready();
  auto peer = f.connect();
  for (size_t i = 0; i < 256; ++i)
    REQUIRE(peer.sender->send(std::string(MAX_PAYLOAD_SIZE, 'x')));
  REQUIRE_FALSE(peer.sender->send("overflow"));
  REQUIRE(eventually([&] { return f.fake->sends == 256; }));
  REQUIRE_FALSE(peer.sender->send("still ENet owned"));
  f.finish();
  REQUIRE(f.fake->frees >= 257);
  REQUIRE_FALSE(peer.sender->send("closed"));
}

TEST_CASE("Owned actual loop optional feedback adapter fences real handoff across service stack", "[control-loop]") {
  Fixture f;
  auto before = std::make_shared<Gate>(), after = std::make_shared<Gate>(), freed = std::make_shared<Gate>();
  f.fake->before_callback = before;
  f.fake->after_callback = after;
  f.fake->after_free = freed;
  f.fake->hold = true;
  f.start();
  f.ready();
  auto first = f.connect();
  auto second_session = f.session;
  second_session.session_id = 43;
  second_session.enet_secret_payload = 456;
  second_session.connection_scope = wolf::core::sessions::ConnectionScope::fresh(43);
  second_session.run_scope = wolf::core::sessions::RunScope::fresh("43");
  second_session.control_crypto = std::make_shared<ControlCryptoState>();
  second_session.control_crypto->mode = EncryptionMode::v2;
  f.sessions->store(immer::vector<wolf::core::events::StreamSession>{f.session, second_session});
  f.fake->push(ENET_EVENT_TYPE_CONNECT, 1, 456);
  REQUIRE(eventually([&] { return f.loop->peers().size() == 2; }));
  f.fake->push(ENET_EVENT_TYPE_RECEIVE, 1, 0, f.encrypted(1));
  REQUIRE(eventually([&] {
    auto p = f.loop->peers();
    return p.size() == 2 && p[0].authenticated && p[1].authenticated;
  }));
  PeerLease second;
  for (auto &p : f.loop->peers())
    if (p.connection.matches(second_session.connection_scope))
      second = p;
  REQUIRE(second.token);
  REQUIRE_FALSE(second.token.matches(first.token));
  InputSeatDomain seat_domain;
  auto destination = wolf::core::sessions::RunScope::fresh("L");
  SeatCandidate initial{f.session.run_scope,
                        destination,
                        f.session.connection_scope,
                        first.token,
                        BindingRevision::fresh(0),
                        0};
  auto writes = std::make_shared<std::atomic_size_t>(0);
  auto seat = seat_domain.create(
      initial,
      {true, true, true},
      [](const auto &) { return true; },
      [writes](auto callback) {
        inputtino::checked::detail::Io io{
            [writes](int fd, const input_event &, size_t size) -> inputtino::checked::IoResult {
              if (fd != 42 || size != sizeof(input_event))
                throw std::runtime_error("unexpected IO");
              ++*writes;
              return {static_cast<std::ptrdiff_t>(size), 0};
            },
            [](int, int) {
              std::this_thread::sleep_for(1ms);
              return inputtino::checked::IoResult{0, 0};
            },
            [](int, input_event &, size_t) -> inputtino::checked::IoResult {
              throw std::runtime_error("unexpected read");
            },
            [](int, unsigned long, void *) -> int { throw std::runtime_error("unexpected ioctl"); },
            [] { return Clock::now(); }};
        return std::make_shared<CheckedXboxBackend>(
            inputtino::checked::Xbox::adopt({writes, 42}, std::move(io), std::move(callback), start_seat_thread));
      });
  REQUIRE(seat);
  REQUIRE(eventually([&] { return bool(seat->current()); }));
  auto backend = seat->backend_identity();
  auto generation = seat->generation();
  REQUIRE(f.loop->attach_feedback(seat));
  REQUIRE_FALSE(f.loop->attach_feedback(seat));
  seat->router()->intake(123, 456);
  REQUIRE(eventually([&] { return f.fake->sends == 1; }));
  auto runtime = std::make_shared<streaming::ConsumerRuntime>([](const auto &s) { return bool(s); });
  ::sessions::InputHandoff handoff(seat, runtime);
  SeatCandidate target{second_session.run_scope,
                       destination,
                       second_session.connection_scope,
                       second.token,
                       BindingRevision::fresh(0),
                       15};
  auto transfer = handoff.request(::sessions::TransferToken::fresh(0),
                                  {seat->current(), target, [](const auto &) { return true; }});
  REQUIRE(transfer.handle);
  REQUIRE(transfer.handle->status() == ::sessions::TransferStatus::Pending);
  f.fake->hold = false;
  REQUIRE(eventually([&] { return before->entered.load(); }));
  REQUIRE_FALSE(seat->neutral_complete());
  REQUIRE(f.loop->status().calls > 0);
  before->release();
  REQUIRE(eventually([&] { return after->entered.load(); }));
  REQUIRE_FALSE(seat->neutral_complete());
  REQUIRE(transfer.handle->status() == ::sessions::TransferStatus::Pending);
  after->release();
  REQUIRE(eventually([&] { return freed->entered.load(); }));
  REQUIRE_FALSE(seat->neutral_complete());
  REQUIRE(transfer.handle->status() == ::sessions::TransferStatus::Pending);
  freed->release();
  REQUIRE(eventually([&] { return transfer.handle->status() == ::sessions::TransferStatus::Granted; }));
  REQUIRE(seat->backend_identity() == backend);
  REQUIRE(seat->generation().matches(generation));
  seat->router()->intake(65535, 0);
  REQUIRE(eventually([&] { return f.fake->sends == 2; }));
  REQUIRE(crypto::str_to_hex(decrypt_packet(f.sent(1), key, EncryptionMode::v2, Direction::host)) ==
          "0B010A00000000000F00FFFF0000");
  REQUIRE(read_packet<uint32_t>(std::string_view(f.sent(1)).substr(sizeof(ControlPacket))) == 1);
  f.finish();
  REQUIRE(seat->quarantined());
  REQUIRE(writes->load() >= 40);
}

TEST_CASE("Owned control stop during service discards late ingress and preserves actual join", "[control-loop]") {
  Fixture f;
  auto gate = std::make_shared<Gate>();
  f.fake->service_gate = gate;
  f.fake->push(ENET_EVENT_TYPE_CONNECT, 0, 123);
  f.start();
  f.ready();
  REQUIRE(eventually([&] { return gate->entered.load(); }));
  f.loop->request_stop();
  REQUIRE(f.loop->status().calls == 1);
  REQUIRE(f.loop->join_when_exited() == JoinResult::Refused);
  gate->release();
  f.finish();
  REQUIRE(f.loop->peers().empty());
  REQUIRE(f.fake->sends == 0);
}

TEST_CASE("Owned control constructor refusal retains start slot without host publication", "[control-loop]") {
  Fixture f;
  f.config.now = []() -> Clock::time_point { throw std::runtime_error("constructor clock"); };
  REQUIRE_FALSE(f.domain.start(f.config, f.sessions, f.bus, f.fake->ops()));
  REQUIRE(f.fake->creates == 0);
  f.config.now = [] { return Clock::now(); };
  REQUIRE_FALSE(f.domain.start(f.config, f.sessions, f.bus, f.fake->ops()));
}
TEST_CASE("Owned control CONNECT reuse retires old association without resetting new allocation", "[control-loop]") {
  Fixture f;
  f.start();
  f.ready();
  auto first = f.connect();
  f.fake->push(ENET_EVENT_TYPE_CONNECT, 0, 123);
  REQUIRE(eventually([&] {
    auto p = f.loop->peers();
    return p.size() == 1 && !p[0].token.matches(first.token);
  }));
  REQUIRE_FALSE(first.sender->send("stale"));
  REQUIRE_FALSE(f.loop->peers()[0].authenticated);
  REQUIRE(f.fake->resets == 0);
  f.fake->push(ENET_EVENT_TYPE_RECEIVE, 0, 0, f.encrypted(1));
  REQUIRE(eventually([&] { return f.loop->peers()[0].authenticated; }));
  REQUIRE(f.loop->peers()[0].sender->send("new allocation"));
  REQUIRE(eventually([&] { return f.fake->sends == 1; }));
  REQUIRE(decrypt_packet(f.sent(0), key, EncryptionMode::v2, Direction::host) == "new allocation");
  f.finish();
}

TEST_CASE("Owned control legacy send rejection and transferred throw preserve actual packet ownership",
          "[control-loop]") {
  Fixture f;
  auto ops = f.fake->ops();
  SECTION("negative send leaves caller ownership") {
    ops.send = [](void *, ENetPeer *, uint8_t, ENetPacket *) { return -1; };
  }
  SECTION("throw after transfer forces stop and owner reset") {
    ops.send = [](void *p, ENetPeer *, uint8_t, ENetPacket *packet) -> int {
      auto &fake = *static_cast<Fake *>(p);
      fake.affinity();
      fake.pending.push_back(packet);
      throw std::runtime_error("transferred then throw");
    };
  }
  f.loop = f.domain.start(f.config, f.sessions, f.bus, std::move(ops));
  REQUIRE(f.loop);
  f.ready();
  auto peer = f.connect();
  auto before = f.fake->frees.load();
  REQUIRE(peer.sender->send("attempt"));
  REQUIRE(eventually([&] { return f.fake->frees > before; }));
  f.finish();
  REQUIRE(f.fake->destroys == 1);
  REQUIRE_FALSE(f.fake->bad_thread);
}

TEST_CASE("Owned control lifecycle dependency destruction follows actual join outside all loop locks",
          "[control-loop]") {
  auto gate = std::make_shared<Gate>();
  struct Open {
    std::shared_ptr<Gate> gate;
    ~Open() {
      gate->release();
    }
  };
  struct Payload {
    std::shared_ptr<Fake> fake;
    std::shared_ptr<Gate> gate;
    ~Payload() {
      gate->wait();
    }
  };
  Fixture f;
  auto domain = std::make_unique<ControlLoopDomain>();
  auto payload = std::make_shared<Payload>();
  payload->fake = f.fake;
  payload->gate = gate;
  auto ops = f.fake->ops();
  ops.owner = std::shared_ptr<void>(payload, f.fake.get());
  std::weak_ptr<Payload> weak = payload;
  payload.reset();
  auto owner = domain->start(f.config, f.sessions, f.bus, std::move(ops));
  REQUIRE(owner);
  REQUIRE(eventually([&] { return owner->status().ready; }));
  owner->request_stop();
  REQUIRE(eventually([&] { return owner->join_when_exited() == JoinResult::Joined; }));
  REQUIRE(f.fake->destroys == 1);
  REQUIRE_FALSE(weak.expired()); // immutable operations retained until domain release
  owner.reset();
  std::atomic_bool retired{false};
  std::jthread lifecycle([domain = std::move(domain), &retired]() mutable {
    domain.reset();
    retired = true;
  });
  Open open{gate}; // opens BEFORE jthread joins, also on failed assertion
  REQUIRE(eventually([&] { return gate->entered.load(); }));
  REQUIRE_FALSE(retired);
  gate->release();
  lifecycle.join();
  REQUIRE(retired);
  REQUIRE(weak.expired());
}
