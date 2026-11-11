#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <condition_variable>
#include <control/checked_xbox_backend.hpp>
#include <deque>
#include <future>
#include <linux/uinput.h>
#include <mutex>
#include <vector>

using namespace inputtino::checked;
using namespace std::chrono_literals;
namespace {
using Clock = std::chrono::steady_clock;
struct Gate {
  std::mutex mutex;
  std::condition_variable cv;
  bool entered = false, released = false;
  void wait() {
    std::unique_lock lock(mutex);
    entered = true;
    cv.notify_all();
    cv.wait(lock, [&] { return released; });
  }
  bool arrival() {
    std::unique_lock lock(mutex);
    return cv.wait_for(lock, 2s, [&] { return entered; });
  }
  void open() {
    std::lock_guard lock(mutex);
    released = true;
    cv.notify_all();
  }
};
struct Fake {
  Gate start, poll_gate, write_gate, callback_gate, destruction;
  std::atomic_bool block_poll{false}, block_write{false}, block_destruction{false};
  std::atomic_int reads{0}, polls{0}, ioctls{0}, callbacks{0};
  bool throw_poll = false, throw_read = false, throw_ioctl = false;
  IoResult poll_failure{0, 0}, read_failure{0, 0};
  bool fail_read = false, fail_ioctl = false;
  std::mutex events_mutex;
  std::deque<input_event> pending;
  std::vector<input_event> written;
  int fail_at = -1;
  IoResult failure{-1, EAGAIN};
  bool throw_write = false;
  int upload_retval = 123;
  uint16_t effect_type = FF_RUMBLE;
  void release() {
    start.open();
    poll_gate.open();
    write_gate.open();
    callback_gate.open();
    destruction.open();
  }
  void rumble() {
    std::lock_guard lock(events_mutex);
    input_event upload{}, play{};
    upload.type = EV_UINPUT;
    upload.code = UI_FF_UPLOAD;
    upload.value = 3;
    play.type = EV_FF;
    play.code = 1;
    play.value = 1;
    pending.push_back(upload);
    pending.push_back(play);
  }
  detail::Io ops() {
    return {[this](int fd, const input_event &ev, size_t size) {
              if (fd != 42 || size != sizeof(input_event))
                throw std::runtime_error("wrong retained fd/size");
              const auto index = written.size();
              written.push_back(ev);
              if (block_write.load())
                write_gate.wait();
              if (static_cast<int>(index) == fail_at) {
                if (throw_write)
                  throw std::runtime_error("injected write exception");
                return failure;
              }
              return IoResult{static_cast<std::ptrdiff_t>(size), 0};
            },
            [this](int fd, int timeout) {
              ++polls;
              if (fd != 42 || timeout != 10)
                throw std::runtime_error("wrong poll parameters");
              if (block_poll.load())
                poll_gate.wait();
              if (throw_poll)
                throw std::runtime_error("injected poll exception");
              if (poll_failure.count < 0)
                return poll_failure;
              std::this_thread::sleep_for(1ms);
              std::lock_guard lock(events_mutex);
              return IoResult{pending.empty() ? 0 : 1, 0};
            },
            [this](int fd, input_event &ev, size_t size) {
              ++reads;
              if (fd != 42 || size != sizeof(ev))
                throw std::runtime_error("wrong read parameters");
              if (throw_read)
                throw std::runtime_error("injected read exception");
              if (fail_read)
                return read_failure;
              std::lock_guard lock(events_mutex);
              if (pending.empty())
                return IoResult{-1, EAGAIN};
              ev = pending.front();
              pending.pop_front();
              return IoResult{static_cast<std::ptrdiff_t>(sizeof(ev)), 0};
            },
            [this](int fd, unsigned long request, void *arg) {
              ++ioctls;
              if (fd != 42)
                throw std::runtime_error("wrong ioctl fd");
              if (throw_ioctl)
                throw std::runtime_error("injected ioctl exception");
              if (fail_ioctl)
                return -1;
              if (request == UI_BEGIN_FF_UPLOAD) {
                auto &u = *static_cast<uinput_ff_upload *>(arg);
                u.effect.id = 1;
                u.effect.type = effect_type;
                u.effect.u.rumble.strong_magnitude = 1000;
                u.effect.u.rumble.weak_magnitude = 2000;
                u.effect.replay.length = 1000;
              } else if (request == UI_END_FF_UPLOAD)
                upload_retval = static_cast<uinput_ff_upload *>(arg)->retval;
              else
                throw std::runtime_error("unexpected test ioctl");
              return 0;
            },
            [] { return Clock::time_point{} + 10s; }};
  }
};
struct Resource {
  std::shared_ptr<Fake> fake;
  explicit Resource(std::shared_ptr<Fake> f) : fake(std::move(f)) {}
  ~Resource() {
    if (fake->block_destruction.load())
      fake->destruction.wait();
  }
};
struct Fixture {
  std::shared_ptr<Fake> fake = std::make_shared<Fake>();
  std::weak_ptr<Resource> resource;
  std::unique_ptr<Xbox> xbox;
  std::vector<std::thread> writers;
  void create(
      Xbox::Feedback feedback = [](int, int) {},
      detail::ThreadFactory start = [](auto fn) { return std::thread(std::move(fn)); }) {
    auto owner = std::make_shared<Resource>(fake);
    resource = owner;
    xbox = Xbox::adopt({std::move(owner), 42}, fake->ops(), std::move(feedback), std::move(start));
  }
  ~Fixture() {
    // Every assertion failure unwinds through this: no closed test gates, borrowed
    // callback state or joinable writer can survive an early REQUIRE failure.
    if (xbox)
      xbox->request_stop();
    fake->release();
    for (auto &writer : writers)
      if (writer.joinable())
        writer.join();
    if (xbox)
      xbox->retire_until(Clock::now() + 2s);
  }
};
bool wait_for(const std::function<bool()> &predicate) {
  const auto until = Clock::now() + 2s;
  while (!predicate() && Clock::now() < until)
    std::this_thread::sleep_for(1ms);
  return predicate();
}
} // namespace

