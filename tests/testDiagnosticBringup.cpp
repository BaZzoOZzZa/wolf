#include <audio/startup.hpp>
#include <catch2/catch_test_macros.hpp>
#include <control/input_handler.hpp>
#include <events/reflectors.hpp>
#include <rfl/toml.hpp>
#include <sessions/diagnostic_startup.hpp>
#include <sessions/handlers.hpp>

using namespace wolf::core;
using namespace moonlight::control;
using namespace moonlight::control::pkts;
using namespace std::chrono_literals;
namespace config = wolf::config;
namespace diag = wolf::core::sessions::diagnostic;
namespace ast = wolf::core::audio::startup;
namespace state {
std::shared_ptr<immer::atom<immer::vector<immer::box<events::App>>>>
parse_apps(const std::vector<config::BaseApp> &,
           const std::string &,
           const std::string &,
           const config::BaseAppVideoOverride &,
           const std::string &,
           const std::string &,
           const std::string &,
           const config::BaseAppAudioOverride &,
           SessionsAtoms,
           const std::shared_ptr<events::EventBusType> &);
}
namespace {
events::StreamSession session(bool diagnostic = true) {
  auto s = events::StreamSession{.app = std::make_shared<events::App>(events::App{}),
                                 .session_id = 42,
                                 .connection_scope = sessions::ConnectionScope::fresh(42),
                                 .run_scope = sessions::RunScope::fresh("42")};
  s.event_bus = std::make_shared<events::EventBusType>();
  s.app->device_free_diagnostic = diagnostic;
  return s;
}
struct Startup {
  boost::promise<streaming::WaylandDisplayReady> producer;
  int mouse = 0, keyboard = 0, audio = 0, compositor = 0, bind = 0, wait = 0, runner = 0, failed = 0;
  bool socket_ready = true;
  std::vector<std::string> order;
  diag::StartupOps ops() {
    return {[&] {
              ++compositor;
              return producer.get_future();
            },
            [&] { ++mouse; },
            [&] { ++keyboard; },
            [&] {
              ++audio;
              order.push_back("audio");
            },
            [&](const auto &) { ++bind; },
            [&](const auto &ready) {
              ++wait;
              return socket_ready && !ready.wayland_socket_name.empty();
            },
            [&] {
              ++runner;
              order.push_back("runner");
            },
            [&] { ++failed; }};
  }
};
template <class T> T packet(INPUT_TYPE type) {
  T p{};
  p.packet_type = INPUT_DATA;
  p.packet_len = boost::endian::native_to_little(uint16_t(sizeof(T) - 4));
  p.data_size = boost::endian::native_to_big(uint32_t(sizeof(T) - 8));
  p.type = type;
  if constexpr (std::is_same_v<T, MOUSE_MOVE_ABS_PACKET>) {
    p.width = boost::endian::native_to_big<int16_t>(640);
    p.height = boost::endian::native_to_big<int16_t>(360);
  }
  if constexpr (std::is_same_v<T, CONTROLLER_MOTION_PACKET>)
    p.motion_type = ACCELERATION;
  return p;
}
template <class T> std::string bytes(const T &p) {
  return {reinterpret_cast<const char *>(&p), sizeof(p)};
}
} // namespace

