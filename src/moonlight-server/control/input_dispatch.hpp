#pragma once

#include <limits>
#include <moonlight/control.hpp>
#include <numbers>

namespace control {

// Small device-boundary seams: production calls these with a PS5Joypad;
// isolated tests supply a recording sink without opening uinput/uhid.
inline float normalized_input(const moonlight::control::pkts::netfloat &value) {
  return std::clamp(moonlight::control::input_float(value), 0.0f, 1.0f);
}

template <typename Pad>
bool dispatch_controller_touch(const moonlight::control::pkts::CONTROLLER_TOUCH_PACKET &pkt, Pad &pad) {
  using namespace moonlight::control;
  using namespace pkts;
  if (!valid_input_fields(pkt))
    return false;
  auto pointer = static_cast<int>(boost::endian::little_to_native(pkt.pointer_id));
  switch (pkt.event_type) {
  case TOUCH_EVENT_DOWN:
  case TOUCH_EVENT_HOVER:
  case TOUCH_EVENT_MOVE:
    pad.place_finger(pointer,
                     static_cast<uint16_t>(normalized_input(pkt.x) * inputtino::PS5Joypad::touchpad_width),
                     static_cast<uint16_t>(normalized_input(pkt.y) * inputtino::PS5Joypad::touchpad_height));
    break;
  case TOUCH_EVENT_UP:
  case TOUCH_EVENT_HOVER_LEAVE:
  case TOUCH_EVENT_CANCEL:
    pad.release_finger(pointer);
    break;
  case TOUCH_EVENT_CANCEL_ALL:
  case TOUCH_EVENT_BUTTON_ONLY:
    // Preserve the existing unsupported-event behaviour (no device operation).
    break;
  }
  return true;
}

template <typename Pad>
bool dispatch_controller_motion(const moonlight::control::pkts::CONTROLLER_MOTION_PACKET &pkt, Pad &pad) {
  using namespace moonlight::control;
  using namespace pkts;
  if (!valid_input_fields(pkt))
    return false;
  const bool acceleration = pkt.motion_type == ACCELERATION;
  // Convert in double before narrowing. The patched inputtino encoder scales
  // in double and saturates to signed int16 before endian conversion.
  auto axis = [&](const netfloat &value) {
    const double decoded = input_float(value);
    return static_cast<float>(acceleration ? decoded : decoded * (std::numbers::pi / 180.0));
  };
  pad.set_motion(acceleration ? inputtino::PS5Joypad::ACCELERATION : inputtino::PS5Joypad::GYROSCOPE,
                 axis(pkt.x),
                 axis(pkt.y),
                 axis(pkt.z));
  return true;
}

template <typename Pad>
bool dispatch_controller_battery(const moonlight::control::pkts::CONTROLLER_BATTERY_PACKET &pkt, Pad &pad) {
  using namespace moonlight::control;
  using namespace pkts;
  if (!valid_input_fields(pkt))
    return false;
  inputtino::PS5Joypad::BATTERY_STATE state;
  switch (pkt.battery_state) {
  case BATTERY_STATE_UNKNOWN:
  case BATTERY_NOT_PRESENT:
    return true; // Valid report, but no representable device state.
  case BATTERY_DISCHARGHING:
    state = inputtino::PS5Joypad::BATTERY_DISCHARGING;
    break;
  case BATTERY_CHARGING:
    state = inputtino::PS5Joypad::BATTERY_CHARGHING;
    break;
  case BATTERY_NOT_CHARGING:
    state = inputtino::PS5Joypad::CHARGHING_ERROR;
    break;
  case BATTERY_FULL:
    state = inputtino::PS5Joypad::BATTERY_FULL;
    break;
  default:
    return false; // Fail closed even if the outer wire check is bypassed.
  }
  if (pkt.battery_percentage != BATTERY_PERCENTAGE_UNKNOWN)
    pad.set_battery(state, pkt.battery_percentage);
  return true;
}

} // namespace control