TEST_CASE("checked Xbox actual full frames use exact retained fd and mappings", "[checked-xbox]") {
  Fixture f;
  f.create();
  auto generation = f.xbox->generation();
  auto r = f.xbox->neutral(generation);
  REQUIRE(r.outcome == WriteOutcome::KernelFrameAccepted);
  REQUIRE(r.accepted_events == 20);
  REQUIRE(r.attempted_events == 20);
  REQUIRE(r.sequence == 1);
  REQUIRE(f.fake->written.size() == 20);
  const std::array<int, 20> codes{BTN_START, BTN_SELECT, BTN_THUMBL, BTN_THUMBR, BTN_TL,    BTN_TR,    BTN_MODE,
                                  BTN_SOUTH, BTN_EAST,   BTN_NORTH,  BTN_WEST,   ABS_HAT0X, ABS_HAT0Y, ABS_X,
                                  ABS_Y,     ABS_RX,     ABS_RY,     ABS_Z,      ABS_RZ,    SYN_REPORT};
  for (size_t i = 0; i < codes.size(); ++i) {
    CHECK(f.fake->written[i].code == codes[i]);
    CHECK(f.fake->written[i].type == (i < 11 ? EV_KEY : (i == 19 ? EV_SYN : EV_ABS)));
    CHECK(f.fake->written[i].value == 0);
  }
  Frame extremes{0xf7ff, -32768, -32768, 32767, 32767, 255, 0};
  r = f.xbox->apply(generation, extremes);
  REQUIRE(r.outcome == WriteOutcome::KernelFrameAccepted);
  REQUIRE(r.sequence == 2);
  for (size_t i = 20; i < 31; ++i)
    CHECK(f.fake->written[i].value == 1);
  CHECK(f.fake->written[31].value == -1); // left/up take precedence over opposing directions
  CHECK(f.fake->written[32].value == -1);
  CHECK(f.fake->written[33].value == -32768);
  CHECK(f.fake->written[34].value == 32767); // no -INT16_MIN wrap
  CHECK(f.fake->written[35].value == 32767);
  CHECK(f.fake->written[36].value == -32767);
  CHECK(f.fake->written[37].value == 255);
  CHECK(f.fake->written[38].value == 0);
  CHECK(f.xbox->last_confirmed()->ly == -32768);
  REQUIRE(f.xbox->neutral(generation).outcome == WriteOutcome::KernelFrameAccepted);
  REQUIRE(f.xbox->neutral(generation).outcome == WriteOutcome::KernelFrameAccepted);
  CHECK(f.fake->written.size() == 80); // force all, even cached zero
  Frame directions{inputtino::Joypad::DPAD_RIGHT | inputtino::Joypad::DPAD_DOWN};
  REQUIRE(f.xbox->apply(generation, directions).outcome == WriteOutcome::KernelFrameAccepted);
  CHECK(f.fake->written[91].value == 1);
  CHECK(f.fake->written[92].value == 1);
}

