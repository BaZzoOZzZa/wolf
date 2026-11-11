#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

using Catch::Matchers::Equals;

#include <moonlight/control.hpp>
using namespace moonlight::control;

static std::string to_string(const ControlEncryptedPacket &packet) {
  return {(char *)&packet, packet.full_size()};
}

TEST_CASE("Control AES Encryption", "CONTROL") {
  // A bunch of packets taken from a real session

  std::string aes_key = "EDF04A215C4FBEA20934120C8480D855";

  SECTION("30 bytes") { // original packet: 01001A0000000000BF0EB6DA10E47C702EC8644EB87D9CF7B6FAC9FF75CA
    std::string payload = crypto::hex_to_str("020302000000");
    std::uint32_t seq = 0;
    auto encrypted_packet = *encrypt_packet(aes_key, seq, payload);
    REQUIRE_THAT(crypto::str_to_hex(to_string(encrypted_packet)),
                 Equals("01001A0000000000BF0EB6DA10E47C702EC8644EB87D9CF7B6FAC9FF75CA"));
    REQUIRE(boost::endian::little_to_native(encrypted_packet.seq) == seq);
    REQUIRE(boost::endian::little_to_native(encrypted_packet.header.length) ==
            sizeof(encrypted_packet.seq) + GCM_TAG_SIZE + payload.length());

    auto decrypted = decrypt_packet(encrypted_packet, aes_key);
    REQUIRE_THAT(decrypted, Equals(payload));
    REQUIRE_THAT(packet_type_to_str(((ControlPacket *)decrypted.data())->type), Equals("IDR_FRAME"));

    encrypted_packet.gcm_tag[0] ^= 1;
    REQUIRE_THROWS_AS(decrypt_packet(encrypted_packet, aes_key), std::runtime_error);
  }

  SECTION("29 bytes") { // original packet: 010019000100000021DBB8DC0590AF3A2B20BCE5A347DE31D366E5B9C5"
    std::string payload = crypto::hex_to_str("0703010000");
    std::uint32_t seq = 1;
    auto encrypted_packet = *encrypt_packet(aes_key, seq, payload);
    REQUIRE_THAT(crypto::str_to_hex(to_string(encrypted_packet)),
                 Equals("010019000100000021DBB8DC0590AF3A2B20BCE5A347DE31D366E5B9C5"));
    REQUIRE(boost::endian::little_to_native(encrypted_packet.seq) == seq);
    REQUIRE(boost::endian::little_to_native(encrypted_packet.header.length) ==
            sizeof(encrypted_packet.seq) + GCM_TAG_SIZE + payload.length());

    auto decrypted = decrypt_packet(encrypted_packet, aes_key);
    REQUIRE_THAT(decrypted, Equals(payload));
    REQUIRE_THAT(packet_type_to_str(((ControlPacket *)decrypted.data())->type), Equals("START_B"));
  }

  SECTION("36 bytes") { // original packet: 0100200002000000220722FBADED58A03F2E8898F0F1DCB7C93F6235590618E4186AD990
    std::string payload = crypto::hex_to_str("000208000400000000000000");
    std::uint32_t seq = 2;
    auto encrypted_packet = *encrypt_packet(aes_key, seq, payload);
    REQUIRE_THAT(crypto::str_to_hex(to_string(encrypted_packet)),
                 Equals("0100200002000000220722FBADED58A03F2E8898F0F1DCB7C93F6235590618E4186AD990"));
    REQUIRE(boost::endian::little_to_native(encrypted_packet.seq) == seq);
    REQUIRE(boost::endian::little_to_native(encrypted_packet.header.length) ==
            sizeof(encrypted_packet.seq) + GCM_TAG_SIZE + payload.length());

    auto decrypted = decrypt_packet(encrypted_packet, aes_key);
    REQUIRE_THAT(decrypted, Equals(payload));
    REQUIRE_THAT(packet_type_to_str(((ControlPacket *)decrypted.data())->type), Equals("PERIODIC_PING"));
  }

  SECTION("46 bytes") { // original packet:
                        // 01002A00060000005A4D999FB2542F85BDD39D99F77EB825254569D2C04E21241B5CEC01BD3F93129718ECC1F153
    std::string payload = crypto::hex_to_str("060212000000000E05000000033400C00000059F0329");
    std::uint32_t seq = 6;
    auto encrypted_packet = *encrypt_packet(aes_key, seq, payload);
    REQUIRE_THAT(
        crypto::str_to_hex(to_string(encrypted_packet)),
        Equals("01002A00060000005A4D999FB2542F85BDD39D99F77EB825254569D2C04E21241B5CEC01BD3F93129718ECC1F153"));
    REQUIRE(boost::endian::little_to_native(encrypted_packet.seq) == seq);
    REQUIRE(boost::endian::little_to_native(encrypted_packet.header.length) ==
            sizeof(encrypted_packet.seq) + GCM_TAG_SIZE + payload.length());

    auto decrypted = decrypt_packet(encrypted_packet, aes_key);
    REQUIRE_THAT(decrypted, Equals(payload));
    REQUIRE_THAT(packet_type_to_str(((ControlPacket *)decrypted.data())->type), Equals("INPUT_DATA"));
  }
}