TEST_CASE("Diagnostic actual no-compositor readiness never waits for a Wayland socket", "[diagnostic][readiness]") {
  for (bool device_free : {false, true}) {
    Startup io;
    auto s = session(device_free);
    s.app->start_virtual_compositor = false;
    s.app->start_audio_server = !device_free;
    auto done = diag::start(*s.app, io.ops());
    if (done.valid())
      done.get();
    CHECK(io.mouse == (device_free ? 0 : 1));
    CHECK(io.keyboard == (device_free ? 0 : 1));
    CHECK(io.audio == (device_free ? 0 : 1));
    CHECK(io.compositor == 0);
    CHECK(io.bind == 0);
    CHECK(io.wait == 0);
    CHECK(io.failed == 0);
    CHECK(io.runner == 1);
    if (!device_free)
      CHECK(io.order == std::vector<std::string>{"audio", "runner"});
  }
}
TEST_CASE("Actual compositor completion retains ready wait and failure behavior", "[diagnostic][readiness]") {
  for (bool socket_ready : {false, true}) {
    Startup io;
    io.socket_ready = socket_ready;
    auto s = session(false);
    s.app->start_virtual_compositor = true;
    s.app->start_audio_server = true;
    auto done = diag::start(*s.app, io.ops());
    CHECK(io.runner == 0);
    CHECK(io.mouse == 0);
    CHECK(io.keyboard == 0);
    CHECK(io.audio == 1);
    io.producer.set_value({.wayland_socket_name = "owned-socket"});
    done.get();
    CHECK(io.compositor == 1);
    CHECK(io.bind == 1);
    CHECK(io.wait == 1);
    CHECK(io.runner == int(socket_ready));
    CHECK(io.failed == int(!socket_ready));
  }
  Startup io;
  auto s = session(false);
  s.app->start_virtual_compositor = true;
  auto done = diag::start(*s.app, io.ops());
  io.producer.set_exception(boost::copy_exception(std::runtime_error("producer failed")));
  CHECK_THROWS(done.get());
  CHECK(io.runner == 0);
  CHECK(io.bind == 0);
  CHECK(io.wait == 0);
}
TEST_CASE("Actual config parse validates diagnostic before runner construction", "[diagnostic]") {
  config::BaseApp app{.title = "ball",
                      .start_virtual_compositor = false,
                      .start_audio_server = false,
                      .runner = config::AppCMD{.run_cmd = "sleep 600"}};
  auto parse = [&] {
    return state::parse_apps(
        {app},
        "",
        "",
        {.source = "src", .sink = "sink", .producer_buffer_caps = "caps", .h264_encoder = "encoder"},
        "params",
        "",
        "",
        {.source = "audio", .audio_params = "params", .opus_encoder = "opus", .sink = "sink"},
        std::make_shared<immer::atom<immer::vector<events::StreamSession>>>(),
        std::make_shared<events::EventBusType>());
  };
  CHECK_FALSE(events::device_free(*parse()->load()->front()));
  app.device_free_diagnostic = true;
  CHECK(events::device_free(*parse()->load()->front()));
  CHECK(rfl::json::write(*parse()->load()->front()).find("device_free_diagnostic") == std::string::npos);
  app.start_virtual_compositor = true;
  CHECK_THROWS(parse());
  app.start_virtual_compositor = false;
  app.start_audio_server = true;
  CHECK_THROWS(parse());
  app.start_audio_server = false;
  app.runner = config::AppDocker{};
  CHECK_THROWS(parse());
  app.device_free_diagnostic = false;
  CHECK_NOTHROW(parse());
  Startup io;
  auto s = session();
  s.app->start_virtual_compositor = true;
  auto done = diag::start(*s.app, io.ops());
  CHECK(io.failed == 1);
  CHECK(io.compositor == 0);
  CHECK(io.mouse == 0);
}
TEST_CASE("Diagnostic common input ingress refuses all valid types after real V2 decrypt", "[diagnostic]") {
  auto s = session();
  unsigned plugs = 0, combos = 0;
  auto plug = s.event_bus->register_handler<immer::box<events::PlugDeviceEvent>>(
      [&](const immer::box<events::PlugDeviceEvent> &) { ++plugs; });
  auto combo = s.event_bus->register_handler<immer::box<events::ClientWolfUIComboEvent>>(
      [&](const immer::box<events::ClientWolfUIComboEvent> &) { ++combos; });
  auto wires = std::vector<std::string>{bytes(packet<MOUSE_MOVE_REL_PACKET>(MOUSE_MOVE_REL)),
                                        bytes(packet<MOUSE_MOVE_ABS_PACKET>(MOUSE_MOVE_ABS)),
                                        bytes(packet<MOUSE_BUTTON_PACKET>(MOUSE_BUTTON_PRESS)),
                                        bytes(packet<MOUSE_BUTTON_PACKET>(MOUSE_BUTTON_RELEASE)),
                                        bytes(packet<MOUSE_SCROLL_PACKET>(MOUSE_SCROLL)),
                                        bytes(packet<MOUSE_HSCROLL_PACKET>(MOUSE_HSCROLL)),
                                        bytes(packet<KEYBOARD_PACKET>(KEY_PRESS)),
                                        bytes(packet<KEYBOARD_PACKET>(KEY_RELEASE)),
                                        bytes(packet<TOUCH_PACKET>(TOUCH)),
                                        bytes(packet<PEN_PACKET>(PEN)),
                                        bytes(packet<CONTROLLER_TOUCH_PACKET>(CONTROLLER_TOUCH)),
                                        bytes(packet<CONTROLLER_MOTION_PACKET>(CONTROLLER_MOTION)),
                                        bytes(packet<CONTROLLER_BATTERY_PACKET>(CONTROLLER_BATTERY)),
                                        bytes(packet<HAPTICS_PACKET>(HAPTICS))};
  // UTF8 is variable-size, use an independently bounded one-character frame.
  auto text = packet<UTF8_TEXT_PACKET>(UTF8_TEXT);
  text.text[0] = 'x';
  text.data_size = boost::endian::native_to_big(uint32_t(sizeof(INPUT_TYPE) + 1));
  text.packet_len = boost::endian::native_to_little(uint16_t(sizeof(INPUT_TYPE) + 5));
  wires.emplace_back(reinterpret_cast<const char *>(&text), sizeof(INPUT_TYPE) + 9);
  for (int slot = 0; slot < 16; ++slot) {
    auto multi = packet<CONTROLLER_MULTI_PACKET>(CONTROLLER_MULTI);
    multi.controller_number = slot;
    multi.active_gamepad_mask = uint16_t(1u << slot);
    multi.button_flags = inputtino::Joypad::START | inputtino::Joypad::DPAD_UP | inputtino::Joypad::RIGHT_BUTTON;
    wires.push_back(bytes(multi)); // deliberately BEFORE arrival
    auto arrival = packet<CONTROLLER_ARRIVAL_PACKET>(CONTROLLER_ARRIVAL);
    arrival.controller_number = slot;
    wires.push_back(bytes(arrival));
    control::controller_multi(multi, s, {});
    control::controller_arrival(arrival, s, {});
    CHECK_FALSE(control::handle_input(s, {}, &multi));
    CHECK_FALSE(control::handle_input(s, {}, &arrival));
  }
  uint32_t sequence = 0;
  const std::string key = "00112233445566778899aabbccddeeff";
  for (const auto &wire : wires) {
    REQUIRE(valid_input_packet(wire));
    auto encrypted = encrypt_packet(key, sequence++, wire, EncryptionMode::v2, Direction::client);
    auto clear = decrypt_packet(std::string(reinterpret_cast<const char *>(encrypted.get()), encrypted->full_size()),
                                key,
                                EncryptionMode::v2,
                                Direction::client);
    CHECK_FALSE(control::handle_input(s, {}, clear)); // SAME ENet/Unix API common call
    CHECK_FALSE(control::handle_input(s, {}, wire.substr(0, wire.size() - 1)));
  }
  control::mouse_move_rel(packet<MOUSE_MOVE_REL_PACKET>(MOUSE_MOVE_REL), s);
  control::mouse_move_abs(packet<MOUSE_MOVE_ABS_PACKET>(MOUSE_MOVE_ABS), s);
  control::mouse_button(packet<MOUSE_BUTTON_PACKET>(MOUSE_BUTTON_PRESS), s);
  control::mouse_scroll(packet<MOUSE_SCROLL_PACKET>(MOUSE_SCROLL), s);
  control::mouse_h_scroll(packet<MOUSE_HSCROLL_PACKET>(MOUSE_HSCROLL), s);
  control::keyboard_key(packet<KEYBOARD_PACKET>(KEY_PRESS), s);
  control::utf8_text(text, s);
  control::touch(packet<TOUCH_PACKET>(TOUCH), s);
  control::pen(packet<PEN_PACKET>(PEN), s);
  control::controller_touch(packet<CONTROLLER_TOUCH_PACKET>(CONTROLLER_TOUCH), s);
  control::controller_motion(packet<CONTROLLER_MOTION_PACKET>(CONTROLLER_MOTION), s);
  control::controller_battery(packet<CONTROLLER_BATTERY_PACKET>(CONTROLLER_BATTERY), s);
  CHECK(plugs == 0);
  CHECK(combos == 0);
  CHECK(s.joypads->load()->empty());
  CHECK_FALSE(s.mouse->has_value());
  CHECK_FALSE(s.keyboard->has_value());
  CHECK_FALSE(s.pen_tablet->has_value());
  CHECK_FALSE(s.touch_screen->has_value());
  s.app->device_free_diagnostic = false;
  CHECK(control::handle_input(s, {}, bytes(packet<HAPTICS_PACKET>(HAPTICS)))); // unchanged harmless normal path
}
TEST_CASE("Diagnostic exact Plug queues and real lobby Join relay refuse attachment", "[diagnostic]") {
  for (bool disabled : {false, true}) {
    auto s = session(disabled);
    auto state = immer::box<state::AppState>{
        state::AppState{.event_bus = s.event_bus,
                        .lobbies = std::make_shared<immer::atom<immer::vector<events::Lobby>>>(),
                        .running_sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>()}};
    state->running_sessions->store(immer::vector<events::StreamSession>{s});
    auto queue = std::make_shared<events::devices_atom_queue>();
    auto event = immer::box<events::PlugDeviceEvent>{
        events::PlugDeviceEvent{.session_id = "42", .run_scope = s.run_scope, .connection_scope = s.connection_scope}};
    CHECK(diag::queue_device(event, *state->running_sessions->load(), {}, s.run_scope, queue) == !disabled);
    CHECK(queue->pop(0ms).has_value() == !disabled);
    events::Lobby lobby{.id = "L",
                        .name = "test",
                        .started_by_profile_id = "test",
                        .multi_user = false,
                        .stop_when_everyone_leaves = false,
                        .run_scope = sessions::RunScope::fresh("L")};
    lobby.connected_sessions->store(immer::vector<immer::box<std::string>>{immer::box<std::string>{"42"}});
    lobby.members->store(
        immer::map<size_t, std::shared_ptr<events::StreamSession>>{}.set(42,
                                                                         std::make_shared<events::StreamSession>(s)));
    state->lobbies->store(immer::vector<events::Lobby>{lobby});
    auto handlers = sessions::setup_lobbies_handlers(state, "", {});
    state->event_bus->fire_event(event);
    CHECK(lobby.plugged_devices_queue->pop(0ms).has_value() == !disabled);
    if (disabled) {
      events::JoinLobbyEvent join{.lobby_id = "L",
                                  .moonlight_session_id = 42,
                                  .lobby_scope = lobby.run_scope,
                                  .connection_scope = s.connection_scope};
      auto answer = join.error_message.get()->get_future();
      state->event_bus->fire_event(immer::box<events::JoinLobbyEvent>{join});
      REQUIRE(answer.wait_for(0ms) == std::future_status::ready);
      CHECK(answer.get() == "Diagnostic app cannot join lobbies");
      CHECK(lobby.members->load()->size() == 1); // no migration/removal or alias mutation
    }
  }
}
TEST_CASE("Actual audio startup refuses fallback and ingress on failed or disconnected Pulse", "[diagnostic]") {
  // Opaque provider handle: never dereferenced or passed to real Pulse functions.
  auto storage = std::make_shared<int>(0);
  auto server = std::shared_ptr<audio::Server>(storage, reinterpret_cast<audio::Server *>(storage.get()));
  int connects = 0, fallbacks = 0, ingress = 0;
  bool healthy = false;
  ast::Operations ops{[&] {
                        ++connects;
                        return server;
                      },
                      [&](const auto &) { return healthy; },
                      [&]() -> std::optional<sessions::AudioServer> {
                        ++fallbacks;
                        return {};
                      }};
  auto result = ast::setup("FALSE", ops);
  CHECK_FALSE(result);
  CHECK(connects == 1);
  CHECK(fallbacks == 0);
  CHECK_FALSE(ast::dispatch(result, ops, [&] { ++ingress; }));
  CHECK(ingress == 0);
  result = ast::setup("TRUE", ops);
  CHECK_FALSE(result);
  CHECK(fallbacks == 1);
  healthy = true;
  result = ast::setup("FALSE", ops);
  REQUIRE(result);
  CHECK(fallbacks == 1);
  CHECK(ast::dispatch(result, ops, [&] { ++ingress; }));
  CHECK(ingress == 1);
  healthy = false;
  CHECK_FALSE(ast::dispatch(result, ops, [&] { ++ingress; }));
  CHECK(ingress == 1);
  CHECK_FALSE(ast::dispatch(sessions::AudioServer{}, ops, [&] { ++ingress; }));
}
TEST_CASE("Actual mDNS dispatch exact FALSE skips whole worker without dependency suppression", "[diagnostic]") {
  int launches = 0;
  diag::dispatch_mdns("FALSE", [&] { ++launches; });
  CHECK(launches == 0);
  diag::dispatch_mdns("TRUE", [&] { ++launches; });
  CHECK(launches == 1);
  diag::dispatch_mdns("", [&] { ++launches; });
  CHECK(launches == 2);
}