TEST_CASE("checked Xbox every event failure prevents commit including SYN", "[checked-xbox]") {
  for (int mode = 0; mode < 8; ++mode) {
    for (int index = 0; index < 20; ++index) {
      CAPTURE(mode, index);
      Fixture f;
      f.create();
      Frame held{inputtino::Joypad::A, 123};
      REQUIRE(f.xbox->apply(f.xbox->generation(), held).outcome == WriteOutcome::KernelFrameAccepted);
      f.fake->written.clear();
      f.fake->fail_at = index;
      const std::array<IoResult, 8> failures{{{-1, EAGAIN},
                                              {-1, EINTR},
                                              {-1, EIO},
                                              {0, 0},
                                              {1, 0},
                                              {sizeof(input_event) - 1, 0},
                                              {sizeof(input_event) + 1, 0},
                                              {-1, 0}}};
      f.fake->failure = failures[mode];
      f.fake->throw_write = mode == 7;
      auto r = f.xbox->neutral(f.xbox->generation());
      CHECK(r.outcome != WriteOutcome::KernelFrameAccepted);
      CHECK(r.reason == (mode == 7 ? WriteError::Exception : (mode < 3 ? WriteError::Io : WriteError::ShortWrite)));
      CHECK(r.error == (mode < 3 ? failures[mode].error : 0));
      CHECK(r.accepted_events == static_cast<size_t>(index));
      CHECK(r.attempted_events == static_cast<size_t>(index + 1));
      CHECK(f.fake->written.size() == static_cast<size_t>(index + 1)); // no remaining-byte retry
      CHECK(r.sequence == 1);
      CHECK(f.xbox->last_confirmed()->buttons == held.buttons);
      CHECK(f.xbox->last_confirmed()->lx == 123);
      REQUIRE(f.xbox->apply(f.xbox->generation(), {}).reason == WriteError::Poisoned);
      f.fake->fail_at = -1;
      f.fake->written.clear();
      r = f.xbox->neutral(f.xbox->generation()); // explicit attempt, never owner grant
      REQUIRE(r.outcome == WriteOutcome::KernelFrameAccepted);
      CHECK(r.sequence == 2);
      CHECK(r.accepted_events == 20);
      CHECK(f.xbox->last_confirmed()->buttons == 0);
      CHECK(f.xbox->apply(f.xbox->generation(), held).reason == WriteError::Poisoned);
    }
  }
}

TEST_CASE("checked Xbox null stale same-fd generation and stopped writes fail closed", "[checked-xbox]") {
  Fixture first, second;
  first.create();
  second.create();
  const auto old = first.xbox->generation();
  CHECK(second.xbox->neutral(old).reason == WriteError::Generation);
  CHECK(second.xbox->neutral({}).reason == WriteError::Generation);
  CHECK(second.fake->written.empty());
  CHECK(second.xbox->apply(second.xbox->generation(), Frame{0x10000}).reason == WriteError::Buttons);
  CHECK(second.fake->written.empty());
  REQUIRE(first.xbox->retire_until(Clock::now() + 2s) == Retirement::Retired);
  CHECK(first.resource.expired());
  CHECK(first.xbox->neutral(old).reason == WriteError::Stopped);
  CHECK_FALSE(old.matches(second.xbox->generation()));
  control::CheckedXboxBackend adapter(std::move(second.xbox));
  CHECK(adapter.neutral(old).reason == WriteError::Generation);
  CHECK(adapter.neutral(adapter.generation()).outcome == WriteOutcome::KernelFrameAccepted);
  adapter.request_stop();
  CHECK(adapter.apply(adapter.generation(), {}).reason == WriteError::Stopped);
  REQUIRE(adapter.retire_until(Clock::now() + 2s) == Retirement::Retired);
  CHECK(second.resource.expired());
  REQUIRE_THROWS_AS(control::CheckedXboxBackend(nullptr), std::invalid_argument);
}