TEST_CASE("control joypad input packets") {
  std::string payload =
      crypto::hex_to_str("060222000000001E0C0000001A000000010014000010000000000000000000009C0000005500");

  auto input_data = (pkts::CONTROLLER_MULTI_PACKET *)payload.data();
  auto pressed_btns = input_data->button_flags | (input_data->buttonFlags2 << 16);

  REQUIRE(input_data->type == pkts::CONTROLLER_MULTI);
  REQUIRE(input_data->active_gamepad_mask == 1);
  REQUIRE(pressed_btns & pkts::CONTROLLER_BTN::A);
}
TEST_CASE("Control V2 agrees with Moonlight directional wire vectors", "[CONTROL][security]") {
  // Moonlight-common-c ControlStream.c: LE32 sequence + six zero bytes + HC/CC.
  // Ciphertext/tag independently generated with Python cryptography AESGCM.
  const std::string key = "EDF04A215C4FBEA20934120C8480D855";
  auto payload = crypto::hex_to_str("020302000000");
  struct Vector {
    uint32_t seq;
    Direction direction;
    const char *iv;
    const char *tag_cipher;
  };
  for (auto v :
       {Vector{0, Direction::host, "000000000000000000004843", "C0DD077889DCE3DE827617277E9D1E5ACCABADC5EEF9"},
        Vector{255, Direction::host, "FF0000000000000000004843", "EC1796860ABE1D498E7D80F5BD8DBA59108600193C43"},
        Vector{256, Direction::host, "000100000000000000004843", "8E3A1AAF24E64131662126B3A1A2BA97142B93B3626A"},
        Vector{UINT32_MAX, Direction::host, "FFFFFFFF0000000000004843", "7BF0B9D10A1C981B143EE41EA8C07B5ED8A958D58049"},
        Vector{256, Direction::client, "000100000000000000004343", "A9572CC9A3AB7C7038C4FD10EB744ACFE8705673234E"}}) {
    REQUIRE(crypto::str_to_hex(control_iv(v.seq, EncryptionMode::v2, v.direction)) == v.iv);
    auto packet = encrypt_packet(key, v.seq, payload, EncryptionMode::v2, v.direction);
    auto wire = to_string(*packet);
    REQUIRE(crypto::str_to_hex(wire.substr(8)) == v.tag_cipher);
    REQUIRE(decrypt_packet(wire, key, EncryptionMode::v2, v.direction) == payload);
    auto wrong_direction = v.direction == Direction::host ? Direction::client : Direction::host;
    REQUIRE_THROWS_AS(decrypt_packet(wire, key, EncryptionMode::v2, wrong_direction), std::runtime_error);
    REQUIRE_THROWS_AS(decrypt_packet(wire, key, EncryptionMode::legacy), std::runtime_error);
  }
}

