#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <future>
#include <streaming/consumer_events.hpp>
#include <streaming/consumer_pipeline.hpp>

using namespace streaming;
using namespace wolf::core;
using namespace std::chrono_literals;

namespace {
bool eventually(const std::function<bool()> &test) {
  auto until = std::chrono::steady_clock::now() + 2s;
  do {
    if (test())
      return true;
    std::this_thread::sleep_for(1ms);
  } while (std::chrono::steady_clock::now() < until);
  return test();
}
struct Gate {
  std::mutex mutex;
  std::condition_variable wake;
  bool open = false;
  std::atomic_bool entered{false};
  void wait() {
    entered = true;
    std::unique_lock lock(mutex);
    // Test failure must never leave a deliberately blocked worker forever.
    wake.wait_for(lock, 4s, [&] { return open; });
  }
  void release() {
    std::lock_guard lock(mutex);
    open = true;
    wake.notify_all();
  }
  ~Gate() {
    release();
  }
};
struct Current {
  std::mutex mutex;
  std::vector<ConnectionScope> scopes;
  bool contains(const ConnectionScope &scope) {
    std::lock_guard lock(mutex);
    return std::any_of(scopes.begin(), scopes.end(), [&](const auto &s) { return s.matches(scope); });
  }
  void set(std::vector<ConnectionScope> value) {
    std::lock_guard lock(mutex);
    scopes = std::move(value);
  }
};
struct Fixture {
  ConnectionScope scope = ConnectionScope::fresh(42);
  RunScope run = RunScope::fresh("42");
  std::shared_ptr<Current> current = std::make_shared<Current>();
  std::shared_ptr<events::EventBusType> bus = std::make_shared<events::EventBusType>();
  std::shared_ptr<ConsumerRuntime> runtime;
  immer::vector<immer::box<events::EventBusHandlers>> handlers;
  explicit Fixture(ConsumerPolicy policy = {}) {
    current->set({scope});
    runtime =
        std::make_shared<ConsumerRuntime>([current = current](const auto &s) { return current->contains(s); }, policy);
    handlers = consumer_event_handlers(bus, runtime);
  }
  ~Fixture() {
    // Last registration box unregisters before the owner joins.
    handlers = {};
    runtime.reset();
  }
  template <class Event> void fire(Event event) {
    bus->fire_event(immer::box<Event>{std::move(event)});
  }
  ConsumerAdmission admit(ConsumerKind kind, ConsumerRuntime::Work work) {
    events::VideoSession request{.session_id = 42,
                                 .port = 1234,
                                 .client_ip = "127.0.0.1",
                                 .rtp_secret_payload = {},
                                 .connection_scope = scope,
                                 .run_scope = run};
    return admit_consumer(*runtime, request, kind, std::move(work));
  }
  void ping(ConsumerKind kind) {
    if (kind == ConsumerKind::Video)
      fire(events::RTPVideoPingEvent{.client_ip = "127.0.0.1", .client_port = 1234, .payload = std::array<char, 16>{}});
    else
      fire(events::RTPAudioPingEvent{.client_ip = "127.0.0.1", .client_port = 1234, .payload = std::array<char, 16>{}});
  }
  bool joined(ConsumerKind kind) {
    auto status = runtime->status(scope, kind);
    return status && status->joined;
  }
};
ConsumerPolicy fast_policy() {
  ConsumerPolicy policy;
  policy.ping_timeout = 40ms;
  policy.drain_timeout = 30ms;
  policy.poll_interval = 2ms;
  return policy;
}
void software_gst() {
  static std::once_flag once;
  std::call_once(once, [] {
    // No registry scan/plugin scanner and no GPU/VAAPI/Pulse/Wayland plugin
    // initialization. Only the installed software coreelements plugin is loaded.
    g_setenv("GST_PLUGIN_SYSTEM_PATH_1_0", "", TRUE);
    g_setenv("GST_PLUGIN_PATH_1_0", "", TRUE);
    g_setenv("GST_REGISTRY_UPDATE", "no", TRUE);
    REQUIRE(gst_init_check(nullptr, nullptr, nullptr)); // NOT streaming::init()
    auto core = gst_plugin_load_file(WOLF_TEST_GST_CORE_PLUGIN, nullptr);
    REQUIRE(core != nullptr);
    gst_object_unref(core);
    auto plugins = gst_registry_get_plugin_list(gst_registry_get());
    for (auto item = plugins; item; item = item->next) {
      auto plugin = GST_PLUGIN(item->data);
      if (gst_plugin_is_loaded(plugin)) {
        const std::string name = gst_plugin_get_name(plugin);
        CHECK((name == "coreelements" || name == "staticelements"));
      }
    }
    gst_plugin_list_free(plugins);
  });
}
} // namespace