#include <rest/helpers.hpp>
namespace endpoints::https {
std::optional<moonlight::DisplayMode> parse_run_display_mode(const SimpleWeb::CaseInsensitiveMultimap &, bool, bool);
std::shared_ptr<events::StreamSession> create_run_session(const SimpleWeb::CaseInsensitiveMultimap &,
                                                          const std::string &,
                                                          const state::PairedClient &,
                                                          immer::box<state::AppState>,
                                                          const events::App &);
} // namespace endpoints::https

TEST_CASE("Actual launch mode parsing owns SSO and bounded token storage", "[diagnostic][launch-mode]") {
  namespace https = endpoints::https;
  auto fallback = https::parse_run_display_mode({}, true, false);
  REQUIRE(fallback);
  CHECK(fallback->width == 1920);
  CHECK(fallback->height == 1080);
  CHECK(fallback->refreshRate == 60);
  CHECK(fallback->hevc_supported);
  CHECK_FALSE(fallback->av1_supported);
  for (auto mode :
       {std::string("640x360x30"), std::string("1920x1080x60"), std::string("2147483647x2147483647x2147483647")}) {
    auto parsed = https::parse_run_display_mode({{"mode", mode}}, false, true);
    REQUIRE(parsed);
    mode.assign(512, 'z'); // caller/provider storage is gone; result has owned values
    CHECK_FALSE(parsed->hevc_supported);
    CHECK(parsed->av1_supported);
    CHECK(parsed->width > 0);
    CHECK(parsed->height > 0);
    CHECK(parsed->refreshRate > 0);
  }
  auto small = https::parse_run_display_mode({{"mode", std::string("640x360x30")}}, false, false);
  REQUIRE(small);
  CHECK(small->width == 640);
  CHECK(small->height == 360);
  CHECK(small->refreshRate == 30);
  for (const auto &bad : std::vector<std::string>{"",
                                                  "640",
                                                  "640x360",
                                                  "640x360x30x1",
                                                  "x360x30",
                                                  "640xx30",
                                                  "640x360x",
                                                  "0x360x30",
                                                  "640x-1x30",
                                                  "640x360x+30",
                                                  " 640x360x30",
                                                  "640x360x30junk",
                                                  "2147483648x360x30",
                                                  "640x360x2147483648",
                                                  std::string(33, '1'),
                                                  std::string("640x360x30\0junk", 15)}) {
    CHECK_FALSE(https::parse_run_display_mode({{"mode", bad}}, true, true));
  }
}