TEST_CASE("Control key lifetime, exhaustion and replay policy", "[CONTROL][security]") {
  ControlCryptoState negotiation;
  REQUIRE_FALSE(negotiation.negotiate(0, false));
  REQUIRE(negotiation.mode == EncryptionMode::unnegotiated);
  REQUIRE_FALSE(negotiation.negotiate(3, false));
  REQUIRE(negotiation.negotiate(CONTROL_V2, false));
  REQUIRE(negotiation.negotiate(CONTROL_V2, false));
  REQUIRE_FALSE(negotiation.negotiate(0, true));

  ControlKeyRegistry registry;
  auto first = registry.find_or_insert(std::string(16, 'a'));
  REQUIRE(first);
  REQUIRE(first->reserve_sequence() == 0);
  REQUIRE(first->received.accept(0));
  auto resumed = registry.find_or_insert(std::string(16, 'a'));
  REQUIRE(resumed == first);
  REQUIRE(resumed->reserve_sequence() == 1);
  REQUIRE_FALSE(resumed->received.accept(0));
  REQUIRE(resumed->received.accept(5));
  REQUIRE(resumed->received.accept(3)); // ENet channels may arrive out of order
  REQUIRE_FALSE(resumed->received.accept(3));
  REQUIRE(resumed->received.accept(2048));
  REQUIRE_FALSE(resumed->received.accept(1));
  first->next_sequence = UINT32_MAX;
  REQUIRE(first->reserve_sequence() == UINT32_MAX);
  REQUIRE_FALSE(first->reserve_sequence());
  REQUIRE_FALSE(first->reserve_sequence());
  for (uint32_t i = 0; i < ControlKeyRegistry::capacity - 1; ++i) {
    std::string key(16, '\0');
    std::memcpy(key.data(), &i, sizeof(i));
    REQUIRE(registry.find_or_insert(key));
  }
  REQUIRE_FALSE(registry.find_or_insert(std::string(16, 'b')));
  REQUIRE(registry.find_or_insert(std::string(16, 'a')) == first);
  REQUIRE_FALSE(registry.find_or_insert("short"));
}

TEST_CASE("Encrypted envelope validates the actual received extent", "[CONTROL][security]") {
  std::string key = "EDF04A215C4FBEA20934120C8480D855";
  auto packet = encrypt_packet(key, 0, crypto::hex_to_str("020302000000"));
  const auto wire = to_string(*packet);
  for (size_t size = 0; size < wire.size(); ++size) {
    // Own exactly the received bytes (including zero); ASan can catch any overread.
    const std::string short_wire(wire.data(), size);
    REQUIRE_THROWS_AS(decrypt_packet(short_wire, key), std::runtime_error);
  }
  REQUIRE_THROWS_AS(decrypt_packet(wire + "x", key), std::runtime_error);
  for (uint16_t length : {0, 1, 19, 20, 65535}) {
    auto malformed = wire;
    auto le = boost::endian::native_to_little(length);
    std::memcpy(malformed.data() + 2, &le, sizeof(le));
    REQUIRE_THROWS_AS(decrypt_packet(malformed, key), std::runtime_error);
  }
  bool saw_nul = false;
  for (uint32_t seq = 0; seq < 256; ++seq) {
    auto encrypted = encrypt_packet(key, seq, "test");
    saw_nul |= std::string_view(encrypted->gcm_tag, GCM_TAG_SIZE).find('\0') != std::string_view::npos;
    REQUIRE(decrypt_packet(to_string(*encrypted), key) == "test");
  }
  REQUIRE(saw_nul);
  REQUIRE_THROWS_AS(encrypt_packet(key, 0, std::string(MAX_PAYLOAD_SIZE + 1, 'x')), std::runtime_error);
}

