#pragma once

#include <control/control.hpp>
#include <events/events.hpp>
#include <moonlight/control.hpp>
#include <type_traits>

namespace control {

using namespace moonlight::control::pkts;
using namespace wolf::core;

/**
 * Side effect: session devices might be updated when hotplugging
 */
bool handle_input(events::StreamSession &session,
                  immer::box<std::shared_ptr<ControlPeer>> connected_client,
                  std::string_view bytes);

namespace detail {
// The common factory uses this selector (including legacy override behavior).
wolf::config::ControllerType selected_joypad_type(const events::StreamSession &session,
                                                  int controller_number,
                                                  moonlight::control::pkts::CONTROLLER_TYPE requested_type,
                                                  uint8_t capabilities);

// Internal synchronous IO boundary, never stored. Production supplies session
// lookup + the common Xbox factory + real setters. Tests inject IO only, after
// the SAME dispatcher/policy checks. This is not a peer authentication provider.
struct TrialPadIO {
  std::function<bool()> compatible;
  std::function<bool(int)> present;
  std::function<bool(int)> create_xbox;
  std::function<bool(int, const CONTROLLER_MULTI_PACKET &)> apply;
};
// No shortcuts/feedback here; mask removal uses the same setters with zeroes.
// Setters are legacy void operations: success is NOT checked kernel delivery.
template <class Pad> void apply_trial_frame(Pad &pad, const CONTROLLER_MULTI_PACKET &pkt) {
  const auto buttons = uint32_t(uint16_t(boost::endian::little_to_native(pkt.button_flags))) |
                       (uint32_t(uint16_t(boost::endian::little_to_native(pkt.buttonFlags2))) << 16);
  pad.set_pressed_buttons(buttons);
  pad.set_stick(inputtino::Joypad::LS,
                boost::endian::little_to_native(pkt.left_stick_x),
                boost::endian::little_to_native(pkt.left_stick_y));
  pad.set_stick(inputtino::Joypad::RS,
                boost::endian::little_to_native(pkt.right_stick_x),
                boost::endian::little_to_native(pkt.right_stick_y));
  pad.set_triggers(pkt.left_trigger, pkt.right_trigger);
}
} // namespace detail

// Internal injected IO overload; normal callers keep the original signature.
// Non-null ControlPeer is necessary, not sufficient proof of current authority:
// the actual ENet loop authenticates/replay-checks/current-checks before calling.
bool handle_input(events::StreamSession &session,
                  immer::box<std::shared_ptr<ControlPeer>> connected_client,
                  std::string_view bytes,
                  const detail::TrialPadIO &trial_io);

// Local callers with a complete typed object (including device tests). A bare
// INPUT_PKT pointer is deliberately not accepted: it carries no buffer bound.
template <typename Packet>
requires(std::is_base_of_v<INPUT_PKT, Packet> && !std::is_same_v<INPUT_PKT, Packet>)
bool handle_input(events::StreamSession &session,
                  immer::box<std::shared_ptr<ControlPeer>> connected_client,
                  const Packet *packet) {
  if (!packet)
    return false;
  auto owned = *packet;
  size_t size = sizeof(Packet);
  if constexpr (std::is_same_v<Packet, UTF8_TEXT_PACKET>) {
    auto data_size = boost::endian::big_to_native(owned.data_size);
    if (data_size <= sizeof(INPUT_TYPE) || data_size > sizeof(INPUT_TYPE) + UTF8_TEXT_MAX_LEN)
      return false;
    size = data_size + 8;
  }
  owned.packet_type = INPUT_DATA;
  owned.packet_len = boost::endian::native_to_little(static_cast<uint16_t>(size - 4));
  owned.data_size = boost::endian::native_to_big(static_cast<uint32_t>(size - 8));
  return handle_input(session, connected_client, {reinterpret_cast<const char *>(&owned), size});
}

void mouse_move_rel(const MOUSE_MOVE_REL_PACKET &pkt, events::StreamSession &session);

void mouse_move_abs(const MOUSE_MOVE_ABS_PACKET &pkt, events::StreamSession &session);

void mouse_button(const MOUSE_BUTTON_PACKET &pkt, events::StreamSession &session);

void mouse_scroll(const MOUSE_SCROLL_PACKET &pkt, events::StreamSession &session);

void mouse_h_scroll(const MOUSE_HSCROLL_PACKET &pkt, events::StreamSession &session);

void keyboard_key(const KEYBOARD_PACKET &pkt, events::StreamSession &session);

void utf8_text(const UTF8_TEXT_PACKET &pkt, events::StreamSession &session);

void touch(const TOUCH_PACKET &pkt, events::StreamSession &session);

void pen(const PEN_PACKET &pkt, events::StreamSession &session);

void controller_arrival(const CONTROLLER_ARRIVAL_PACKET &pkt,
                        events::StreamSession &session,
                        immer::box<std::shared_ptr<ControlPeer>> connected_client);

void controller_multi(const CONTROLLER_MULTI_PACKET &pkt,
                      events::StreamSession &session,
                      immer::box<std::shared_ptr<ControlPeer>> connected_client);

void controller_touch(const CONTROLLER_TOUCH_PACKET &pkt, events::StreamSession &session);

void controller_motion(const CONTROLLER_MOTION_PACKET &pkt, events::StreamSession &session);

void controller_battery(const CONTROLLER_BATTERY_PACKET &pkt, events::StreamSession &session);

} // namespace control