TEST_CASE("Actual create_run_session rejects bad mode before directories scopes or publication",
          "[diagnostic][launch-mode]") {
  namespace https = endpoints::https;
  // Owned temporary filesystem fixture only. No server, runner, device, or socket.
  struct Directory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
                                 ("wolf-launch-mode-" + crypto::str_to_hex(crypto::random(8)));
    Directory() {
      std::filesystem::create_directory(path);
    }
    ~Directory() {
      std::error_code ec;
      std::filesystem::remove_all(path, ec);
    }
  } dir;
  auto bus = std::make_shared<events::EventBusType>();
  unsigned streams = 0, runners = 0;
  auto stream_handler = bus->register_handler<immer::box<events::StreamSession>>(
      [&](const immer::box<events::StreamSession> &) { ++streams; });
  auto runner_handler = bus->register_handler<immer::box<events::StartRunner>>(
      [&](const immer::box<events::StartRunner> &) { ++runners; });
  auto running = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>();
  auto state = immer::box<state::AppState>{
      state::AppState{.config = immer::box<state::Config>{state::Config{.support_hevc = false, .support_av1 = false}},
                      .host = immer::box<state::Host>{state::Host{.host_base_state_folder = dir.path.string(),
                                                                  .local_base_state_folder = dir.path.string()}},
                      .event_bus = bus,
                      .running_sessions = running}};
  const auto app = events::App{.base = {.title = "mode-fixture", .id = "1", .support_hdr = false}};
  const auto client = state::PairedClient{.client_cert = "fixture-not-TLS", .app_state_folder = "client"};
  for (const auto &bad : {"", "640", "640xx30", "640x360x30x1", "640x360x2147483648"}) {
    CHECK_FALSE(https::create_run_session({{"mode", bad}}, "127.0.0.1", client, state, app));
    CHECK(std::filesystem::is_empty(dir.path));
    CHECK(running->load()->empty());
    CHECK(streams == 0);
    CHECK(runners == 0);
  }
  const SimpleWeb::CaseInsensitiveMultimap headers{{"mode", "640x360x30"},
                                                   {"rikey", "00112233445566778899aabbccddeeff"},
                                                   {"rikeyid", "1"}};
  auto created = https::create_run_session(headers, "127.0.0.1", client, state, app);
  REQUIRE(created);
  CHECK(created->display_mode.width == 640);
  CHECK(created->display_mode.height == 360);
  CHECK(created->display_mode.refreshRate == 30);
  CHECK(bool(created->connection_scope));
  CHECK(bool(created->run_scope));
  CHECK(created->ip == "127.0.0.1");
  CHECK(std::filesystem::is_directory(dir.path / "client" / "mode-fixture"));
  CHECK(running->load()->empty()); // construction alone never publishes or starts anything
  CHECK(streams == 0);
  CHECK(runners == 0);
}