TEST_CASE("Consumer stop-before-registration and cancel-before-ping use real bus adapters", "[consumer]") {
  Fixture f;
  std::atomic_int starts{0};
  std::atomic_int game_stops{0};
  auto game = f.bus->register_handler<immer::box<events::GameStopRequest>>(
      [&](const immer::box<events::GameStopRequest> &) { ++game_stops; });
  auto ended = f.bus->register_handler<immer::box<events::RunEnded>>(
      [&](const immer::box<events::RunEnded> &) { ++game_stops; });
  auto work = [&](ConsumerJob &, const ConsumerPing &) {
    ++starts;
    return true;
  };
  SECTION("stop before either kind registers") {
    f.fire(events::StopStreamEvent{.session_id = 42, .connection_scope = f.scope});
    REQUIRE(f.admit(ConsumerKind::Video, work) == ConsumerAdmission::Revoked);
    REQUIRE(f.admit(ConsumerKind::Audio, work) == ConsumerAdmission::Revoked);
    auto ticket = f.runtime->revoke(f.scope);
    REQUIRE(ticket);
    REQUIRE(ticket->complete());
  }
  SECTION("pause while both wait") {
    REQUIRE(f.admit(ConsumerKind::Video, work) == ConsumerAdmission::Accepted);
    REQUIRE(f.admit(ConsumerKind::Audio, work) == ConsumerAdmission::Accepted);
    f.fire(events::PauseStreamEvent{.session_id = 42, .connection_scope = f.scope});
    f.ping(ConsumerKind::Video);
    f.ping(ConsumerKind::Audio);
    REQUIRE(eventually([&] { return f.joined(ConsumerKind::Video) && f.joined(ConsumerKind::Audio); }));
  }
  REQUIRE(starts == 0);
  REQUIRE(game_stops == 0); // no persistent-run stop/completion emitted
  REQUIRE(f.current->contains(f.scope));
  game.unregister();
  ended.unregister();
}

TEST_CASE("Consumer timeout ignores late ping and duplicate ANNOUNCE", "[consumer]") {
  Fixture f(fast_policy());
  std::atomic_int starts{0};
  REQUIRE(f.admit(ConsumerKind::Audio, [&](auto &, auto &) {
    ++starts;
    return true;
  }) == ConsumerAdmission::Accepted);
  REQUIRE(eventually([&] { return f.joined(ConsumerKind::Audio); }));
  auto status = f.runtime->status(f.scope, ConsumerKind::Audio);
  REQUIRE(status->state == ConsumerState::Stopped);
  REQUIRE(status->reason == ConsumerReason::PingTimeout);
  f.ping(ConsumerKind::Audio);
  REQUIRE(f.admit(ConsumerKind::Audio, [&](auto &, auto &) {
    ++starts;
    return true;
  }) == ConsumerAdmission::Duplicate);
  REQUIRE(starts == 0);
}

TEST_CASE("Concurrent duplicate admissions reserve exactly one waiter per kind", "[consumer]") {
  Gate gate;
  std::atomic_int accepted{0}, duplicate{0}, starts{0};
  Fixture f;
  std::vector<std::thread> callers;
  for (int n = 0; n < 4; ++n)
    callers.emplace_back([&] {
      auto result = f.admit(ConsumerKind::Video, [&](auto &, auto &) {
        ++starts;
        gate.wait();
        return true;
      });
      accepted += result == ConsumerAdmission::Accepted;
      duplicate += result == ConsumerAdmission::Duplicate;
    });
  for (auto &caller : callers)
    caller.join();
  REQUIRE(accepted == 1);
  REQUIRE(duplicate == 3);
  f.ping(ConsumerKind::Video);
  f.ping(ConsumerKind::Video);
  REQUIRE(eventually([&] { return gate.entered.load(); }));
  REQUIRE(starts == 1);
  gate.release();
  REQUIRE(eventually([&] { return f.joined(ConsumerKind::Video); }));
}

