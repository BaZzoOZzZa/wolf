#pragma once
#include <algorithm>
#include <array>
#include <boost/endian/conversion.hpp>
#include <cmath>
#include <crypto/crypto.hpp>
#include <cstdint>
#include <cstring>
#include <inputtino/input.hpp>
#include <memory>
#include <moonlight/control_crypto.hpp>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace moonlight::control {

namespace pkts {

using netfloat = uint8_t[4]; // little-endian IEEE-754 bytes on the wire

enum PACKET_TYPE : std::uint16_t {
  START_A = boost::endian::little_to_native(0x0305),
  START_B = boost::endian::little_to_native(0x0307),
  INVALIDATE_REF_FRAMES = boost::endian::little_to_native(0x0301),
  LOSS_STATS = boost::endian::little_to_native(0x0201),
  FRAME_STATS = boost::endian::little_to_native(0x0204),
  INPUT_DATA = boost::endian::little_to_native(0x0206),
  TERMINATION = boost::endian::little_to_native(0x0109),
  PERIODIC_PING = boost::endian::little_to_native(0x0200),
  IDR_FRAME = boost::endian::little_to_native(0x0302),
  ENCRYPTED = boost::endian::little_to_native(0x0001),
  HDR_MODE = boost::endian::little_to_native(0x010e),
  RUMBLE_DATA = boost::endian::little_to_native(0x010b),
  RUMBLE_TRIGGERS = boost::endian::little_to_native(0x5500),
  MOTION_EVENT = boost::endian::little_to_native(0x5501),
  RGB_LED_EVENT = boost::endian::little_to_native(0x5502),
  ADAPTIVE_TRIGGER_EVENT = boost::endian::little_to_native(0x5503),
};

enum INPUT_TYPE : int {
  MOUSE_MOVE_REL = boost::endian::native_to_little(0x00000007),
  MOUSE_MOVE_ABS = boost::endian::native_to_little(0x00000005),
  MOUSE_BUTTON_PRESS = boost::endian::native_to_little(0x00000008),
  MOUSE_BUTTON_RELEASE = boost::endian::native_to_little(0x00000009),
  KEY_PRESS = boost::endian::native_to_little(0x00000003),
  KEY_RELEASE = boost::endian::native_to_little(0x00000004),
  MOUSE_SCROLL = boost::endian::native_to_little(0x0000000A),
  MOUSE_HSCROLL = boost::endian::native_to_little(0x55000001),
  TOUCH = boost::endian::native_to_little(0x55000002),
  PEN = boost::endian::native_to_little(0x55000003),
  CONTROLLER_MULTI = boost::endian::native_to_little(0x0000000C),
  CONTROLLER_ARRIVAL = boost::endian::native_to_little(0x55000004),
  CONTROLLER_TOUCH = boost::endian::native_to_little(0x55000005),
  CONTROLLER_MOTION = boost::endian::native_to_little(0x55000006),
  CONTROLLER_BATTERY = boost::endian::native_to_little(0x55000007),
  HAPTICS = boost::endian::native_to_little(0x0000000D),
  UTF8_TEXT = boost::endian::native_to_little(0x00000017),
};

enum CONTROLLER_TYPE : uint8_t {
  UNKNOWN = 0x00,
  XBOX = 0x01,
  PS = 0x02,
  NINTENDO = 0x03,
  AUTO = 0xFF // not part of the protocol, I've added it for simplicity
};

enum CONTROLLER_CAPABILITIES : uint8_t {
  ANALOG_TRIGGERS = 0x01,
  RUMBLE = 0x02,
  TRIGGER_RUMBLE = 0x04,
  TOUCHPAD = 0x08,
  ACCELEROMETER = 0x10,
  GYRO = 0x20,
  BATTERY = 0x40,
  RGB_LED = 0x80
};

enum CONTROLLER_BTN : unsigned int {
  DPAD_UP = 0x0001,
  DPAD_DOWN = 0x0002,
  DPAD_LEFT = 0x0004,
  DPAD_RIGHT = 0x0008,

  START = 0x0010,
  BACK = 0x0020,
  HOME = 0x0400,

  LEFT_STICK = 0x0040,
  RIGHT_STICK = 0x0080,
  LEFT_BUTTON = 0x0100,
  RIGHT_BUTTON = 0x0200,

