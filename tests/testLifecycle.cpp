// Deliberately independent of Catch2, Wolf runtime, Docker, GStreamer and devices.
#include <core/docker.hpp>
#include <sessions/lifecycle.hpp>

#include <atomic>
#include <barrier>
#include <iostream>
#include <stdexcept>
#include <thread>

using wolf::core::sessions::LobbyLifecycle;
using namespace wolf::core::docker;

namespace {
void check(bool condition, const char *expression, int line) {
  if (!condition) {
    throw std::runtime_error(std::string(expression) + " at line " + std::to_string(line));
  }
}
#define CHECK(...) check((__VA_ARGS__), #__VA_ARGS__, __LINE__)

LobbyLifecycle::Ticket ready(LobbyLifecycle &gate, const std::string &id, const std::string &resource) {
  auto admission = gate.create(id, resource, "request");
  CHECK(admission.result == LobbyLifecycle::Create::admitted);
  CHECK(gate.launch(admission.ticket));
  CHECK(gate.ready(admission.ticket));
  return admission.ticket;
}

void inspect_requires_evidence() {
  CHECK(!InspectResult{}.terminal());
  CHECK(!InspectResult{InspectState::present, {}}.terminal());
  CHECK(InspectResult{InspectState::missing, {}}.terminal());
  for (auto status : {CREATED, RUNNING, PAUSED, RESTARTING, REMOVING, UNKNOWN}) {
    CHECK(!InspectResult{InspectState::present, Container{.status = status}}.terminal());
  }
  CHECK(InspectResult{InspectState::present, Container{.status = EXITED}}.terminal());
  CHECK(InspectResult{InspectState::present, Container{.status = DEAD}}.terminal());
  CHECK(!InspectResult{InspectState::unknown, Container{.status = EXITED}}.terminal());
}

void uncertain_start_and_bounded_reconciliation() {
  using Decision = ContainerMonitor::Decision;
  const auto now = ContainerMonitor::Clock::time_point{};
  auto exited = InspectResult{InspectState::present, Container{.status = EXITED}};
  auto running = InspectResult{InspectState::present, Container{.status = RUNNING}};
  ContainerMonitor uncertain_start(false);
  CHECK(uncertain_start.observe(exited, now) == Decision::reconcile);
  // The daemon can still start after the first Exited reply. Do not release.
  CHECK(uncertain_start.observe(running, now + std::chrono::seconds(1)) == Decision::live);
  CHECK(uncertain_start.observe(exited, now + std::chrono::seconds(2)) == Decision::terminal);

  ContainerMonitor lost(false);
  CHECK(lost.observe(exited, now) == Decision::reconcile);
  CHECK(lost.observe(exited, now + std::chrono::seconds(29)) == Decision::reconcile);
  CHECK(lost.observe(exited, now + std::chrono::seconds(30)) == Decision::quarantine);

  ContainerMonitor known(true);
  CHECK(known.observe({}, now) == Decision::reconcile);
  CHECK(known.observe({}, now + std::chrono::seconds(29)) == Decision::reconcile);
  CHECK(known.observe(running, now + std::chrono::seconds(30)) == Decision::live);
  CHECK(known.observe({}, now + std::chrono::seconds(60)) == Decision::reconcile);
  CHECK(known.observe({}, now + std::chrono::seconds(90)) == Decision::quarantine);
  CHECK(known.observe({InspectState::missing, {}}, now + std::chrono::seconds(91)) == Decision::terminal);
}

void retry_and_uncertainty() {
  LobbyLifecycle gate;
  auto first = gate.create("request-1", "prefix", "payload");
  CHECK(first.result == LobbyLifecycle::Create::admitted);
  // A timed-out HTTP waiter does not end the operation. Both keyed and legacy
  // retries must compete with the same reservation before any callback runs.
  CHECK(gate.create("request-1", "prefix", "payload").result == LobbyLifecycle::Create::pending);
  CHECK(gate.create("request-2", "prefix", "payload").result == LobbyLifecycle::Create::conflict);
  CHECK(gate.create("request-1", "prefix", "changed").result == LobbyLifecycle::Create::conflict);
  CHECK(gate.launch(first.ticket));
  CHECK(!gate.launch(first.ticket));
  CHECK(gate.ready(first.ticket));
  CHECK(gate.create("request-1", "prefix", "payload").result == LobbyLifecycle::Create::replay_ready);
  gate.quarantine(first.ticket);
  CHECK(gate.join(first.ticket, 1, true) == LobbyLifecycle::Join::unavailable);
  CHECK(gate.create("request-1", "prefix", "payload").result == LobbyLifecycle::Create::conflict);
  CHECK(gate.stop(first.ticket));
  gate.stopped(first.ticket);
  gate.producer_finished(first.ticket);
  // Worker returned Unknown: stopping media alone cannot release game admission.
  CHECK(gate.create("request-2", "prefix", "payload").result == LobbyLifecycle::Create::conflict);
  gate.runner_finished(first.ticket); // Only a confirmed terminal runner permits this.
  auto next = gate.create("request-2", "prefix", "payload");
  CHECK(next.result == LobbyLifecycle::Create::admitted);
  CHECK(gate.create("request-1", "prefix", "payload").result == LobbyLifecycle::Create::conflict);
  // Delayed completions on an old ticket must not release its successor's slot.
  gate.producer_finished(first.ticket);
  gate.runner_finished(first.ticket);
  gate.stopped(first.ticket);
  CHECK(gate.create("request-3", "prefix", "payload").result == LobbyLifecycle::Create::conflict);
}

void stop_before_ready() {
  LobbyLifecycle gate;
  auto first = gate.create("old", "prefix", "payload").ticket;
  CHECK(gate.stop(first));
  gate.retry_stop(first); // Stop preceded public lobby publication.
  CHECK(gate.stop(first));
  gate.stopped(first);
  CHECK(!gate.launch(first)); // Late callback can never start the runner.
  CHECK(!gate.ready(first));
  CHECK(gate.create("next", "prefix", "payload").result == LobbyLifecycle::Create::conflict);
  gate.producer_finished(first);
  CHECK(gate.create("next", "prefix", "payload").result == LobbyLifecycle::Create::admitted);
}

void concurrent_creates() {
  LobbyLifecycle gate;
  constexpr int workers = 16;
  std::barrier start(workers);
  std::atomic_int admitted = 0;
  std::vector<std::thread> threads;
  for (int i = 0; i < workers; ++i) {
    threads.emplace_back([&, i] {
      start.arrive_and_wait();
      if (gate.create(std::to_string(i), "one-prefix", "payload").result == LobbyLifecycle::Create::admitted) {
        ++admitted;
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }
  CHECK(admitted == 1);
}

void concurrent_joins() {
  LobbyLifecycle gate;
  auto entry = ready(gate, "lobby", "prefix");
  constexpr int workers = 16;
  std::barrier start(workers);
  std::atomic_int admitted = 0;
  std::atomic_size_t winner = 0;
  std::vector<std::thread> threads;
  for (std::size_t i = 1; i <= workers; ++i) {
    threads.emplace_back([&, i] {
      start.arrive_and_wait();
      if (gate.join(entry, i, false) == LobbyLifecycle::Join::admitted) {
        ++admitted;
        winner = i;
        gate.finish(entry); // The side effects complete after admission.
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }
  CHECK(admitted == 1);
  CHECK(gate.join(entry, winner, false) == LobbyLifecycle::Join::already_joined);
  auto other = ready(gate, "other", "other-prefix");
  CHECK(gate.join(other, winner, true) == LobbyLifecycle::Join::session_busy);
  CHECK(gate.leave(entry, winner));
  gate.finish(entry, winner);
  CHECK(gate.join(other, winner, true) == LobbyLifecycle::Join::admitted);
}

void stop_during_join_and_reentrant_leave() {
  LobbyLifecycle gate;
  auto entry = ready(gate, "lobby", "prefix");
  CHECK(gate.join(entry, 42, false) == LobbyLifecycle::Join::admitted);
  // These represent synchronous events re-entering while device migration runs.
  CHECK(!gate.leave(entry, 42));
  CHECK(!gate.stop(entry));
  CHECK(gate.join(entry, 43, false) == LobbyLifecycle::Join::unavailable);
  auto deferred = gate.finish(entry);
  CHECK(deferred.stop);
  CHECK(deferred.leaves == std::vector<std::size_t>{42});
  CHECK(gate.leave(entry, 42));
  CHECK(!gate.stop(entry));
  CHECK(gate.finish(entry, 42).stop);
  CHECK(gate.stop(entry));
  CHECK(!gate.stop(entry)); // Stop -> leave -> Stop recursion cannot clean twice.
  gate.stopped(entry);
  gate.runner_finished(entry);
  CHECK(gate.create("new", "prefix", "request").result == LobbyLifecycle::Create::conflict);
  gate.producer_finished(entry);
  CHECK(gate.create("new", "prefix", "request").result == LobbyLifecycle::Create::admitted);
}

void launch_stop_race() {
  for (int i = 0; i < 100; ++i) {
    LobbyLifecycle gate;
    auto entry = gate.create("lobby", "prefix", "request").ticket;
    std::barrier start(2);
    std::thread launch([&] {
      start.arrive_and_wait();
      if (gate.launch(entry)) {
        gate.ready(entry);
      }
    });
    start.arrive_and_wait();
    CHECK(gate.stop(entry));
    launch.join();
    CHECK(!gate.launch(entry));
    CHECK(!gate.ready(entry));
    CHECK(gate.join(entry, 1, false) == LobbyLifecycle::Join::unavailable);
  }
}

void all_producers_must_finish() {
  LobbyLifecycle gate;
  auto entry = gate.create("a", "prefix", "request").ticket;
  CHECK(gate.add_audio_producer(entry));
  CHECK(!gate.add_audio_producer(entry));
  gate.producer_finished(entry);
  CHECK(!gate.launch(entry));
  CHECK(gate.stop(entry));
  gate.stopped(entry);
  CHECK(!gate.add_audio_producer(entry));
  gate.producer_finished(entry);
  gate.producer_finished(entry); // A duplicated video callback cannot retire audio.
  CHECK(!gate.launch(entry));
  CHECK(gate.create("b", "prefix", "request").result == LobbyLifecycle::Create::conflict);
  gate.producer_finished(entry, LobbyLifecycle::Producer::audio);
  CHECK(gate.create("b", "prefix", "request").result == LobbyLifecycle::Create::admitted);
}

void bounded_history() {
  LobbyLifecycle gate(2);
  for (auto id : {"a", "b"}) {
    auto entry = gate.create(id, "prefix", "request").ticket;
    CHECK(entry != nullptr);
    CHECK(gate.stop(entry));
    gate.stopped(entry);
    gate.producer_finished(entry);
  }
  CHECK(gate.create("c", "prefix", "request").result == LobbyLifecycle::Create::exhausted);
  CHECK(gate.create("a", "prefix", "request").result == LobbyLifecycle::Create::conflict);
  LobbyLifecycle fresh;
  CHECK(fresh.create(std::string(129, 'k'), "prefix", "request").result == LobbyLifecycle::Create::exhausted);
  CHECK(fresh.create("key", std::string(4097, 'p'), "request").result == LobbyLifecycle::Create::exhausted);
  CHECK(fresh.create("key", "prefix", std::string(65537, 'r')).result == LobbyLifecycle::Create::exhausted);
}
} // namespace

int main() {
  try {
    inspect_requires_evidence();
    uncertain_start_and_bounded_reconciliation();
    retry_and_uncertainty();
    stop_before_ready();
    concurrent_creates();
    concurrent_joins();
    stop_during_join_and_reentrant_leave();
    launch_stop_race();
    bounded_history();
    all_producers_must_finish();
    std::cout << "10 lifecycle scenarios passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