#include <cstddef>
#include <enet/protocol.h>
TEST_CASE("Pinned actual ENet header offset and wire layout remain unchanged", "[diagnostic][enet-offset]") {
  static_assert(offsetof(ENetProtocolHeader, sentTime) == 2);
  static_assert(sizeof(ENetProtocolHeader) == 4);
  CHECK(offsetof(ENetProtocolHeader, peerID) == 0);
  CHECK(offsetof(ENetProtocolHeader, sentTime) == sizeof(enet_uint16));
  CHECK(sizeof(ENetProtocolHeader) == 2 * sizeof(enet_uint16));
}

#include <gst-plugin/audio.hpp>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
GST_PLUGIN_STATIC_DECLARE(rtpmoonlightpay_audio);
namespace {
void diagnostic_audio_gst() {
  static const bool initialized = [] {
    g_setenv("GST_PLUGIN_SYSTEM_PATH_1_0", "", TRUE);
    g_setenv("GST_PLUGIN_PATH_1_0", "", TRUE);
    g_setenv("GST_REGISTRY_UPDATE", "no", TRUE);
    if (!gst_init_check(nullptr, nullptr, nullptr))
      return false;
    moonlight::fec::init();
    GST_PLUGIN_STATIC_REGISTER(rtpmoonlightpay_audio);
    return true;
  }();
  REQUIRE(initialized);
  guint major, minor, micro, nano;
  gst_version(&major, &minor, &micro, &nano);
  REQUIRE(major == 1);
  REQUIRE(minor == 28);
  REQUIRE(micro == 7);
  auto plugins = gst_registry_get_plugin_list(gst_registry_get());
  for (auto item = plugins; item; item = item->next) {
    auto plugin = GST_PLUGIN(item->data);
    if (gst_plugin_is_loaded(plugin)) {
      const std::string name = gst_plugin_get_name(plugin);
      CHECK((name == "staticelements" || name == "rtpmoonlightpay_audio"));
    }
  }
  gst_plugin_list_free(plugins);
}
struct AudioObject {
  GstElement *value;
  ~AudioObject() {
    if (value)
      gst_object_unref(value);
  }
};
struct AudioPipeline {
  GstElement *value = gst_pipeline_new(nullptr);
  ~AudioPipeline() {
    if (value) {
      gst_element_set_state(value, GST_STATE_NULL);
      gst_object_unref(value);
    }
  }
};
} // namespace