  SPECIAL_FLAG = 0x0400,
  PADDLE1_FLAG = 0x010000,
  PADDLE2_FLAG = 0x020000,
  PADDLE3_FLAG = 0x040000,
  PADDLE4_FLAG = 0x080000,
  TOUCHPAD_FLAG = 0x100000, // Touchpad buttons on Sony controllers
  MISC_FLAG = 0x200000,     // Share/Mic/Capture/Mute buttons on various controllers

  /* This follows the XBOX controller layout */
  A = 0x1000,
  B = 0x2000,
  X = 0x4000,
  Y = 0x8000
};

enum KEYBOARD_MODIFIERS : char {
  NONE = 0x00,
  SHIFT = 0x01,
  CTRL = 0x02,
  ALT = 0x04,
  META = 0x08
};

enum MOONLIGHT_MODIFIERS : short {
  M_SHIFT = 0x10,
  M_CTRL = 0x11,
  M_MENU = 0x12,
  M_ALT = 0xA4,
  M_META = 0x5B,
  M_LSHIFT = 0xA0,
  M_RSHIFT = 0xA1,
  M_LCTRL = 0xA2,
  M_RCTRL = 0xA3,
  M_RALT = 0xA5,
  M_RMETA = 0x5C
};

// make sure these structs are allocated in 1-byte blocks so the data aligns
// right
#pragma pack(push, 1)

struct INPUT_PKT {
  unsigned short packet_type; // This should always be 0x0206 little endian (INPUT_DATA)
  unsigned short packet_len;  // the total size of the packet

  unsigned int data_size; // the size of the input data

  INPUT_TYPE type;
};

struct MOUSE_MOVE_REL_PACKET : INPUT_PKT {
  short delta_x;
  short delta_y;
};

struct MOUSE_MOVE_ABS_PACKET : INPUT_PKT {
  short x;
  short y;
  short unused;
  short width;
  short height;
};

struct MOUSE_BUTTON_PACKET : INPUT_PKT {
  unsigned char button;
};

struct MOUSE_SCROLL_PACKET : INPUT_PKT {
  short scroll_amt1;
  short scroll_amt2;
  short zero1;
};

struct MOUSE_HSCROLL_PACKET : INPUT_PKT {
  short scroll_amount;
};

struct KEYBOARD_PACKET : INPUT_PKT {
  unsigned char flags;
  short key_code;
  char modifiers;
  short zero1;
};

// this is the same size moonlight uses
constexpr int UTF8_TEXT_MAX_LEN = 32;
struct UTF8_TEXT_PACKET : INPUT_PKT {
  char text[UTF8_TEXT_MAX_LEN];
};

struct CONTROLLER_MULTI_PACKET : INPUT_PKT {
  short header_b;
  short controller_number;
  /* A bitfield with bits set for each controller present. */
  short active_gamepad_mask;
  short mid_b;
  short button_flags;
  unsigned char left_trigger;
  unsigned char right_trigger;
  short left_stick_x;
  short left_stick_y;
  short right_stick_x;
  short right_stick_y;
  short tail_a;
  short buttonFlags2; // Sunshine protocol extension (always 0 for GFE)
  short tailB;
};

struct HAPTICS_PACKET : INPUT_PKT {
  uint16_t enable;
};

enum TOUCH_EVENT_TYPE : uint8_t {
  TOUCH_EVENT_HOVER = 0x00,
  TOUCH_EVENT_DOWN = 0x01,
  TOUCH_EVENT_UP = 0x02,
  TOUCH_EVENT_MOVE = 0x03,
  TOUCH_EVENT_CANCEL = 0x04,
  TOUCH_EVENT_BUTTON_ONLY = 0x05,
  TOUCH_EVENT_HOVER_LEAVE = 0x06,
  TOUCH_EVENT_CANCEL_ALL = 0x07
};

struct TOUCH_PACKET : INPUT_PKT {
  TOUCH_EVENT_TYPE event_type;
  uint8_t zero[1]; // Alignment/reserved
  uint16_t rotation;
  uint32_t pointer_id;
  netfloat x;
  netfloat y;
  netfloat pressure_or_distance;
  netfloat contact_area_major;
  netfloat contact_area_minor;
};

enum TOOL_TYPE : uint8_t {
  TOOL_TYPE_UNKNOWN = 0x00,
  TOOL_TYPE_PEN = 0x01,
  TOOL_TYPE_ERASER = 0x02,
};

enum PEN_BUTTON_TYPE : uint8_t {
  PEN_BUTTON_TYPE_PRIMARY = 0x01,
  PEN_BUTTON_TYPE_SECONDARY = 0x02,
  PEN_BUTTON_TYPE_TERTIARY = 0x04
};

static constexpr uint8_t PEN_TILT_UNKNOWN = 0xFF;
static constexpr uint16_t PEN_ROTATION_UNKNOWN = 0xFFFF;

struct PEN_PACKET : INPUT_PKT {
  TOUCH_EVENT_TYPE event_type;
  TOOL_TYPE tool_type;
  uint8_t pen_buttons;
  uint8_t zero[1]; // Alignment/reserved
  netfloat x;
  netfloat y;
  netfloat pressure_or_distance;
  uint16_t rotation;
  uint8_t tilt;
  uint8_t zero2[1];
  netfloat contact_area_major;
  netfloat contact_area_minor;
};

struct CONTROLLER_ARRIVAL_PACKET : INPUT_PKT {
  uint8_t controller_number;
  CONTROLLER_TYPE controller_type;
  uint8_t capabilities; // see: CONTROLLER_CAPABILITIES
  uint32_t support_button_flags;
};

struct CONTROLLER_TOUCH_PACKET : INPUT_PKT {
  uint8_t controller_number;
  TOUCH_EVENT_TYPE event_type;
  uint8_t zero[2]; // Alignment/reserved
  uint32_t pointer_id;
  netfloat x;
  netfloat y;
  netfloat pressure;
};

enum MOTION_TYPE : uint8_t {
  ACCELERATION = 0x01,
  GYROSCOPE = 0x02
};

constexpr uint8_t BATTERY_PERCENTAGE_UNKNOWN = 0xFF;

// One byte on the wire -- must stay uint8_t, otherwise it eats the following battery_percentage
// byte in CONTROLLER_BATTERY_PACKET and the percentage is always read as the trailing zero (0%).
enum BATTERY_STATE : uint8_t {
  BATTERY_STATE_UNKNOWN = 0x0,
  BATTERY_NOT_PRESENT = 0x1,
  BATTERY_DISCHARGHING = 0x2,
  BATTERY_CHARGING = 0x3,
  BATTERY_NOT_CHARGING = 0x4,
  BATTERY_FULL = 0x05
};

struct CONTROLLER_MOTION_PACKET : INPUT_PKT {
  uint8_t controller_number;
  MOTION_TYPE motion_type;
  uint8_t zero[2]; // Alignment/reserved
  netfloat x;
  netfloat y;
  netfloat z;
};

struct CONTROLLER_BATTERY_PACKET : INPUT_PKT {
  uint8_t controller_number;
  BATTERY_STATE battery_state;
  uint8_t battery_percentage;
  uint8_t zero[1]; // Alignment/reserved
};

#pragma pack(pop)

} // namespace pkts

