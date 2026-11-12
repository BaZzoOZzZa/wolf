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
extern "C" {
GST_PLUGIN_STATIC_DECLARE(rtpmoonlightpay_audio);
}
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

TEST_CASE("Actual audio GObject output rejects invalid AES key before consuming crypto and stays closed",
          "[diagnostic][audio-lifetime]") {
  diagnostic_audio_gst();
  for (const auto &key : {std::string{}, std::string("00"), std::string(30, '1'), std::string(34, '1')}) {
    unsigned input_releases = 0;
    AudioObject object{
        GST_ELEMENT(g_object_new(gst_TYPE_rtp_moonlight_pay_audio, "aes-key", key.c_str(), "aes-iv", "17", nullptr))};
    auto pay = gst_rtp_moonlight_pay_audio(object.value);
    auto trans = GST_BASE_TRANSFORM(object.value);
    auto invoke = [&] {
      auto input = gst_buffer_new_and_fill(16, 1);
      gst_mini_object_weak_ref(
          GST_MINI_OBJECT(input),
          [](gpointer data, GstMiniObject *) { ++*static_cast<unsigned *>(data); },
          &input_releases);
      trans->queued_buf = input; // transfer-full, actual generate_output must retire it once.
      GstBuffer *out = input;    // sentinel: callback must overwrite, never dereferenced afterward.
      auto result = GST_BASE_TRANSFORM_GET_CLASS(trans)->generate_output(trans, &out);
      CHECK(out == nullptr);
      CHECK(trans->queued_buf == nullptr);
      return result;
    };
    CHECK(invoke() == GST_FLOW_ERROR);
    CHECK(input_releases == 1);
    CHECK(pay->encrypt);
    CHECK(pay->encrypt_failed);
    CHECK(pay->cur_seq_number == 0);
    g_object_set(object.value, "aes-key", "00112233445566778899aabbccddeeff", nullptr);
    CHECK(invoke() == GST_FLOW_ERROR); // fixing the property does not reopen failed instance.
    CHECK(input_releases == 2);
    CHECK(pay->cur_seq_number == 0);
  }
}

