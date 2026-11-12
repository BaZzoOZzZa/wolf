#pragma once
#include <atomic>
#include <bitset>
#include <control/control.hpp>
#include <moonlight/control.hpp>
#include <optional>
#include <state/serialised_config.hpp>
#include <thread>
#include <vector>

namespace control::direct {
using Spec = wolf::config::DirectWindowTarget;
struct Geometry {
  uint32_t root;
  int32_t x, y;
  uint32_t width, height;
};
struct Attributes {
  bool viewable, input_output;
};
struct Tree {
  uint32_t root, parent;
};
struct Pointer {
  uint32_t child;
  int32_t x, y;
};
struct Event {
  enum Kind {
    none,
    changed,
    destroyed,
    error
  } kind;
  uint32_t window = 0;
};
enum class Property {
  pid,
  active_window
};
struct Keymap {
  uint8_t first = 0, last = 0, columns = 0, group = 0, mods = 0, latched = 0, locked = 0;
  int16_t latched_group = 0;
  std::vector<uint32_t> symbols;
  std::bitset<256> shift_codes;
  bool operator==(const Keymap &) const = default;
};
// Primitive synchronous XCB/proc providers only. No policy/permission callbacks.
// Native connection is private; no Xlib/global error handler or extra worker.
struct Operations {
  std::function<std::shared_ptr<void>()> connect;
  std::function<std::optional<uint32_t>(void *, uint32_t)> screen_root;
  std::function<bool(void *, uint32_t)> subscribe;
  std::function<std::optional<Attributes>(void *, uint32_t)> attributes;
  std::function<std::optional<Geometry>(void *, uint32_t)> geometry;
  std::function<std::optional<Pointer>(void *, uint32_t, uint32_t)> translate;
  std::function<std::optional<uint32_t>(void *, uint32_t, Property)> property;
  std::function<std::optional<uint64_t>(uint32_t)> start_time;
  std::function<std::optional<Tree>(void *, uint32_t)> tree;
  std::function<std::optional<Pointer>(void *, uint32_t)> pointer;
  std::function<Event(void *)> poll;
  std::function<bool(void *, uint32_t, int16_t, int16_t)> motion;
  std::function<bool(void *, uint8_t, bool)> button;
  std::function<bool(void *, uint8_t, bool)> key;
  std::function<std::pair<uint8_t, uint8_t>(void *)> key_range;
  std::function<std::optional<Keymap>(void *)> keymap;
  static Operations defaults();
};
class WindowInput {
  Spec spec_;
  Operations ops_;
  std::shared_ptr<void> connection_;
  std::atomic_flag busy_ = ATOMIC_FLAG_INIT;
  std::thread::id owner_;
  bool failed_ = false;
  const bool keyboard_mouse_;
  std::bitset<16> buttons_;
  std::bitset<256> keys_;
  int vertical_remainder_ = 0, horizontal_remainder_ = 0;
  wolf::core::sessions::ConnectionScope source_;
  std::weak_ptr<ControlPeer> peer_;
  struct Sample {
    uint64_t x, y, dx, dy;
    uint32_t width, height;
  };
  std::optional<Sample> sample_;
  std::optional<uint32_t> contact_;
  WindowInput(Spec, Operations, bool);
  bool events();
  std::optional<Geometry> snapshot();
  bool descendant(uint32_t);
  bool hit(const Geometry &, std::optional<std::pair<int, int>> = {});
  bool release_button(uint8_t);
  bool point(const Sample &, const Geometry &);
  bool authorize(const wolf::core::sessions::ConnectionScope &, const std::shared_ptr<ControlPeer> &);
  bool fail();
  bool release();

public:
  static std::shared_ptr<WindowInput> bind(Spec, Operations = Operations::defaults(), bool keyboard_mouse = false);
  ~WindowInput() = default; // no injected button from lifecycle thread; legacy held-input limit remains
  const Spec &spec() const {
    return spec_;
  }
  bool absolute(const wolf::core::sessions::ConnectionScope &,
                const std::shared_ptr<ControlPeer> &,
                const moonlight::control::pkts::MOUSE_MOVE_ABS_PACKET &);
  bool button(const wolf::core::sessions::ConnectionScope &,
              const std::shared_ptr<ControlPeer> &,
              const moonlight::control::pkts::MOUSE_BUTTON_PACKET &);
  bool key(const wolf::core::sessions::ConnectionScope &,
           const std::shared_ptr<ControlPeer> &,
           const moonlight::control::pkts::KEYBOARD_PACKET &);
  bool text(const wolf::core::sessions::ConnectionScope &, const std::shared_ptr<ControlPeer> &, std::string_view);
  bool relative(const wolf::core::sessions::ConnectionScope &,
                const std::shared_ptr<ControlPeer> &,
                const moonlight::control::pkts::MOUSE_MOVE_REL_PACKET &);
  bool scroll(const wolf::core::sessions::ConnectionScope &,
              const std::shared_ptr<ControlPeer> &,
              int amount,
              bool horizontal);
  bool touch(const wolf::core::sessions::ConnectionScope &,
             const std::shared_ptr<ControlPeer> &,
             const moonlight::control::pkts::TOUCH_PACKET &);
};
} // namespace control::direct