static constexpr int GCM_TAG_SIZE = 16;
static constexpr int MAX_PAYLOAD_SIZE = 128;
static constexpr std::uint32_t TERMINATE_REASON_GRACEFULL = boost::endian::native_to_big(0x80030023);

// Control replies are wire objects too: never transmit native tail padding.
#pragma pack(push, 1)
struct ControlPacket {
  pkts::PACKET_TYPE type;
  std::uint16_t length; // The length of the REST of the packet, EXCLUDING size of type and length
};

struct ControlTerminatePacket {
  ControlPacket header = {.type = pkts::TERMINATION, .length = sizeof(std::uint32_t)};
  std::uint32_t reason = TERMINATE_REASON_GRACEFULL;
};

struct ControlRumblePacket {
  ControlPacket header;

  std::uint32_t useless;

  std::uint16_t controller_number;
  std::uint16_t low_freq;
  std::uint16_t high_freq;
};

struct ControlRumbleTriggerPacket {
  ControlPacket header;

  std::uint16_t controller_number;
  std::uint16_t left;
  std::uint16_t right;
};

struct ControlMotionEventPacket {
  ControlPacket header;

  std::uint16_t controller_number;
  std::uint16_t reportrate;
  std::uint8_t type;
};

struct ControlRGBLedPacket {
  ControlPacket header;

  std::uint16_t controller_number;
  std::uint8_t r;
  std::uint8_t g;
  std::uint8_t b;
};

static constexpr std::uint8_t DS_EFFECT_PAYLOAD_SIZE = 10;

struct ControlAdaptiveTriggerPacket {
  ControlPacket header;

