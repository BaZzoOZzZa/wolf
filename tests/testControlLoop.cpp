#include <control/control_loop.hpp>

#include <catch2/catch_test_macros.hpp>
#include <control/control.hpp>
#include <deque>
#include <state/sessions.hpp>
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
    if (before)
      before->wait();
    if (s.marker)
      s.marker(p);
    if (after)
      after->wait();
  }
  void destroy_packet(ENetPacket *p) {
    affinity();
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
    if (after)
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