TEST_CASE("Actual audio split keeps invalid IV encryption failure closed", "[diagnostic][audio-lifetime]") {
  diagnostic_audio_gst();
  AudioObject object{GST_ELEMENT(g_object_new(gst_TYPE_rtp_moonlight_pay_audio,
                                              "aes-key",
                                              "00112233445566778899aabbccddeeff",
                                              "aes-iv",
                                              "not-a-number",
                                              nullptr))};
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

// Supervised legacy input-only trial: replace device IO, not the dispatcher or
// slot/lifetime decision. These providers never open uinput/uhid or send packets.
namespace {
struct TrialIO {
  bool has_pad = false, compatible = true, create_ok = true;
  int factories = 0, writes = 0, last_slot = -1;
  uint32_t buttons = 0;
  int lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;
  void set_pressed_buttons(uint32_t value) {
    buttons = value;
  }
  void set_stick(inputtino::Joypad::STICK_POSITION side, short x, short y) {
    if (side == inputtino::Joypad::LS) {
      lx = x;
      ly = y;
    } else {
      rx = x;
      ry = y;
    }
  }
  void set_triggers(unsigned char left, unsigned char right) {
    lt = left;
    rt = right;
  }
  control::detail::TrialPadIO ops() {
    return {[&] { return compatible; },
            [&](int) { return has_pad; },
            [&](int slot) {
              ++factories;
              last_slot = slot;
              has_pad = create_ok;
              return has_pad;
            },
            [&](int slot, const CONTROLLER_MULTI_PACKET &pkt) {
              ++writes;
              last_slot = slot;
              control::detail::apply_trial_frame(*this, pkt);
              return has_pad;
            }};
  }
};
events::StreamSession trial_session(int slot = 0) {
  auto s = session(false);
  s.app->legacy_pad_only_trial = true;
  s.app->legacy_pad_slot = slot;
  s.app->start_virtual_compositor = false;
  s.app->start_audio_server = false;
  return s;
}
immer::box<std::shared_ptr<control::ControlPeer>> trial_peer(unsigned &sends) {
  auto peer = std::make_shared<control::ControlPeer>();
  peer->send = [&](std::string_view) {
    ++sends;
    return true;
  };
  return {peer}; // Trusted software boundary, NOT authentication/current proof.
}
} // namespace

TEST_CASE("PadOnly trial config defaults and contradictory policy fail before app construction", "[pad-only-trial]") {
  config::BaseApp app{.title = "trial",
                      .start_virtual_compositor = false,
                      .start_audio_server = false,
                      .runner = config::AppCMD{.run_cmd = "unused"}};
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
  auto normal = parse()->load()->front();
  CHECK_FALSE(events::pad_only_trial(*normal));
  CHECK(normal->legacy_pad_slot.get() == 0);
  app.legacy_pad_only_trial = true;
  for (int slot : {0, 7, 15}) {
    app.legacy_pad_slot = slot;
    auto parsed = parse()->load()->front();
    CHECK(events::pad_only_trial(*parsed));
    CHECK(parsed->legacy_pad_slot.get() == slot);
    CHECK_FALSE(events::device_free(*parsed));
    CHECK(rfl::json::write(*parsed).find("legacy_pad") == std::string::npos);
  }
  for (int slot : {-1, 16, 256}) {
    app.legacy_pad_slot = slot;
    CHECK_THROWS_AS(parse(), std::invalid_argument);
  }
  app.legacy_pad_slot = 0;
  app.device_free_diagnostic = true;
  CHECK_THROWS_AS(parse(), std::invalid_argument);
  app.device_free_diagnostic = false;
  app.start_virtual_compositor = true;
  CHECK_THROWS_AS(parse(), std::invalid_argument);
  app.start_virtual_compositor = false;
  app.start_audio_server = true;
  CHECK_THROWS_AS(parse(), std::invalid_argument);
  app.start_audio_server = false;
  app.runner = config::AppDocker{};
  CHECK_THROWS_AS(parse(), std::invalid_argument);
  app.legacy_pad_only_trial = false;
  CHECK_NOTHROW(parse()); // unrelated normal configuration remains permitted
}

TEST_CASE("PadOnly trial actual startup has no devices and one runner", "[pad-only-trial]") {
  for (int slot : {0, 11, 15}) {
    Startup io;
    auto s = trial_session(slot);
    auto done = diag::start(*s.app, io.ops());
    if (done.valid())
      done.get();
    CHECK(io.mouse == 0);
    CHECK(io.keyboard == 0);
    CHECK(io.audio == 0);
    CHECK(io.compositor == 0);
    CHECK(io.bind == 0);
    CHECK(io.wait == 0);
    CHECK(io.runner == 1);
    CHECK(io.failed == 0);
  }
  for (int invalid : {0, 1, 2, 3}) {
    Startup io;
    auto s = trial_session();
    if (invalid == 0)
      s.app->device_free_diagnostic = true;
    if (invalid == 1)
      s.app->start_virtual_compositor = true;
    if (invalid == 2)
      s.app->start_audio_server = true;
    if (invalid == 3)
      s.app->legacy_pad_slot = 16;
    auto done = diag::start(*s.app, io.ops());
    CHECK(io.failed == 1);
    CHECK(io.runner == 0);
    CHECK(io.mouse + io.keyboard + io.audio + io.compositor + io.wait == 0);
  }
}

TEST_CASE("PadOnly trial common factory selection overrides PS motion and per-client overrides", "[pad-only-trial]") {
  auto s = trial_session();
  for (auto override : {config::ControllerType::PS, config::ControllerType::NINTENDO, config::ControllerType::AUTO}) {
    s.client_settings = config::ClientSettings{.controllers_override = {override},
                                               .motion_controller_override = config::ControllerType::PS};
    for (auto advertised : {PS, NINTENDO, XBOX, moonlight::control::pkts::UNKNOWN}) {
      CHECK(control::detail::selected_joypad_type(s, 0, advertised, GYRO | ACCELEROMETER | RUMBLE) ==
            config::ControllerType::XBOX);
    }
  }
  s.app->legacy_pad_only_trial = false;
  CHECK(control::detail::selected_joypad_type(s, 0, PS, GYRO) == config::ControllerType::PS);
  s.client_settings = config::ClientSettings{.controllers_override = {config::ControllerType::NINTENDO}};
  CHECK(control::detail::selected_joypad_type(s, 0, PS, GYRO) == config::ControllerType::NINTENDO);
}

TEST_CASE("PadOnly trial actual decrypted dispatch keeps one pad across arrival and masks", "[pad-only-trial]") {
  for (int selected : {0, 7, 15}) {
    TrialIO io;
    auto ops = io.ops();
    auto s = trial_session(selected);
    unsigned sends = 0, plugs = 0, unplugs = 0, combos = 0;
    auto peer = trial_peer(sends);
    auto p = s.event_bus->register_handler<immer::box<events::PlugDeviceEvent>>(
        [&](const immer::box<events::PlugDeviceEvent> &) { ++plugs; });
    auto u = s.event_bus->register_handler<immer::box<events::UnplugDeviceEvent>>(
        [&](const immer::box<events::UnplugDeviceEvent> &) { ++unplugs; });
    auto c = s.event_bus->register_handler<immer::box<events::ClientWolfUIComboEvent>>(
        [&](const immer::box<events::ClientWolfUIComboEvent> &) { ++combos; });
    auto multi = packet<CONTROLLER_MULTI_PACKET>(CONTROLLER_MULTI);
    multi.controller_number = boost::endian::native_to_little(short(selected));
    CHECK(control::handle_input(s, peer, bytes(multi), ops)); // inactive before arrival: no factory
    CHECK(io.factories == 0);
    CHECK(io.writes == 0);
    multi.active_gamepad_mask = boost::endian::native_to_little(short(uint16_t(1u << selected)));
    multi.button_flags = boost::endian::native_to_little(
        short(inputtino::Joypad::START | inputtino::Joypad::DPAD_UP | inputtino::Joypad::RIGHT_BUTTON));
    multi.left_stick_x = boost::endian::native_to_little(short(-32768));
    multi.left_stick_y = boost::endian::native_to_little(short(32767));
    multi.right_stick_x = boost::endian::native_to_little(short(1234));
    multi.right_stick_y = boost::endian::native_to_little(short(-2345));
    multi.left_trigger = 255;
    multi.right_trigger = 91;
    const std::string key = "00112233445566778899aabbccddeeff";
    auto encrypted = encrypt_packet(key, 1, bytes(multi), EncryptionMode::v2, Direction::client);
    auto clear = decrypt_packet({reinterpret_cast<const char *>(encrypted.get()), encrypted->full_size()},
                                key,
                                EncryptionMode::v2,
                                Direction::client);
    REQUIRE(control::handle_input(s, peer, clear, ops)); // MULTI before ARRIVAL
    CHECK(io.factories == 1);
    CHECK(io.last_slot == selected);
    CHECK(io.lx == -32768);
    CHECK(io.ly == 32767);
    CHECK(io.rx == 1234);
    CHECK(io.ry == -2345);
    CHECK(io.lt == 255);
    CHECK(io.rt == 91);
    CHECK(io.buttons == uint16_t(boost::endian::little_to_native(multi.button_flags)));
    auto arrival = packet<CONTROLLER_ARRIVAL_PACKET>(CONTROLLER_ARRIVAL);
    arrival.controller_number = selected;
    arrival.controller_type = PS;
    arrival.capabilities = GYRO | ACCELEROMETER | RUMBLE;
    for (int cycle = 0; cycle < 3; ++cycle) {
      REQUIRE(control::handle_input(s, peer, bytes(arrival), ops));
      multi.active_gamepad_mask = 0;
      REQUIRE(control::handle_input(s, peer, bytes(multi), ops));
      CHECK(io.buttons == 0);
      CHECK(io.lx == 0);
      CHECK(io.ly == 0);
      CHECK(io.rx == 0);
      CHECK(io.ry == 0);
      CHECK(io.lt == 0);
      CHECK(io.rt == 0);
      CHECK(io.has_pad);
      multi.active_gamepad_mask = boost::endian::native_to_little(short(uint16_t(1u << selected)));
      REQUIRE(control::handle_input(s, peer, bytes(multi), ops));
    }
    CHECK(io.factories == 1);
    CHECK(sends + plugs + unplugs + combos == 0);
    CHECK_FALSE(s.mouse->has_value());
    CHECK_FALSE(s.keyboard->has_value());
  }
}

TEST_CASE("PadOnly trial refuses wrong slots null peers broken scope and IO refusal without effects",
          "[pad-only-trial]") {
  TrialIO io;
  auto ops = io.ops();
  auto s = trial_session(3);
  unsigned sends = 0;
  auto peer = trial_peer(sends);
  for (int slot = 0; slot < 16; ++slot) {
    auto arrival = packet<CONTROLLER_ARRIVAL_PACKET>(CONTROLLER_ARRIVAL);
    arrival.controller_number = slot;
    auto multi = packet<CONTROLLER_MULTI_PACKET>(CONTROLLER_MULTI);
    multi.controller_number = boost::endian::native_to_little(short(slot));
    multi.active_gamepad_mask = boost::endian::native_to_little(short(uint16_t(1u << slot)));
    if (slot != 3) {
      CHECK_FALSE(control::handle_input(s, peer, bytes(arrival), ops));
      CHECK_FALSE(control::handle_input(s, peer, bytes(multi), ops));
      control::controller_arrival(arrival, s, peer); // actual direct helpers: no injected IO/device
      control::controller_multi(multi, s, peer);
    }
    CHECK_FALSE(control::handle_input(s, {}, bytes(arrival), ops)); // actual Unix null-peer shape
    CHECK_FALSE(control::handle_input(s, {}, bytes(multi), ops));
    control::controller_arrival(arrival, s, {});
    control::controller_multi(multi, s, {});
  }
  auto arrival = packet<CONTROLLER_ARRIVAL_PACKET>(CONTROLLER_ARRIVAL);
  arrival.controller_number = 3;
  auto original = s.connection_scope;
  s.connection_scope = sessions::ConnectionScope::fresh(99);
  CHECK_FALSE(control::handle_input(s, peer, bytes(arrival), ops));
  s.connection_scope = {};
  CHECK_FALSE(control::handle_input(s, peer, bytes(arrival), ops));
  s.connection_scope = original;
  auto run = s.run_scope;
  s.run_scope = {};
  CHECK_FALSE(control::handle_input(s, peer, bytes(arrival), ops));
  s.run_scope = run;
  auto empty_peer = std::make_shared<control::ControlPeer>();
  CHECK_FALSE(control::handle_input(s, {empty_peer}, bytes(arrival), ops));
  io.compatible = false;
  CHECK_FALSE(control::handle_input(s, peer, bytes(arrival), ops));
  CHECK(io.factories == 0);
  CHECK(io.writes == 0);
  io.compatible = true;
  io.create_ok = false;
  CHECK_FALSE(control::handle_input(s, peer, bytes(arrival), ops));
  CHECK(io.factories == 1);
  CHECK(io.writes == 0);
  CHECK(sends == 0);
  CHECK(s.joypads->load()->empty());
}

TEST_CASE("PadOnly trial rejects all non-pad common and typed input paths", "[pad-only-trial]") {
  TrialIO io;
  auto ops = io.ops();
  auto s = trial_session();
  unsigned sends = 0;
  auto peer = trial_peer(sends);
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
  auto text = packet<UTF8_TEXT_PACKET>(UTF8_TEXT);
  text.text[0] = 'x';
  text.data_size = boost::endian::native_to_big(uint32_t(sizeof(INPUT_TYPE) + 1));
  text.packet_len = boost::endian::native_to_little(uint16_t(sizeof(INPUT_TYPE) + 5));
  wires.emplace_back(reinterpret_cast<const char *>(&text), sizeof(INPUT_TYPE) + 9);
  for (const auto &wire : wires) {
    REQUIRE(valid_input_packet(wire));
    CHECK_FALSE(control::handle_input(s, peer, wire, ops));
    CHECK_FALSE(control::handle_input(s, {}, wire));
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
  CHECK(io.factories + io.writes + sends == 0);
  CHECK(s.joypads->load()->empty());
  CHECK_FALSE(s.mouse->has_value());
  CHECK_FALSE(s.keyboard->has_value());
  CHECK_FALSE(s.touch_screen->has_value());
  CHECK_FALSE(s.pen_tablet->has_value());
  CHECK(s.held_modifiers->load().get() == 0);
}

TEST_CASE("PadOnly trial actual lobby handlers refuse source and retained target attachments", "[pad-only-trial]") {
  for (bool trial_source : {false, true}) {
    auto source = trial_source ? trial_session() : session(false);
    auto target = trial_session();
    target.session_id = 84;
    target.connection_scope = sessions::ConnectionScope::fresh(84);
    target.run_scope = sessions::RunScope::fresh("84");
    auto state = immer::box<state::AppState>{
        state::AppState{.event_bus = source.event_bus,
                        .lobbies = std::make_shared<immer::atom<immer::vector<events::Lobby>>>(),
                        .running_sessions = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>()}};
    state->running_sessions->store(immer::vector<events::StreamSession>{source, target});
    events::Lobby lobby{.id = "L",
                        .name = "test",
                        .started_by_profile_id = "test",
                        .multi_user = false,
                        .stop_when_everyone_leaves = false,
                        .run_scope = sessions::RunScope::fresh("L")};
    auto members = immer::map<size_t, std::shared_ptr<events::StreamSession>>{}.set(
        source.session_id,
        std::make_shared<events::StreamSession>(source));
    if (!trial_source)
      members = members.set(target.session_id, std::make_shared<events::StreamSession>(target));
    lobby.members->store(members);
    lobby.connected_sessions->store(immer::vector<immer::box<std::string>>{immer::box<std::string>{"42"}});
    REQUIRE(state::get_lobby_for_session({lobby}, source).has_value());
    state->lobbies->store(immer::vector<events::Lobby>{lobby});
    auto handlers = sessions::setup_lobbies_handlers(state, "", {});
    unsigned relayed_unplug = 0;
    auto unplug_handler = source.event_bus->register_handler<immer::box<events::UnplugDeviceEvent>>(
        [&](const immer::box<events::UnplugDeviceEvent> &event) {
          if (event->session_id == "L")
            ++relayed_unplug;
        });
    auto plug =
        immer::box<events::PlugDeviceEvent>{events::PlugDeviceEvent{.session_id = "42",
                                                                    .run_scope = source.run_scope,
                                                                    .connection_scope = source.connection_scope}};
    source.event_bus->fire_event(plug);
    source.event_bus->fire_event(
        immer::box<events::UnplugDeviceEvent>{events::UnplugDeviceEvent{.session_id = "42",
                                                                        .run_scope = source.run_scope,
                                                                        .connection_scope = source.connection_scope}});
    CHECK_FALSE(lobby.plugged_devices_queue->pop(0ms).has_value());
    CHECK(relayed_unplug == 0);
    auto queue = std::make_shared<events::devices_atom_queue>();
    if (trial_source) {
      CHECK_FALSE(diag::queue_device(plug, *state->running_sessions->load(), {}, source.run_scope, queue));
      CHECK_FALSE(queue->pop(0ms).has_value());
    }
    if (!trial_source) {
      auto alias = target;
      alias.run_scope = source.run_scope;
      CHECK_FALSE(diag::queue_device(plug, {source, alias}, {}, source.run_scope, queue));
      CHECK_FALSE(queue->pop(0ms).has_value());
    }
    events::JoinLobbyEvent join{.lobby_id = "L",
                                .moonlight_session_id = source.session_id,
                                .lobby_scope = lobby.run_scope,
                                .connection_scope = source.connection_scope};
    auto answer = join.error_message.get()->get_future();
    source.event_bus->fire_event(immer::box<events::JoinLobbyEvent>{join});
    REQUIRE(answer.wait_for(0ms) == std::future_status::ready);
    CHECK(answer.get() == "Isolated app cannot join lobbies");
    CHECK(lobby.members->load()->size() == members.size());
    CHECK(source.joypads->load()->empty());
  }
}

#include <control/control_loop.hpp>
#include <deque>

namespace {
// Transport provider only: actual ControlLoop owns auth/current/replay/dispatch.
// Raw ENet packet allocation/free is real, host/service never opens a socket.
struct TrialTransport : std::enable_shared_from_this<TrialTransport> {
  struct Event {
    ENetEventType type;
    uint32_t token;
    std::string bytes;
  };
  std::mutex mutex;
  std::deque<Event> events;
  std::atomic<unsigned> disconnected{0}, freed{0};
  void push(ENetEventType type, uint32_t token = 0, std::string bytes = {}) {
    std::lock_guard lock(mutex);
    events.push_back({type, token, std::move(bytes)});
  }
  control::loop::Operations ops() {
    return {
        shared_from_this(),
        [](void *, const char *, uint16_t, size_t) {
          auto host = std::make_unique<ENetHost>();
          host->peers = new ENetPeer[1]{};
          host->peerCount = 1;
          return host.release();
        },
        [](void *context, ENetHost *host, ENetEvent *out, uint32_t) {
          auto &self = *static_cast<TrialTransport *>(context);
          std::optional<Event> next;
          {
            std::lock_guard lock(self.mutex);
            if (!self.events.empty()) {
              next.emplace(std::move(self.events.front()));
              self.events.pop_front();
            }
          }
          if (!next) {
            std::this_thread::sleep_for(1ms);
            return 0;
          }
          *out = {};
          out->type = next->type;
          out->peer = host->peers;
          out->data = next->token;
          if (next->type == ENET_EVENT_TYPE_RECEIVE)
            out->packet = enet_packet_create(next->bytes.data(), next->bytes.size(), 0);
          return 1;
        },
        [](void *, const void *bytes, size_t size, uint32_t flags) { return enet_packet_create(bytes, size, flags); },
        [](void *, ENetPeer *, uint8_t, ENetPacket *) { return -1; },
        [](void *context, ENetPacket *packet) {
          enet_packet_destroy(packet);
          ++static_cast<TrialTransport *>(context)->freed;
        },
        [](void *, ENetPeer *) {},
        [](void *context, ENetPeer *) { ++static_cast<TrialTransport *>(context)->disconnected; },
        [](void *, ENetHost *) {},
        [](void *, ENetHost *host) {
          delete[] host->peers;
          delete host;
        }};
  }
};
bool trial_eventually(const std::function<bool()> &probe) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!probe() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  return probe();
}
} // namespace

TEST_CASE("PadOnly trial actual control owner retires stale connection before input dispatch", "[pad-only-trial]") {
  auto transport = std::make_shared<TrialTransport>();
  auto s = trial_session();
  s.aes_key = "00112233445566778899aabbccddeeff";
  s.enet_secret_payload = 123;
  s.control_crypto->mode = EncryptionMode::v2;
  auto running = std::make_shared<immer::atom<immer::vector<events::StreamSession>>>();
  running->store(immer::vector<events::StreamSession>{s});
  // Domain is destroyed/joined before all captured fixture data, on every assertion unwind.
  control::loop::ControlLoopDomain domain;
  auto loop = domain.start({.peers = 1}, running, s.event_bus, transport->ops());
  REQUIRE(loop);
  REQUIRE(trial_eventually([&] { return loop->status().ready; }));
  auto encrypted = [&](uint32_t sequence) {
    auto inactive = packet<CONTROLLER_MULTI_PACKET>(CONTROLLER_MULTI);
    auto wire = encrypt_packet(s.aes_key, sequence, bytes(inactive), EncryptionMode::v2, Direction::client);
    return std::string(reinterpret_cast<const char *>(wire.get()), wire->full_size());
  };
  transport->push(ENET_EVENT_TYPE_CONNECT, 123);
  REQUIRE(trial_eventually([&] { return loop->peers().size() == 1; }));
  CHECK_FALSE(loop->peers()[0].authenticated);
  transport->push(ENET_EVENT_TYPE_RECEIVE, 0, encrypted(0));
  REQUIRE(trial_eventually([&] {
    auto peers = loop->peers();
    return peers.size() == 1 && peers[0].authenticated;
  }));
  auto old = loop->peers()[0];
  auto replacement = s;
  replacement.connection_scope = sessions::ConnectionScope::fresh(s.session_id);
  running->store(immer::vector<events::StreamSession>{replacement});
  auto freed_before = transport->freed.load();
  transport->push(ENET_EVENT_TYPE_RECEIVE, 0, encrypted(1));
  REQUIRE(trial_eventually([&] { return loop->peers().empty() && transport->freed > freed_before; }));
  CHECK(transport->disconnected == 1);
  CHECK_FALSE(old.sender->send("stale"));
  transport->push(ENET_EVENT_TYPE_CONNECT, 123); // same raw slot, new connection lifetime
  REQUIRE(trial_eventually([&] { return loop->peers().size() == 1; }));
  CHECK_FALSE(loop->peers()[0].token.matches(old.token));
  transport->push(ENET_EVENT_TYPE_RECEIVE, 0, encrypted(1)); // stale packet did not consume new authority
  REQUIRE(trial_eventually([&] {
    auto peers = loop->peers();
    return peers.size() == 1 && peers[0].authenticated;
  }));
  loop->request_stop();
  REQUIRE(trial_eventually([&] { return loop->join_when_exited() == control::loop::JoinResult::Joined; }));
  CHECK_FALSE(loop->status().unknown);
  CHECK(s.joypads->load()->empty()); // only inactive MULTI, no device factory even on regression
}