  std::uint16_t controller_number;
  inputtino::PS5Joypad::TriggerEffect effect;
};

struct ControlEncryptedPacket {
  ControlPacket header; // Always 0x0001 (see PACKET_TYPE ENCRYPTED)
  std::uint32_t seq;    // Monotonically increasing sequence number (used as IV for AES-GCM)

  /**
   * First 16 bytes are the AES GCM TAG
   */
  char gcm_tag[GCM_TAG_SIZE];

  /**
   * Rest of the bytes are the encrypted message
   */
  char payload[MAX_PAYLOAD_SIZE]; // TODO: this should be a char* with a variable size based on header

  /**
   * Helper function to get the payload as a string with the right size
   */
  [[nodiscard]] std::string_view encrypted_msg() const {
    auto len = boost::endian::little_to_native(this->header.length);
    if (len < GCM_TAG_SIZE + sizeof(seq) || len > GCM_TAG_SIZE + sizeof(seq) + sizeof(payload))
      throw std::runtime_error("Invalid encrypted payload length");
    return {payload, static_cast<size_t>(len - GCM_TAG_SIZE - sizeof(seq))};
  }

  [[nodiscard]] size_t full_size() const {
    return boost::endian::little_to_native(this->header.length) + sizeof(ControlPacket);
  }
};

#pragma pack(pop)

// Read wire objects by value. Network buffers need not be aligned and do not
// contain live instances of the C++ derived input structs.
template <typename T> T read_packet(std::string_view bytes) {
  static_assert(std::is_trivially_copyable_v<T>);
  if (bytes.size() < sizeof(T))
    throw std::runtime_error("Truncated control packet");
  T packet{};
  std::memcpy(&packet, bytes.data(), sizeof(T));
  return packet;
}

inline bool valid_control_packet(std::string_view bytes) {
  return bytes.size() >= sizeof(ControlPacket) &&
         boost::endian::little_to_native(read_packet<ControlPacket>(bytes).length) ==
             bytes.size() - sizeof(ControlPacket);
}

// The protocol mask has 16 bits. Bound every input path (also Unix API)
// before indexing or shifting; a product-specific one-pad policy belongs above this layer.
inline constexpr unsigned int CONTROL_SLOT_COUNT = 16;

// Typed entry points use the same field checks as the wire validator. Decode
// before arithmetic; clamp finite normalized values at the device boundary.
inline float input_float(const pkts::netfloat &value) {
  return boost::endian::endian_load<float, sizeof(float), boost::endian::order::little>(value);
}

inline bool valid_input_fields(const pkts::TOUCH_PACKET &pkt) {
  return pkt.event_type <= pkts::TOUCH_EVENT_CANCEL_ALL && std::isfinite(input_float(pkt.x)) &&
         std::isfinite(input_float(pkt.y)) && std::isfinite(input_float(pkt.pressure_or_distance)) &&
         std::isfinite(input_float(pkt.contact_area_major)) && std::isfinite(input_float(pkt.contact_area_minor));
}

inline bool valid_input_fields(const pkts::PEN_PACKET &pkt) {
  return pkt.event_type <= pkts::TOUCH_EVENT_CANCEL_ALL && pkt.tool_type <= pkts::TOOL_TYPE_ERASER &&
         std::isfinite(input_float(pkt.x)) && std::isfinite(input_float(pkt.y)) &&
         std::isfinite(input_float(pkt.pressure_or_distance)) && std::isfinite(input_float(pkt.contact_area_major)) &&
         std::isfinite(input_float(pkt.contact_area_minor));
}

inline bool valid_input_fields(const pkts::CONTROLLER_TOUCH_PACKET &pkt) {
  // This implementation maps wire IDs directly to the two PS5 touchpad slots.
  // Check the uint32 value BEFORE converting to inputtino's signed finger index.
  // Screen-touch IDs have a different mapping and are not restricted here.
  return pkt.controller_number < CONTROL_SLOT_COUNT && pkt.event_type <= pkts::TOUCH_EVENT_CANCEL_ALL &&
         boost::endian::little_to_native(pkt.pointer_id) < 2 && std::isfinite(input_float(pkt.x)) &&
         std::isfinite(input_float(pkt.y)) && std::isfinite(input_float(pkt.pressure));
}

