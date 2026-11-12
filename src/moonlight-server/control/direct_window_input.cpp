#include "direct_window_input.hpp"
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <xcb/xcb.h>
#include <xcb/xtest.h>

namespace control::direct {
namespace {
template <class T> using Reply = std::unique_ptr<T, decltype(&std::free)>;
struct Native {
  xcb_connection_t *c;
  explicit Native(xcb_connection_t *value) : c(value) {}
  Native(const Native &) = delete;
  ~Native() {
    xcb_disconnect(c);
  }
};
xcb_connection_t *connection(void *p) {
  return static_cast<Native *>(p)->c;
}
bool checked(xcb_connection_t *c, xcb_void_cookie_t cookie) {
  Reply<xcb_generic_error_t> error(xcb_request_check(c, cookie), std::free);
  return !error && !xcb_connection_has_error(c);
}
std::optional<xcb_atom_t> atom(xcb_connection_t *c, const char *name) {
  Reply<xcb_intern_atom_reply_t> r(xcb_intern_atom_reply(c, xcb_intern_atom(c, 0, std::strlen(name), name), nullptr),
                                   std::free);
  if (!r || !r->atom)
    return {};
  return r->atom;
}
struct Call {
  std::atomic_flag &flag;
  bool acquired;
  explicit Call(std::atomic_flag &f) : flag(f), acquired(!f.test_and_set(std::memory_order_acquire)) {}
  ~Call() {
    if (acquired)
      flag.clear(std::memory_order_release);
  }
};
} // namespace
Operations Operations::defaults() {
  return {
      []() -> std::shared_ptr<void> {
        const char *display = std::getenv("DISPLAY");
        char *host = nullptr;
        int number = 0, screen = 0;
        if (!display || !xcb_parse_display(display, &host, &number, &screen))
          return {};
        std::free(host);
        auto raw = xcb_connect(display, nullptr);
        // Own transfer-full connection immediately, including allocation failure.
        std::unique_ptr<xcb_connection_t, decltype(&xcb_disconnect)> owned(raw, xcb_disconnect);
        if (!raw || xcb_connection_has_error(raw))
          return {};
        auto native = std::make_shared<Native>(owned.get());
        owned.release();
        Reply<xcb_test_get_version_reply_t> version(
            xcb_test_get_version_reply(native->c, xcb_test_get_version(native->c, 2, 2), nullptr),
            std::free);
        if (!version || xcb_connection_has_error(native->c))
          return {};
        return native;
      },
      [](void *p, uint32_t index) -> std::optional<uint32_t> {
        auto c = connection(p);
        auto roots = xcb_setup_roots_iterator(xcb_get_setup(c));
        for (uint32_t i = 0; i < index && roots.rem; ++i)
          xcb_screen_next(&roots);
        if (!roots.rem || xcb_connection_has_error(c))
          return {};
        return roots.data->root;
      },
      [](void *p, uint32_t xid) {
        auto c = connection(p);
        const uint32_t mask = XCB_EVENT_MASK_STRUCTURE_NOTIFY;
        return checked(c, xcb_change_window_attributes_checked(c, xid, XCB_CW_EVENT_MASK, &mask));
      },
      [](void *p, uint32_t xid) -> std::optional<Attributes> {
        auto c = connection(p);
        Reply<xcb_get_window_attributes_reply_t> r(
            xcb_get_window_attributes_reply(c, xcb_get_window_attributes(c, xid), nullptr),
            std::free);
        if (!r || xcb_connection_has_error(c))
          return {};
        return Attributes{r->map_state == XCB_MAP_STATE_VIEWABLE, r->_class == XCB_WINDOW_CLASS_INPUT_OUTPUT};
      },
      [](void *p, uint32_t xid) -> std::optional<Geometry> {
        auto c = connection(p);
        Reply<xcb_get_geometry_reply_t> r(xcb_get_geometry_reply(c, xcb_get_geometry(c, xid), nullptr), std::free);
        if (!r || xcb_connection_has_error(c))
          return {};
        return Geometry{r->root, r->x, r->y, r->width, r->height};
      },
      [](void *p, uint32_t xid, uint32_t root) -> std::optional<Pointer> {
        auto c = connection(p);
        Reply<xcb_translate_coordinates_reply_t> r(
            xcb_translate_coordinates_reply(c, xcb_translate_coordinates(c, xid, root, 0, 0), nullptr),
            std::free);
        if (!r || !r->same_screen || xcb_connection_has_error(c))
          return {};
        return Pointer{r->child, r->dst_x, r->dst_y};
      },
      [](void *p, uint32_t xid, Property property) -> std::optional<uint32_t> {
        auto c = connection(p);
        auto key = atom(c, property == Property::pid ? "_NET_WM_PID" : "_NET_ACTIVE_WINDOW");
        if (!key)
          return {};
        auto type = property == Property::pid ? XCB_ATOM_CARDINAL : XCB_ATOM_WINDOW;
        Reply<xcb_get_property_reply_t> r(
            xcb_get_property_reply(c, xcb_get_property(c, 0, xid, *key, type, 0, 1), nullptr),
            std::free);
        if (!r || r->format != 32 || r->type != type || r->bytes_after || xcb_get_property_value_length(r.get()) != 4 ||
            xcb_connection_has_error(c))
          return {};
        uint32_t value;
        std::memcpy(&value, xcb_get_property_value(r.get()), 4);
        return value;
      },
      [](uint32_t pid) -> std::optional<uint64_t> {
        if (!pid)
          return {};
        std::ifstream file("/proc/" + std::to_string(pid) + "/stat");
        char buffer[4097]{};
        if (!file.getline(buffer, sizeof(buffer)))
          return {};
        std::string line(buffer);
        auto end = line.rfind(')');
        if (end == std::string::npos)
          return {};
        std::istringstream fields(line.substr(end + 1));
        std::string field;
        for (int i = 0; i <= 19; ++i)
          if (!(fields >> field))
            return {};
        uint64_t value = 0;
        auto parsed = std::from_chars(field.data(), field.data() + field.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != field.data() + field.size() || !value)
          return {};
        return value;
      },
      [](void *p, uint32_t xid) -> std::optional<Tree> {
        auto c = connection(p);
        Reply<xcb_query_tree_reply_t> r(xcb_query_tree_reply(c, xcb_query_tree(c, xid), nullptr), std::free);
        if (!r || xcb_connection_has_error(c))
          return {};
        return Tree{r->root, r->parent};
      },
      [](void *p, uint32_t xid) -> std::optional<Pointer> {
        auto c = connection(p);
        Reply<xcb_query_pointer_reply_t> r(xcb_query_pointer_reply(c, xcb_query_pointer(c, xid), nullptr), std::free);
        if (!r || !r->same_screen || xcb_connection_has_error(c))
          return {};
        return Pointer{r->child, r->root_x, r->root_y};
      },
      [](void *p) -> Event {
        auto c = connection(p);
        Reply<xcb_generic_event_t> e(xcb_poll_for_event(c), std::free);
        if (xcb_connection_has_error(c))
          return {Event::error};
        if (!e)
          return {Event::none};
        auto type = e->response_type & 0x7f;
        if (!type)
          return {Event::error};
        if (type == XCB_DESTROY_NOTIFY)
          return {Event::destroyed, reinterpret_cast<xcb_destroy_notify_event_t *>(e.get())->window};
        return {Event::changed};
      },
      [](void *p, uint32_t root, int16_t x, int16_t y) {
        auto c = connection(p);
        return checked(c, xcb_test_fake_input_checked(c, XCB_MOTION_NOTIFY, 0, XCB_CURRENT_TIME, root, x, y, 0));
      },
      [](void *p, bool press) {
        auto c = connection(p);
        return checked(c,
                       xcb_test_fake_input_checked(c,
                                                   press ? XCB_BUTTON_PRESS : XCB_BUTTON_RELEASE,
                                                   1,
                                                   XCB_CURRENT_TIME,
                                                   XCB_NONE,
                                                   0,
                                                   0,
                                                   0));
      }};
}
WindowInput::WindowInput(Spec s, Operations o) : spec_(s), ops_(std::move(o)) {}
std::shared_ptr<WindowInput> WindowInput::bind(Spec spec, Operations ops) {
  if (!spec.xid || !spec.pid || !spec.root || !spec.process_start_time || spec.screen > 63)
    throw std::invalid_argument("Invalid direct window binding");
  auto input = std::shared_ptr<WindowInput>(new WindowInput(spec, std::move(ops)));
  input->connection_ = input->ops_.connect();
  if (!input->connection_ || !input->ops_.subscribe(input->connection_.get(), spec.xid) || !input->snapshot() ||
      !input->events() || !input->snapshot())
    throw std::runtime_error("Direct window binding unavailable");
  return input;
}
bool WindowInput::events() {
  for (int i = 0; i < 64; ++i) {
    auto e = ops_.poll(connection_.get());
    if (e.kind == Event::none)
      return true;
    if (e.kind == Event::error || (e.kind == Event::destroyed && e.window == spec_.xid))
      return fail();
  }
  return fail(); // bounded backlog, no unbounded event drain
}
std::optional<Geometry> WindowInput::snapshot() {
  auto c = connection_.get();
  auto root = ops_.screen_root(c, spec_.screen);
  auto a = ops_.attributes(c, spec_.xid);
  auto g = ops_.geometry(c, spec_.xid);
  auto origin = ops_.translate(c, spec_.xid, spec_.root);
  auto size = ops_.geometry(c, spec_.root);
  auto pid = ops_.property(c, spec_.xid, Property::pid);
  auto start = ops_.start_time(spec_.pid);
  if (!root || *root != spec_.root || !a || !a->viewable || !a->input_output || !g || g->root != spec_.root ||
      !g->width || !g->height || !origin || !size || size->root != spec_.root || !pid || *pid != spec_.pid || !start ||
      *start != spec_.process_start_time || origin->x < 0 || origin->y < 0 ||
      uint64_t(origin->x) + g->width > size->width || uint64_t(origin->y) + g->height > size->height)
    return {};
  g->x = origin->x;
  g->y = origin->y;
  return g;
}
bool WindowInput::descendant(uint32_t child) {
  for (int i = 0; i < 32 && child && child != spec_.root; ++i) {
    if (child == spec_.xid)
      return true;
    auto t = ops_.tree(connection_.get(), child);
    if (!t || t->root != spec_.root || t->parent == child)
      return false;
    child = t->parent;
  }
  return false;
}
bool WindowInput::point(const Sample &s, const Geometry &g) {
  if (!s.dx || !s.dy || s.x > s.dx || s.y > s.dy || !g.width || !g.height)
    return false;
  const int64_t x = int64_t(g.x) + std::min<uint64_t>(g.width - 1, s.x * g.width / s.dx);
  const int64_t y = int64_t(g.y) + std::min<uint64_t>(g.height - 1, s.y * g.height / s.dy);
  if (x < 0 || y < 0 || x > INT16_MAX || y > INT16_MAX)
    return false;
  auto active = ops_.property(connection_.get(), spec_.root, Property::active_window);
  if (!active || !descendant(*active))
    return false;
  if (!ops_.motion(connection_.get(), spec_.root, int16_t(x), int16_t(y)))
    return fail();
  uint32_t at = spec_.root;
  for (int i = 0; i < 32; ++i) {
    auto p = ops_.pointer(connection_.get(), at);
    if (!p || p->x != x || p->y != y)
      return false;
    if (!p->child)
      return descendant(at);
    if (p->child == at)
      return false;
    at = p->child;
  }
  return false;
}
bool WindowInput::authorize(const wolf::core::sessions::ConnectionScope &source,
                            const std::shared_ptr<ControlPeer> &peer) {
  if (failed_ || !source || !peer || !peer->send)
    return false;
  auto id = std::this_thread::get_id();
  if (owner_ != std::thread::id{} && owner_ != id)
    return false;
  owner_ = id;
  if (source_) {
    if (!source_.matches(source) || peer_.owner_before(peer) || peer.owner_before(peer_))
      return false;
  } else {
    source_ = source;
    peer_ = peer;
  }
  return events();
}
bool WindowInput::release() {
  contact_.reset();
  sample_.reset();
  if (!left_)
    return true;
  left_ = false;
  const bool ok = ops_.button(connection_.get(), false);
  if (!ok)
    failed_ = true;
  return ok;
}
bool WindowInput::fail() {
  failed_ = true;
  try {
    release();
  } catch (...) {
  } // best effort only, never a checked-neutral receipt
  return false;
}
bool WindowInput::absolute(const wolf::core::sessions::ConnectionScope &source,
                           const std::shared_ptr<ControlPeer> &peer,
                           const moonlight::control::pkts::MOUSE_MOVE_ABS_PACKET &pkt) {
  Call call(busy_);
  if (!call.acquired)
    return false;
  try {
    if (!authorize(source, peer) || contact_)
      return false;
    int x = boost::endian::big_to_native(pkt.x), y = boost::endian::big_to_native(pkt.y);
    int w = boost::endian::big_to_native(pkt.width), h = boost::endian::big_to_native(pkt.height);
    if (w <= 0 || h <= 0 || x < 0 || y < 0 || x > w || y > h) {
      sample_.reset();
      return false;
    }
    auto g = snapshot();
    if (!g)
      return fail();
    Sample s{uint64_t(x), uint64_t(y), uint64_t(w), uint64_t(h), g->width, g->height};
    sample_.reset();
    if (!point(s, *g)) {
      if (left_)
        release();
      return false;
    }
    sample_ = s;
    return true;
  } catch (...) {
    return fail();
  }
}
bool WindowInput::button(const wolf::core::sessions::ConnectionScope &source,
                         const std::shared_ptr<ControlPeer> &peer,
                         const moonlight::control::pkts::MOUSE_BUTTON_PACKET &pkt) {
  using namespace moonlight::control::pkts;
  Call call(busy_);
  if (!call.acquired)
    return false;
  try {
    if (!authorize(source, peer) || pkt.button != 1 || contact_)
      return false;
    if (pkt.type == MOUSE_BUTTON_RELEASE)
      return left_ && release();
    if (pkt.type != MOUSE_BUTTON_PRESS || left_ || !sample_)
      return false;
    auto g = snapshot();
    if (!g)
      return fail();
    if (g->width != sample_->width || g->height != sample_->height) {
      sample_.reset();
      return false;
    }
    if (!point(*sample_, *g))
      return false;
    left_ = true; // uncertain checked request may already have acted; cleanup owns release
    if (!ops_.button(connection_.get(), true))
      return fail();
    return true;
  } catch (...) {
    return fail();
  }
}
bool WindowInput::touch(const wolf::core::sessions::ConnectionScope &source,
                        const std::shared_ptr<ControlPeer> &peer,
                        const moonlight::control::pkts::TOUCH_PACKET &pkt) {
  using namespace moonlight::control;
  using namespace pkts;
  Call call(busy_);
  if (!call.acquired)
    return false;
  try {
    if (!authorize(source, peer))
      return false;
    auto id = boost::endian::little_to_native(pkt.pointer_id);
    if (pkt.event_type == TOUCH_EVENT_CANCEL_ALL)
      return contact_ && release();
    if (pkt.event_type == TOUCH_EVENT_UP || pkt.event_type == TOUCH_EVENT_CANCEL) {
      if (!contact_ || *contact_ != id)
        return false;
      return release();
    }
    const bool down = pkt.event_type == TOUCH_EVENT_DOWN;
    if ((!down && pkt.event_type != TOUCH_EVENT_MOVE) || (down && (contact_ || left_)) ||
        (!down && (!contact_ || *contact_ != id)))
      return false;
    float x = input_float(pkt.x), y = input_float(pkt.y);
    if (!std::isfinite(x) || !std::isfinite(y) || x < 0 || y < 0 || x > 1 || y > 1)
      return false;
    auto g = snapshot();
    if (!g)
      return fail();
    if (!down && (!sample_ || g->width != sample_->width || g->height != sample_->height)) {
      release();
      return false;
    }
    constexpr uint64_t units = 1u << 24;
    Sample s{uint64_t(std::llround(double(x) * units)),
             uint64_t(std::llround(double(y) * units)),
             units,
             units,
             g->width,
             g->height};
    if (!point(s, *g)) {
      if (left_)
        release();
      return false;
    }
    sample_ = s;
    if (down) {
      left_ = true;
      if (!ops_.button(connection_.get(), true))
        return fail();
      contact_ = id;
    }
    return true;
  } catch (...) {
    return fail();
  }
}
} // namespace control::direct