TEST_CASE("Actual audio GObject constructs properties and finalizes C++ owners after repeated dispose",
          "[diagnostic][audio-lifetime]") {
  diagnostic_audio_gst();
  for (int iteration = 0; iteration < 8; ++iteration) {
    AudioObject object{GST_ELEMENT(g_object_new(gst_TYPE_rtp_moonlight_pay_audio,
                                                "aes-key",
                                                "00112233445566778899aabbccddeeff",
                                                "aes-iv",
                                                "1234567890123456789",
                                                nullptr))};
    REQUIRE(object.value);
    auto pay = gst_rtp_moonlight_pay_audio(object.value);
    REQUIRE(pay->rs);
    REQUIRE(pay->packets_buffer);
    std::weak_ptr<reed_solomon> rs = pay->rs;
    CHECK(pay->aes_key == crypto::hex_to_str("00112233445566778899aabbccddeeff", true));
    gchar *iv = nullptr;
    gboolean encrypt = false;
    gint duration = 0;
    g_object_get(object.value, "aes-iv", &iv, "encrypt", &encrypt, "packet-duration", &duration, nullptr);
    std::unique_ptr<gchar, decltype(&g_free)> iv_owner(iv, g_free);
    REQUIRE(iv);
    CHECK(std::string(iv) == "1234567890123456789");
    CHECK(encrypt);
    CHECK(duration == 5);
    g_object_run_dispose(G_OBJECT(object.value));
    CHECK(pay->packets_buffer == nullptr);
    g_object_run_dispose(G_OBJECT(object.value));
    CHECK(pay->packets_buffer == nullptr);
    CHECK_FALSE(rs.expired()); // C++ properties/state remain alive until finalize.
    g_object_set(object.value, "aes-iv", "17", nullptr);
    CHECK(pay->aes_iv == "17");
    gst_object_unref(std::exchange(object.value, nullptr));
    CHECK(rs.expired()); // actual finalize released RS, not merely dispose entry.
  }
}