inline bool valid_input_fields(const pkts::CONTROLLER_MOTION_PACKET &pkt) {
  return pkt.controller_number < CONTROL_SLOT_COUNT &&
         (pkt.motion_type == pkts::ACCELERATION || pkt.motion_type == pkts::GYROSCOPE) &&
         std::isfinite(input_float(pkt.x)) && std::isfinite(input_float(pkt.y)) && std::isfinite(input_float(pkt.z));
}

inline bool valid_input_fields(const pkts::CONTROLLER_BATTERY_PACKET &pkt) {
  return pkt.controller_number < CONTROL_SLOT_COUNT && pkt.battery_state <= pkts::BATTERY_FULL &&
         (pkt.battery_percentage <= 100 || pkt.battery_percentage == pkts::BATTERY_PERCENTAGE_UNKNOWN);
}

inline bool valid_input_packet(std::string_view bytes) {
  using namespace pkts;
  if (!valid_control_packet(bytes) || bytes.size() < sizeof(INPUT_PKT))
    return false;
  auto header = read_packet<INPUT_PKT>(bytes);
  if (header.packet_type != INPUT_DATA || boost::endian::big_to_native(header.data_size) != bytes.size() - 8)
    return false;

  switch (header.type) {
  case MOUSE_MOVE_REL:
    return bytes.size() == sizeof(MOUSE_MOVE_REL_PACKET);
  case MOUSE_MOVE_ABS:
    return bytes.size() == sizeof(MOUSE_MOVE_ABS_PACKET) &&
           boost::endian::big_to_native(read_packet<MOUSE_MOVE_ABS_PACKET>(bytes).width) > 0 &&
           boost::endian::big_to_native(read_packet<MOUSE_MOVE_ABS_PACKET>(bytes).height) > 0;
  case MOUSE_BUTTON_PRESS:
  case MOUSE_BUTTON_RELEASE:
    return bytes.size() == sizeof(MOUSE_BUTTON_PACKET);
  case MOUSE_SCROLL:
    return bytes.size() == sizeof(MOUSE_SCROLL_PACKET);
  case MOUSE_HSCROLL:
    return bytes.size() == sizeof(MOUSE_HSCROLL_PACKET);
  case KEY_PRESS:
  case KEY_RELEASE:
    return bytes.size() == sizeof(KEYBOARD_PACKET);
  case UTF8_TEXT:
    return bytes.size() > sizeof(INPUT_PKT) && bytes.size() <= sizeof(UTF8_TEXT_PACKET);
  case TOUCH:
    return bytes.size() == sizeof(TOUCH_PACKET) && valid_input_fields(read_packet<TOUCH_PACKET>(bytes));
  case PEN:
    return bytes.size() == sizeof(PEN_PACKET) && valid_input_fields(read_packet<PEN_PACKET>(bytes));
  case CONTROLLER_MULTI:
    if (bytes.size() != sizeof(CONTROLLER_MULTI_PACKET))
      return false;
    return boost::endian::little_to_native(read_packet<CONTROLLER_MULTI_PACKET>(bytes).controller_number) >= 0 &&
           boost::endian::little_to_native(read_packet<CONTROLLER_MULTI_PACKET>(bytes).controller_number) <
               CONTROL_SLOT_COUNT;
  case CONTROLLER_ARRIVAL:
    return bytes.size() == sizeof(CONTROLLER_ARRIVAL_PACKET) &&
           read_packet<CONTROLLER_ARRIVAL_PACKET>(bytes).controller_number < CONTROL_SLOT_COUNT;
  case CONTROLLER_TOUCH:
    return bytes.size() == sizeof(CONTROLLER_TOUCH_PACKET) &&
           valid_input_fields(read_packet<CONTROLLER_TOUCH_PACKET>(bytes));
  case CONTROLLER_MOTION:
    return bytes.size() == sizeof(CONTROLLER_MOTION_PACKET) &&
           valid_input_fields(read_packet<CONTROLLER_MOTION_PACKET>(bytes));
  case CONTROLLER_BATTERY:
    return bytes.size() == sizeof(CONTROLLER_BATTERY_PACKET) &&
           valid_input_fields(read_packet<CONTROLLER_BATTERY_PACKET>(bytes));
  case HAPTICS:
    return bytes.size() == sizeof(HAPTICS_PACKET);
  default:
    return false;
  }
}