template <typename T> static std::string input_wire(pkts::INPUT_TYPE type) {
  T packet{};
  packet.packet_type = pkts::INPUT_DATA;
  packet.packet_len = boost::endian::native_to_little(static_cast<uint16_t>(sizeof(T) - 4));
  packet.data_size = boost::endian::native_to_big(static_cast<uint32_t>(sizeof(T) - 8));
  packet.type = type;
  if constexpr (std::is_same_v<T, pkts::MOUSE_MOVE_ABS_PACKET>) {
    packet.width = boost::endian::native_to_big<int16_t>(1920);
    packet.height = boost::endian::native_to_big<int16_t>(1080);
  }
  return {reinterpret_cast<const char *>(&packet), sizeof(packet)};
}

TEST_CASE("Input boundaries and controller IDs are checked before dispatch", "[CONTROL][security]") {
  using namespace pkts;
  // Captured Moonlight packet, independently of the C++ packet definitions.
  REQUIRE(valid_input_packet(crypto::hex_to_str("060212000000000E05000000033400C00000059F0329")));
  auto samples = {input_wire<MOUSE_MOVE_REL_PACKET>(MOUSE_MOVE_REL),
                  input_wire<MOUSE_MOVE_ABS_PACKET>(MOUSE_MOVE_ABS),
                  input_wire<MOUSE_BUTTON_PACKET>(MOUSE_BUTTON_PRESS),
                  input_wire<MOUSE_BUTTON_PACKET>(MOUSE_BUTTON_RELEASE),
                  input_wire<MOUSE_SCROLL_PACKET>(MOUSE_SCROLL),
                  input_wire<MOUSE_HSCROLL_PACKET>(MOUSE_HSCROLL),
                  input_wire<KEYBOARD_PACKET>(KEY_PRESS),
                  input_wire<KEYBOARD_PACKET>(KEY_RELEASE),
                  input_wire<TOUCH_PACKET>(TOUCH),
                  input_wire<PEN_PACKET>(PEN),
                  input_wire<CONTROLLER_MULTI_PACKET>(CONTROLLER_MULTI),
                  input_wire<CONTROLLER_ARRIVAL_PACKET>(CONTROLLER_ARRIVAL),
                  input_wire<CONTROLLER_TOUCH_PACKET>(CONTROLLER_TOUCH),
                  input_wire<CONTROLLER_MOTION_PACKET>(CONTROLLER_MOTION),
                  input_wire<CONTROLLER_BATTERY_PACKET>(CONTROLLER_BATTERY),
                  input_wire<HAPTICS_PACKET>(HAPTICS)};
  for (const auto &wire : samples) {
    REQUIRE(valid_input_packet(wire));
    REQUIRE_FALSE(valid_input_packet(wire + "x"));
    for (size_t n = 0; n < wire.size(); ++n) {
      auto truncated = wire.substr(0, n);
      REQUIRE_FALSE(valid_input_packet(truncated));
      if (n >= sizeof(INPUT_PKT)) {
        auto header = read_packet<INPUT_PKT>(truncated);
        header.packet_len = boost::endian::native_to_little(static_cast<uint16_t>(n - 4));
        header.data_size = boost::endian::native_to_big(static_cast<uint32_t>(n - 8));
        std::memcpy(truncated.data(), &header, sizeof(header));
        REQUIRE_FALSE(valid_input_packet(truncated)); // even internally consistent short frames
      }
    }
  }
  auto controller = input_wire<CONTROLLER_MULTI_PACKET>(CONTROLLER_MULTI);
  for (int16_t id : {0, 1, 15}) {
    auto packet = read_packet<CONTROLLER_MULTI_PACKET>(controller);
    packet.controller_number = boost::endian::native_to_little(id);
    std::memcpy(controller.data(), &packet, sizeof(packet));
    REQUIRE(valid_input_packet(controller));
  }
  for (int16_t id : {-32768, -1, 16, 31, 32, 32767}) {
    auto packet = read_packet<CONTROLLER_MULTI_PACKET>(controller);
    packet.controller_number = boost::endian::native_to_little(id);
    std::memcpy(controller.data(), &packet, sizeof(packet));
    REQUIRE_FALSE(valid_input_packet(controller));
  }
  for (auto wire : {input_wire<CONTROLLER_ARRIVAL_PACKET>(CONTROLLER_ARRIVAL),
                    input_wire<CONTROLLER_TOUCH_PACKET>(CONTROLLER_TOUCH),
                    input_wire<CONTROLLER_MOTION_PACKET>(CONTROLLER_MOTION),
                    input_wire<CONTROLLER_BATTERY_PACKET>(CONTROLLER_BATTERY)}) {
    for (unsigned char id : {16, 255}) {
      wire[sizeof(INPUT_PKT)] = static_cast<char>(id);
      REQUIRE_FALSE(valid_input_packet(wire));
    }
  }
  auto text = crypto::hex_to_str("06020C000000000817000000F09F92A9"); // U+1F4A9
  REQUIRE(valid_input_packet(text));
  text[4] = static_cast<char>(0xff); // underflow/overflow of claimed text length
  REQUIRE_FALSE(valid_input_packet(text));
  auto unknown = input_wire<INPUT_PKT>(static_cast<INPUT_TYPE>(0x7fffffff));
  REQUIRE_FALSE(valid_input_packet(unknown));
}