TEST_CASE("Stale scopes and reused numeric IDs cannot activate or revoke replacements", "[consumer]") {
  Fixture f;
  std::atomic_int starts{0};
  auto old = f.scope;
  REQUIRE(f.admit(ConsumerKind::Video, [&](auto &, auto &) {
    ++starts;
    return true;
  }) == ConsumerAdmission::Accepted);
  auto replacement = ConnectionScope::fresh(42);
  f.current->set({replacement});
  f.ping(ConsumerKind::Video); // old correlator never authorizes an old job
  REQUIRE(eventually([&] { return f.runtime->status(old, ConsumerKind::Video)->joined; }));
  REQUIRE(starts == 0);
  REQUIRE(f.admit(ConsumerKind::Audio, [](auto &, auto &) { return true; }) == ConsumerAdmission::Stale);
  f.scope = replacement;
  f.fire(events::StopStreamEvent{.session_id = 42, .connection_scope = old});
  f.fire(events::StopStreamEvent{.session_id = 42}); // null is not wildcard
  REQUIRE(f.admit(ConsumerKind::Video, [&](auto &, auto &) {
    ++starts;
    return true;
  }) == ConsumerAdmission::Accepted);
  f.ping(ConsumerKind::Video);
  REQUIRE(eventually([&] { return f.joined(ConsumerKind::Video); }));
  REQUIRE(starts == 1);
}

TEST_CASE("Unknown retains worker ownership and denies replacement and bounded capacity", "[consumer]") {
  auto policy = fast_policy();
  policy.max_unknown = 1;
  Gate gate;
  Fixture f(policy);
  REQUIRE(f.admit(ConsumerKind::Video, [&](auto &, auto &) {
    gate.wait();
    return true;
  }) == ConsumerAdmission::Accepted);
  f.ping(ConsumerKind::Video);
  REQUIRE(eventually([&] { return gate.entered.load(); }));
  f.fire(events::StopStreamEvent{.session_id = 42, .connection_scope = f.scope});
  REQUIRE(eventually([&] { return f.runtime->status(f.scope, ConsumerKind::Video)->state == ConsumerState::Unknown; }));
  REQUIRE_FALSE(f.runtime->status(f.scope, ConsumerKind::Video)->joined);
  auto ticket = f.runtime->revoke(f.scope);
  REQUIRE(ticket);
  REQUIRE_FALSE(ticket->complete());
  auto old = f.scope;
  auto replacement = ConnectionScope::fresh(42);
  auto unrelated = ConnectionScope::fresh(43);
  f.current->set({replacement, unrelated});
  f.scope = replacement;
  REQUIRE(f.admit(ConsumerKind::Video, [](auto &, auto &) { return true; }) == ConsumerAdmission::Busy);
  REQUIRE(f.runtime->admit(unrelated, RunScope::fresh("43"), ConsumerKind::Video, {}, [](auto &, auto &) {
    return true;
  }) == ConsumerAdmission::Capacity);
  gate.release();
  REQUIRE(eventually([&] { return f.runtime->status(old, ConsumerKind::Video)->joined; }));
  REQUIRE(eventually([&] { return ticket->complete(); }));
  REQUIRE(f.admit(ConsumerKind::Video, [](auto &, auto &) { return true; }) == ConsumerAdmission::Accepted);
  REQUIRE_FALSE(f.runtime->status(old, ConsumerKind::Video)); // old record pruned
  REQUIRE(ticket->connection.matches(old));
  REQUIRE(ticket->complete()); // durable proof survives pruning and ID reuse
  f.ping(ConsumerKind::Video);
  REQUIRE(eventually([&] { return f.joined(ConsumerKind::Video); }));
}