/** A bounded legacy Moonlight envelope. The tag is binary, never a C string. */
inline std::string decrypt_packet(std::string_view bytes,
                                  std::string_view gcm_key,
                                  EncryptionMode mode = EncryptionMode::legacy,
                                  Direction direction = Direction::client) {
  constexpr size_t prefix_size = sizeof(ControlPacket) + sizeof(std::uint32_t) + GCM_TAG_SIZE;
  if (!valid_control_packet(bytes) || bytes.size() < prefix_size ||
      read_packet<ControlPacket>(bytes).type != pkts::ENCRYPTED)
    throw std::runtime_error("Invalid encrypted control envelope");

  auto seq = read_packet<std::uint32_t>(bytes.substr(sizeof(ControlPacket)));
  auto iv = control_iv(boost::endian::little_to_native(seq), mode, direction);
  if (iv.empty())
    throw std::runtime_error("Control encryption not negotiated");
  auto key = crypto::hex_to_str(gcm_key, true);
  if (key.size() != 16)
    throw std::runtime_error("Invalid control key length");
  return crypto::aes_decrypt_gcm(bytes.substr(prefix_size),
                                 key,
                                 bytes.substr(sizeof(ControlPacket) + sizeof(seq), GCM_TAG_SIZE),
                                 iv,
                                 static_cast<int>(iv.size()));
}

// Only for an owned, complete object (e.g. locally produced test vectors).
inline std::string decrypt_packet(const ControlEncryptedPacket &packet, std::string_view key) {
  if (packet.full_size() > sizeof(packet))
    throw std::runtime_error("Oversized encrypted control packet");
  return decrypt_packet({reinterpret_cast<const char *>(&packet), packet.full_size()}, key);
}

/**
 * Turns a payload into a properly formatted control encrypted packet
 */
static std::unique_ptr<ControlEncryptedPacket> encrypt_packet(std::string_view gcm_key,
                                                              std::uint32_t seq,
                                                              std::string_view payload,
                                                              EncryptionMode mode = EncryptionMode::legacy,
                                                              Direction direction = Direction::client) {
  if (payload.size() > MAX_PAYLOAD_SIZE)
    throw std::runtime_error("Control payload exceeds packet capacity");
  auto key = crypto::hex_to_str(gcm_key, true);
  if (key.size() != 16)
    throw std::runtime_error("Invalid control key length");
  auto iv = control_iv(seq, mode, direction);
  if (iv.empty())
    throw std::runtime_error("Control encryption not negotiated");

  auto [encrypted_str, gcm_tag] = crypto::aes_encrypt_gcm(payload, key, iv, static_cast<int>(iv.size()));

  std::uint16_t size = sizeof(seq) + GCM_TAG_SIZE + encrypted_str.length();
  ControlEncryptedPacket encrypted_pkt = {
      .header = {.type = pkts::ENCRYPTED, .length = boost::endian::native_to_little(size)},
      .seq = boost::endian::native_to_little(seq)};

  std::copy(gcm_tag.begin(), gcm_tag.end(), encrypted_pkt.gcm_tag);
  std::copy(encrypted_str.begin(), encrypted_str.end(), encrypted_pkt.payload);

  return std::make_unique<ControlEncryptedPacket>(encrypted_pkt);
}

static constexpr const char *packet_type_to_str(pkts::PACKET_TYPE p) noexcept {
  switch (p) {
  case pkts::START_A:
    return "START_A";
  case pkts::START_B:
    return "START_B";
  case pkts::INVALIDATE_REF_FRAMES:
    return "INVALIDATE_REF_FRAMES";
  case pkts::LOSS_STATS:
    return "LOSS_STATS";
  case pkts::FRAME_STATS:
    return "FRAME_STATS";
  case pkts::INPUT_DATA:
    return "INPUT_DATA";
  case pkts::RUMBLE_DATA:
    return "RUMBLE_DATA";
  case pkts::TERMINATION:
    return "TERMINATION";
  case pkts::PERIODIC_PING:
    return "PERIODIC_PING";
  case pkts::IDR_FRAME:
    return "IDR_FRAME";
  case pkts::ENCRYPTED:
    return "ENCRYPTED";
  case pkts::HDR_MODE:
    return "HDR_MODE";
  case pkts::RUMBLE_TRIGGERS:
    return "RUMBLE_TRIGGERS";
  case pkts::MOTION_EVENT:
    return "MOTION_EVENT";
  case pkts::RGB_LED_EVENT:
    return "RGB_LED_EVENT";
  case pkts::ADAPTIVE_TRIGGER_EVENT:
    return "ADAPTIVE_TRIGGER_EVENT";
  }
  return "Unrecognised";
}

} // namespace moonlight::control