TEST_CASE("checked Xbox unpublished factory failures release all captures", "[checked-xbox]") {
  Fixture f;
  auto payload = std::make_shared<int>(7);
  std::weak_ptr<int> weak = payload;
  REQUIRE_THROWS(f.create([hold = std::move(payload)](int, int) {},
                          [](auto) -> std::thread {
                            throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
                          }));
  CHECK(f.resource.expired());
  CHECK(weak.expired());
  CHECK(f.fake->polls == 0);
  REQUIRE_THROWS(f.create([](int, int) {}, [](auto) { return std::thread{}; }));
  CHECK(f.resource.expired());
  auto start = [](auto fn) { return std::thread(std::move(fn)); };
  REQUIRE_THROWS_AS(Xbox::adopt({{}, 42}, f.fake->ops(), [](int, int) {}, start), std::invalid_argument);
  auto owner = std::make_shared<int>(0);
  REQUIRE_THROWS_AS(Xbox::adopt({owner, -1}, f.fake->ops(), [](int, int) {}, start), std::invalid_argument);
  auto incomplete = f.fake->ops();
  incomplete.read = {};
  REQUIRE_THROWS_AS(Xbox::adopt({owner, 42}, incomplete, [](int, int) {}, start), std::invalid_argument);
  CHECK(f.fake->polls == 0);
}

TEST_CASE("checked Xbox immutable callback is owned before listener start", "[checked-xbox]") {
  Fixture f;
  auto payload = std::make_shared<int>(7);
  std::weak_ptr<int> weak = payload;
  f.fake->rumble();
  f.create(
      [hold = std::move(payload), fake = f.fake](int low, int high) {
        if (low != 1000 || high != 2000 || *hold != 7)
          throw std::runtime_error("feedback mismatch");
        ++fake->callbacks;
      },
      [fake = f.fake](auto work) {
        return std::thread([fake, work = std::move(work)] {
          fake->start.wait();
          work();
        });
      });
  REQUIRE(f.fake->start.arrival());
  CHECK_FALSE(weak.expired());
  CHECK(f.fake->polls == 0);
  f.fake->start.open();
  REQUIRE(wait_for([&] { return f.fake->callbacks == 1; }));
  CHECK_FALSE(f.xbox->listener_status().failed);
  REQUIRE(f.xbox->retire_until(Clock::now() + 2s) == Retirement::Retired);
  CHECK(f.resource.expired());
  CHECK(weak.expired());
  CHECK(f.fake->upload_retval == 0);
}

TEST_CASE("checked Xbox in-flight callback retains resource until actual retirement", "[checked-xbox]") {
  Fixture f;
  auto payload = std::make_shared<int>(7);
  std::weak_ptr<int> weak = payload;
  f.fake->rumble();
  f.create([hold = std::move(payload), fake = f.fake](int, int) {
    ++fake->callbacks;
    fake->callback_gate.wait();
  });
  REQUIRE(f.fake->callback_gate.arrival());
  f.xbox->request_stop();
  CHECK(f.xbox->retire_until(Clock::now() + 2ms) == Retirement::Unknown);
  CHECK_FALSE(weak.expired());
  CHECK_FALSE(f.resource.expired());
  CHECK(f.xbox->apply(f.xbox->generation(), {}).reason == WriteError::Stopped);
  f.fake->callback_gate.open();
  REQUIRE(f.xbox->retire_until(Clock::now() + 2s) == Retirement::Retired);
  CHECK(weak.expired());
  CHECK(f.resource.expired());
  CHECK(f.fake->callbacks == 1);
}