TEST_CASE("An authenticated payload still needs inner framing", "[CONTROL][security]") {
  const std::string key = "EDF04A215C4FBEA20934120C8480D855";
  for (const auto &plaintext : {std::string{}, std::string("x"), std::string("xx"), std::string("xxx")}) {
    auto encrypted = encrypt_packet(key, 0, plaintext, EncryptionMode::v2, Direction::client);
    auto decoded = decrypt_packet(to_string(*encrypted), key, EncryptionMode::v2, Direction::client);
    REQUIRE(decoded == plaintext);
    REQUIRE_FALSE(valid_control_packet(decoded));
    REQUIRE_FALSE(valid_input_packet(decoded));
  }
  auto claimed_input = crypto::hex_to_str("06020000"); // valid control header, missing input header
  auto encrypted = encrypt_packet(key, 0, claimed_input, EncryptionMode::v2, Direction::client);
  auto decoded = decrypt_packet(to_string(*encrypted), key, EncryptionMode::v2, Direction::client);
  REQUIRE(valid_control_packet(decoded));
  REQUIRE_FALSE(valid_input_packet(decoded));
}

TEST_CASE("Control feedback has canonical packed wire lengths", "[CONTROL][security]") {
  ControlRumblePacket rumble{
      .header = {.type = pkts::RUMBLE_DATA, .length = boost::endian::native_to_little<uint16_t>(10)},
      .controller_number = boost::endian::native_to_little<uint16_t>(1),
      .low_freq = boost::endian::native_to_little<uint16_t>(0x1234),
      .high_freq = boost::endian::native_to_little<uint16_t>(0x5678)};
  REQUIRE(crypto::str_to_hex({reinterpret_cast<const char *>(&rumble), sizeof(rumble)}) ==
          "0B010A0000000000010034127856");
  ControlMotionEventPacket motion{
      .header = {.type = pkts::MOTION_EVENT, .length = boost::endian::native_to_little<uint16_t>(5)},
      .controller_number = boost::endian::native_to_little<uint16_t>(1),
      .reportrate = boost::endian::native_to_little<uint16_t>(100),
      .type = 2};
  REQUIRE(crypto::str_to_hex({reinterpret_cast<const char *>(&motion), sizeof(motion)}) == "015505000100640002");
}