TEST_CASE("Consumer capacity bounds waiters and preserves stop tombstones", "[consumer]") {
  auto policy = fast_policy();
  policy.max_jobs = 1;
  policy.max_records = 1;
  Fixture f(policy);
  REQUIRE(f.admit(ConsumerKind::Video, [](auto &, auto &) { return true; }) == ConsumerAdmission::Accepted);
  REQUIRE(f.admit(ConsumerKind::Audio, [](auto &, auto &) { return true; }) == ConsumerAdmission::Capacity);
  auto other = ConnectionScope::fresh(43);
  f.current->set({f.scope, other});
  f.runtime->revoke(other); // no room for tombstone => fail closed, never forget stop
  f.ping(ConsumerKind::Video);
  REQUIRE(eventually([&] { return f.joined(ConsumerKind::Video); }));
  REQUIRE(f.runtime->admit(other, RunScope::fresh("43"), ConsumerKind::Audio, {}, [](auto &, auto &) {
    return true;
  }) == ConsumerAdmission::Capacity);
}

TEST_CASE("EOS is not completion until private-context drain and worker join", "[consumer][software-gst]") {
  software_gst();
  Gate drain;
  std::atomic_bool context_owned{false}, pipeline_destroyed{false};
  Fixture f(fast_policy());
  REQUIRE(f.admit(ConsumerKind::Video, [&](ConsumerJob &job, auto &) {
    return run_consumer_pipeline(
        "fakesrc num-buffers=1 ! fakesink sync=false",
        job,
        {.setup =
             [&](GstElement *pipeline) {
               context_owned = g_main_context_is_owner(g_main_context_get_thread_default());
               g_object_weak_ref(
                   G_OBJECT(pipeline),
                   +[](gpointer data, GObject *) { *static_cast<std::atomic_bool *>(data) = true; },
                   &pipeline_destroyed);
             },
         .drain = [&](GstElement *) { drain.wait(); }});
  }) == ConsumerAdmission::Accepted);
  f.ping(ConsumerKind::Video);
  REQUIRE(eventually([&] { return drain.entered.load(); }));
  REQUIRE(context_owned);
  REQUIRE_FALSE(f.joined(ConsumerKind::Video));
  REQUIRE_FALSE(pipeline_destroyed);
  REQUIRE(eventually([&] { return f.runtime->status(f.scope, ConsumerKind::Video)->state == ConsumerState::Unknown; }));
  drain.release();
  REQUIRE(eventually([&] { return f.joined(ConsumerKind::Video); }));
  REQUIRE(f.runtime->status(f.scope, ConsumerKind::Video)->state == ConsumerState::Stopped);
  REQUIRE(pipeline_destroyed);
}

TEST_CASE("Consumer commands execute on their private context and do not touch persistent producers",
          "[consumer][software-gst]") {
  software_gst();
  auto producer = RunScope::fresh("lobby");
  std::atomic_int commands{0}, stops{0};
  std::atomic_bool correct{false};
  Fixture f;
  auto observer = f.bus->register_handler<immer::box<events::GameStopRequest>>(
      [&](const immer::box<events::GameStopRequest> &) { ++stops; });
  REQUIRE(f.admit(ConsumerKind::Video, [&](ConsumerJob &job, auto &) {
    auto worker = std::this_thread::get_id();
    return run_consumer_pipeline("fakesrc is-live=true ! identity sleep-time=10000 ! fakesink sync=false",
                                 job,
                                 {.commands = [&](GstElement *, ConsumerCommands command) {
                                   if (command.idr || command.producer) {
                                     correct = g_main_context_is_owner(g_main_context_get_thread_default()) &&
                                               std::this_thread::get_id() == worker && command.producer &&
                                               command.producer->matches(producer);
                                     ++commands;
                                   }
                                 }});
  }) == ConsumerAdmission::Accepted);
  f.fire(events::SwitchStreamProducerEvents{.session_id = 42,
                                            .interpipe_src_id = "lobby",
                                            .connection_scope = f.scope,
                                            .producer_scope = producer});
  f.fire(events::IDRRequestEvent{.session_id = 42, .connection_scope = f.scope});
  f.ping(ConsumerKind::Video);
  REQUIRE(eventually([&] { return commands.load() == 1; }));
  REQUIRE(correct);
  f.fire(events::PauseStreamEvent{.session_id = 42, .connection_scope = f.scope});
  REQUIRE(eventually([&] { return f.joined(ConsumerKind::Video); }));
  REQUIRE(stops == 0);
  REQUIRE(producer);
  observer.unregister();
}

