#pragma once
#include <enet/enet.h>
#include <future>
#include <sessions/connection_scope.hpp>
#include <state/data-structures.hpp>
#include <thread>

namespace control {
struct PeerIdentity;
using PeerToken = wolf::core::sessions::Scope<size_t, PeerIdentity>;
struct ControlPeer;
class InputSeat;
} // namespace control
namespace control::loop {
using Clock = std::chrono::steady_clock;
using ThreadFactory = std::function<std::thread(std::function<void()>)>;
struct Config {
  std::string host = "0.0.0.0";
  uint16_t port = 47999;
  size_t peers = 20;
  std::chrono::milliseconds decision_timeout{5000};
  std::function<Clock::time_point()> now = [] { return Clock::now(); }; // nonblocking trusted clock
};
// Trusted synchronous C-operation table; context must not capture State/Owner.
// create: throw means unpublished resources already rolled back. Other calls
// retain ordinary ENet ownership contracts; uncertain release is never retried.
// service leaves event zero or a valid transfer-full received packet even on throw;
// it never destroys host. No escaping borrowed event pointer.
struct Operations {
  std::shared_ptr<void> owner;
  ENetHost *(*create)(void *, const char *, uint16_t, size_t) = nullptr;
  int (*service)(void *, ENetHost *, ENetEvent *, uint32_t) = nullptr;
  ENetPacket *(*packet)(void *, const void *, size_t, uint32_t) = nullptr;
  int (*send)(void *, ENetPeer *, uint8_t, ENetPacket *) = nullptr;
  void (*destroy_packet)(void *, ENetPacket *) = nullptr;
  void (*reset)(void *, ENetPeer *) = nullptr;
  void (*disconnect)(void *, ENetPeer *) = nullptr;
  void (*flush)(void *, ENetHost *) = nullptr;
  void (*destroy_host)(void *, ENetHost *) = nullptr;
  static Operations defaults(); // no init/host/socket until worker calls create
};
struct PeerLease {
  PeerToken token;
  wolf::core::sessions::ConnectionScope connection;
  wolf::core::sessions::RunScope run;
  bool authenticated = false;
  std::shared_ptr<ControlPeer> sender; // immutable callback, no raw peer
};
struct Status {
  bool ready, stop_requested, exited, joined, failed, unknown;
  size_t peers, calls;
};
enum class JoinResult {
  Refused,
  Unknown,
  Joined
};
struct State;
class ControlLoopDomain;
class ControlLoopOwner {
  friend class ControlLoopDomain;
  std::shared_ptr<State> state_;
  std::thread worker_;
  std::shared_future<void> exited_;
  std::mutex join_mutex_;
  std::atomic_bool joined_{false};
  ControlLoopOwner(std::shared_ptr<State>, std::shared_future<void>);

public:
  ~ControlLoopOwner(); // lifecycle only; may wait indefinitely, never execution stack
  void request_stop() noexcept;
  Status status() const;
  std::vector<PeerLease> peers() const;
  JoinResult join_when_exited(); // lifecycle caller; no blocking until exit proved
  // Internal one-shot slot, no runtime caller in Wolf. Takes an EXISTING seat;
  // no creation/GameACL/selection/ingress authority implied.
  bool attach_feedback(std::shared_ptr<InputSeat>);
};
class ControlLoopDomain {
  bool reserved_ = false;
  std::mutex mutex_;
  std::shared_ptr<ControlLoopOwner> owner_;

public:
  ~ControlLoopDomain(); // request stop + actual join; lifecycle owner only
  std::shared_ptr<ControlLoopOwner> start(
      Config,
      state::SessionsAtoms,
      std::shared_ptr<wolf::core::events::EventBusType>,
      Operations = Operations::defaults(),
      ThreadFactory = [](std::function<void()> work) { return std::thread(std::move(work)); });
};
} // namespace control::loop