TEST_CASE("checked Xbox stop during poll cannot publish buffered feedback", "[checked-xbox]") {
  Fixture f;
  f.fake->block_poll = true;
  f.fake->rumble();
  f.create([fake = f.fake](int, int) { ++fake->callbacks; });
  REQUIRE(f.fake->poll_gate.arrival());
  CHECK(f.xbox->retire_until(Clock::now() + 2ms) == Retirement::Unknown);
  CHECK_FALSE(f.resource.expired());
  f.fake->poll_gate.open();
  REQUIRE(f.xbox->retire_until(Clock::now() + 2s) == Retirement::Retired);
  CHECK(f.fake->callbacks == 0);
  CHECK(f.fake->reads == 0);
  CHECK(f.resource.expired());
}

TEST_CASE("checked Xbox write and resource destruction are completion work", "[checked-xbox]") {
  Fixture f;
  f.create();
  SECTION("in-flight write") {
    f.fake->block_write = true;
    std::promise<WriteReceipt> promise;
    auto result = promise.get_future();
    // Promise is heap-owned by writer, so REQUIRE failure cannot invalidate it.
    auto owned = std::make_shared<std::promise<WriteReceipt>>(std::move(promise));
    f.writers.emplace_back([&f, owned] { owned->set_value(f.xbox->neutral(f.xbox->generation())); });
    REQUIRE(f.fake->write_gate.arrival());
    CHECK(f.xbox->retire_until(Clock::now() + 2ms) == Retirement::Unknown);
    CHECK_FALSE(f.resource.expired());
    f.fake->write_gate.open();
    REQUIRE(result.wait_for(2s) == std::future_status::ready);
    CHECK(result.get().outcome == WriteOutcome::KernelFrameAccepted); // reserved work may finish
  }
  SECTION("device release blocked") {
    f.fake->block_destruction = true;
    f.xbox->request_stop();
    REQUIRE(f.fake->destruction.arrival());
    CHECK(f.xbox->retire_until(Clock::now() + 2ms) == Retirement::Unknown);
    // weak_ptr may expire at destructor entry; only join is release evidence.
    f.fake->destruction.open();
  }
  REQUIRE(f.xbox->retire_until(Clock::now() + 2s) == Retirement::Retired);
  CHECK(f.resource.expired());
}

TEST_CASE("checked Xbox listener errors release resources and stay failed", "[checked-xbox]") {
  Fixture f;
  SECTION("poll error") {
    f.fake->poll_failure = {-1, EIO};
  }
  SECTION("poll exception") {
    f.fake->throw_poll = true;
  }
  SECTION("read zero") {
    f.fake->rumble();
    f.fake->fail_read = true;
    f.fake->read_failure = {0, 0};
  }
  SECTION("read short") {
    f.fake->rumble();
    f.fake->fail_read = true;
    f.fake->read_failure = {1, 0};
  }
  SECTION("read error") {
    f.fake->rumble();
    f.fake->fail_read = true;
    f.fake->read_failure = {-1, EIO};
  }
  SECTION("read exception") {
    f.fake->rumble();
    f.fake->throw_read = true;
  }
  SECTION("ioctl error") {
    f.fake->rumble();
    f.fake->fail_ioctl = true;
  }
  SECTION("ioctl exception") {
    f.fake->rumble();
    f.fake->throw_ioctl = true;
  }
  SECTION("callback exception") {
    f.fake->rumble();
  }
  f.create([](int, int) { throw std::runtime_error("injected feedback exception"); });
  REQUIRE(wait_for([&] { return f.xbox->listener_status().failed; }));
  REQUIRE(f.xbox->retire_until(Clock::now() + 2s) == Retirement::Retired);
  CHECK(f.resource.expired());
  CHECK(f.xbox->listener_status().failed);
  CHECK(f.xbox->neutral(f.xbox->generation()).reason == WriteError::Stopped);
}