TEST_CASE("Actual static audio payloader pipeline encrypts four buffers and emits two FEC packets",
          "[diagnostic][audio-lifetime]") {
  diagnostic_audio_gst();
  AudioPipeline pipeline;
  REQUIRE(pipeline.value);
  auto src = GST_ELEMENT(g_object_new(GST_TYPE_APP_SRC, "format", GST_FORMAT_TIME, nullptr));
  gst_bin_add(GST_BIN(pipeline.value), src);
  auto pay = gst_element_factory_make("rtpmoonlightpay_audio", nullptr);
  REQUIRE(pay);
  gst_bin_add(GST_BIN(pipeline.value), pay);
  auto sink = GST_ELEMENT(g_object_new(GST_TYPE_APP_SINK, "sync", FALSE, "async", FALSE, "max-buffers", 8u, nullptr));
  gst_bin_add(GST_BIN(pipeline.value), sink);
  const auto key = crypto::hex_to_str("00112233445566778899aabbccddeeff", true);
  g_object_set(pay, "aes-key", "00112233445566778899aabbccddeeff", "aes-iv", "17", "encrypt", TRUE, nullptr);
  auto caps = gst_caps_new_empty_simple("application/octet-stream");
  gst_app_src_set_caps(GST_APP_SRC(src), caps);
  gst_caps_unref(caps);
  REQUIRE(gst_element_link_many(src, pay, sink, nullptr));
  std::weak_ptr<reed_solomon> rs = gst_rtp_moonlight_pay_audio(pay)->rs;
  REQUIRE(gst_element_set_state(pipeline.value, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE);
  const std::string payload = "bounded dummy encoded audio";
  for (unsigned i = 0; i < 4; ++i) {
    auto buffer = gst_buffer_new_allocate(nullptr, payload.size(), nullptr);
    gst_buffer_fill(buffer, 0, payload.data(), payload.size());
    GST_BUFFER_PTS(buffer) = i * 5 * GST_MSECOND;
    GST_BUFFER_DURATION(buffer) = 5 * GST_MSECOND;
    REQUIRE(gst_app_src_push_buffer(GST_APP_SRC(src), buffer) == GST_FLOW_OK);
  }
  REQUIRE(gst_app_src_end_of_stream(GST_APP_SRC(src)) == GST_FLOW_OK);
  for (unsigned i = 0; i < 6; ++i) {
    auto sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 2 * GST_SECOND);
    std::unique_ptr<GstSample, decltype(&gst_sample_unref)> owner(sample, gst_sample_unref);
    REQUIRE(sample);
    auto buffer = gst_sample_get_buffer(sample);
    REQUIRE(buffer);
    const auto bytes = gst_buffer_copy_content(buffer);
    if (i < 4) {
      REQUIRE(bytes.size() > ::audio::RTP_HEADER_SIZE);
      ::audio::AudioRTPHeaders header{};
      memcpy(&header, bytes.data(), sizeof(header));
      CHECK(header.rtp.packetType == 97);
      CHECK(boost::endian::big_to_native(header.rtp.sequenceNumber) == i);
      CHECK(boost::endian::big_to_native(header.rtp.timestamp) == i * 5);
      const std::string ciphertext(bytes.begin() + ::audio::RTP_HEADER_SIZE, bytes.end());
      CHECK(ciphertext != payload);
      CHECK(crypto::aes_decrypt_cbc(ciphertext, key, derive_iv("17", i), true) == payload);
    } else {
      REQUIRE(bytes.size() > ::audio::FEC_HEADER_SIZE);
      ::audio::AudioFECPacket header{};
      memcpy(&header, bytes.data(), sizeof(header));
      CHECK(header.rtp.packetType == 127);
      CHECK(header.fec_header.fecShardIndex == i - 4);
      CHECK(header.fec_header.baseSequenceNumber == 0);
      CHECK(header.fec_header.payloadType == 97);
    }
  }
  auto extra = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 2 * GST_SECOND);
  if (extra)
    gst_sample_unref(extra);
  CHECK(extra == nullptr);
  CHECK(gst_app_sink_is_eos(GST_APP_SINK(sink)));
  REQUIRE(gst_element_set_state(pipeline.value, GST_STATE_NULL) != GST_STATE_CHANGE_FAILURE);
  gst_object_unref(std::exchange(pipeline.value, nullptr));
  CHECK(rs.expired());
}

TEST_CASE("Actual audio payloader keeps encryption failure closed", "[diagnostic][audio-lifetime]") {
  diagnostic_audio_gst();
  AudioObject object{
      GST_ELEMENT(g_object_new(gst_TYPE_rtp_moonlight_pay_audio, "aes-key", "00", "aes-iv", "17", nullptr))};
  auto pay = gst_rtp_moonlight_pay_audio(object.value);
  auto buffer = gst_buffer_new_and_fill(16, 1);
  std::unique_ptr<GstBuffer, decltype(&gst_buffer_unref)> owner(buffer, gst_buffer_unref);
  auto packets = ::audio::split_into_rtp(pay, buffer);
  std::unique_ptr<GstBufferList, decltype(&gst_buffer_list_unref)> packet_owner(packets, gst_buffer_list_unref);
  CHECK(gst_buffer_list_length(packets) == 0);
  CHECK(pay->encrypt);
  CHECK(pay->encrypt_failed);
  CHECK(pay->cur_seq_number == 0);
}