TEST_CASE("In-flight Gst callback stays owned through cancellation and NULL drain", "[consumer][software-gst]") {
  software_gst();
  Gate callback;
  ConsumerCallbackBarrier barrier;
  std::atomic_bool drained{false};
  struct CallbackData {
    Gate &gate;
    ConsumerCallbackBarrier &barrier;
  } data{callback, barrier};
  Fixture f(fast_policy());
  REQUIRE(f.admit(ConsumerKind::Audio, [&](ConsumerJob &job, auto &) {
    return run_consumer_pipeline("fakesrc num-buffers=1 ! fakesink name=blocked signal-handoffs=true sync=false",
                                 job,
                                 {.setup =
                                      [&](GstElement *pipeline) {
                                        auto sink = gst_bin_get_by_name(GST_BIN(pipeline), "blocked");
                                        g_signal_connect(
                                            sink,
                                            "handoff",
                                            G_CALLBACK(+[](GstElement *, GstBuffer *, GstPad *, gpointer raw) {
                                              auto &data = *static_cast<CallbackData *>(raw);
                                              auto lease = data.barrier.enter();
                                              if (lease)
                                                data.gate.wait();
                                            }),
                                            &data);
                                        gst_object_unref(sink);
                                      },
                                  .drain =
                                      [&](GstElement *pipeline) {
                                        auto sink = gst_bin_get_by_name(GST_BIN(pipeline), "blocked");
                                        g_signal_handlers_disconnect_by_data(sink, &data);
                                        gst_object_unref(sink);
                                        barrier.close_and_wait();
                                        drained = true;
                                      }});
  }) == ConsumerAdmission::Accepted);
  f.ping(ConsumerKind::Audio);
  REQUIRE(eventually([&] { return callback.entered.load(); }));
  f.fire(events::StopStreamEvent{.session_id = 42, .connection_scope = f.scope});
  REQUIRE(eventually([&] { return f.runtime->status(f.scope, ConsumerKind::Audio)->state == ConsumerState::Unknown; }));
  REQUIRE_FALSE(f.joined(ConsumerKind::Audio));
  REQUIRE_FALSE(drained);
  callback.release();
  REQUIRE(eventually([&] { return f.joined(ConsumerKind::Audio); }));
  REQUIRE(drained);
}

TEST_CASE("Revocation after ping but before Gst setup never starts a pipeline", "[consumer][software-gst]") {
  software_gst();
  Gate activation;
  std::atomic_int setups{0};
  Fixture f;
  REQUIRE(f.admit(ConsumerKind::Video, [&](ConsumerJob &job, const auto &) {
    activation.wait();
    return run_consumer_pipeline("fakesrc num-buffers=1 ! fakesink sync=false", job, {.setup = [&](GstElement *) {
                                   ++setups;
                                 }});
  }) == ConsumerAdmission::Accepted);
  f.ping(ConsumerKind::Video);
  REQUIRE(eventually([&] { return activation.entered.load(); }));
  auto ticket = f.runtime->revoke(f.scope);
  REQUIRE(ticket);
  REQUIRE_FALSE(ticket->complete());
  activation.release();
  REQUIRE(eventually([&] { return ticket->complete(); }));
  REQUIRE(setups == 0);
}

TEST_CASE("A broken or absent completion ticket never proves release", "[consumer]") {
  ConsumerDrainTicket empty;
  REQUIRE_FALSE(empty.complete());
  std::shared_future<void> broken;
  {
    std::promise<void> lost;
    broken = lost.get_future().share();
  }
  ConsumerDrainTicket ticket{ConnectionScope::fresh(42), broken};
  REQUIRE_FALSE(ticket.complete());
}