TEST_CASE("checked Xbox owner-thread retirement and unsupported effects are explicit", "[checked-xbox]") {
  Fixture f;
  f.fake->effect_type = FF_CONSTANT;
  f.fake->rumble();
  f.create([fake = f.fake](int, int) { ++fake->callbacks; });
  auto wrong = std::async(std::launch::async, [&] { return f.xbox->retire_until(Clock::now() + 1s); });
  CHECK(wrong.get() == Retirement::WrongThread);
  CHECK_FALSE(f.xbox->listener_status().stop_requested);
  REQUIRE(wait_for([&] { return f.fake->reads >= 2; }));
  REQUIRE(f.xbox->retire_until(Clock::now() + 2s) == Retirement::Retired);
  CHECK(f.fake->upload_retval == -EINVAL);
  CHECK(f.fake->callbacks == 0);
  CHECK_FALSE(f.xbox->listener_status().failed);
}

TEST_CASE("checked Xbox stop before worker entry and allocation failure retain no publication", "[checked-xbox]") {
  Fixture f;
  SECTION("stop before entry") {
    f.create([](int, int) {},
             [fake = f.fake](auto work) {
               return std::thread([fake, work = std::move(work)] {
                 fake->start.wait();
                 work();
               });
             });
    REQUIRE(f.fake->start.arrival());
    CHECK(f.xbox->retire_until(Clock::now() + 2ms) == Retirement::Unknown);
    CHECK_FALSE(f.resource.expired());
    f.fake->start.open();
    REQUIRE(f.xbox->retire_until(Clock::now() + 2s) == Retirement::Retired);
    CHECK(f.fake->polls == 0);
    CHECK(f.resource.expired());
  }
  SECTION("allocation exception at thread factory") {
    auto payload = std::make_shared<int>(7);
    std::weak_ptr<int> weak = payload;
    REQUIRE_THROWS_AS(
        f.create([hold = std::move(payload)](int, int) {}, [](auto) -> std::thread { throw std::bad_alloc(); }),
        std::bad_alloc);
    CHECK(weak.expired());
    CHECK(f.resource.expired());
    CHECK(f.fake->polls == 0);
  }
}

TEST_CASE("checked Xbox individual wire buttons keep pinned Xbox mapping", "[checked-xbox]") {
  Fixture f;
  f.create();
  // Independent protocol golden pairs: all-bits-set alone cannot catch swapped buttons.
  const std::array<std::pair<uint32_t, uint16_t>, 11> golden{{{0x0010, BTN_START},
                                                              {0x0020, BTN_SELECT},
                                                              {0x0040, BTN_THUMBL},
                                                              {0x0080, BTN_THUMBR},
                                                              {0x0100, BTN_TL},
                                                              {0x0200, BTN_TR},
                                                              {0x0400, BTN_MODE},
                                                              {0x1000, BTN_SOUTH},
                                                              {0x2000, BTN_EAST},
                                                              {0x4000, BTN_NORTH},
                                                              {0x8000, BTN_WEST}}};
  for (auto [wire, expected] : golden) {
    CAPTURE(wire, expected);
    f.fake->written.clear();
    REQUIRE(f.xbox->apply(f.xbox->generation(), Frame{wire}).outcome == WriteOutcome::KernelFrameAccepted);
    size_t pressed = 0;
    for (const auto &event : f.fake->written) {
      if (event.type == EV_KEY) {
        CHECK(event.value == (event.code == expected ? 1 : 0));
        pressed += event.value;
      }
    }
    CHECK(pressed == 1);
  }
}

TEST_CASE("checked Xbox transient feedback IO remains cancellable", "[checked-xbox]") {
  Fixture f;
  f.fake->rumble();
  SECTION("poll EINTR") {
    f.fake->poll_failure = {-1, EINTR};
  }
  SECTION("read EAGAIN") {
    f.fake->fail_read = true;
    f.fake->read_failure = {-1, EAGAIN};
  }
  SECTION("read EINTR") {
    f.fake->fail_read = true;
    f.fake->read_failure = {-1, EINTR};
  }
  f.create([fake = f.fake](int, int) { ++fake->callbacks; });
  REQUIRE(wait_for([&] { return f.fake->fail_read ? f.fake->reads >= 2 : f.fake->polls >= 2; }));
  REQUIRE(f.xbox->retire_until(Clock::now() + 2s) == Retirement::Retired);
  CHECK_FALSE(f.xbox->listener_status().failed);
  CHECK(f.resource.expired());
  CHECK(f.fake->callbacks == 0);
}
