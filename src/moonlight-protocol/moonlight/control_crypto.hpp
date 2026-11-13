#pragma once

#include <array>
#include <atomic>
#include <bitset>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace moonlight::control {

enum class EncryptionMode {
  unnegotiated,
  legacy,
  v2
};
enum class Direction {
  client,
  host
};
inline constexpr unsigned CONTROL_V2 = 0x01;

// Shared by snapshots of one launch/resume; never carried over to a new launch.
// Only ANNOUNCE may select a mode. Later attempts to change it fail closed.
struct ControlCryptoState {
  std::atomic<EncryptionMode> mode{EncryptionMode::unnegotiated};

  bool negotiate(unsigned flags, bool allow_legacy) {
    if (flags & ~CONTROL_V2)
      return false;
    auto selected = flags & CONTROL_V2 ? EncryptionMode::v2 : EncryptionMode::legacy;
    if (selected == EncryptionMode::legacy && !allow_legacy)
      return false;
    auto expected = EncryptionMode::unnegotiated;
    return mode.compare_exchange_strong(expected, selected) || expected == selected;
  }
};

// Moonlight ControlStream.c populateOutgoingIv/populateIncomingIv. Legacy's
// one-byte IV is retained for opt-in inbound compatibility ONLY.
inline std::string control_iv(std::uint32_t seq, EncryptionMode mode, Direction direction) {
  if (mode == EncryptionMode::unnegotiated)
    return {};
  std::string iv(mode == EncryptionMode::v2 ? 12 : 16, '\0');
  iv[0] = static_cast<char>(seq);
  if (mode == EncryptionMode::v2) {
    iv[1] = static_cast<char>(seq >> 8);
    iv[2] = static_cast<char>(seq >> 16);
    iv[3] = static_cast<char>(seq >> 24);
    iv[10] = direction == Direction::host ? 'H' : 'C';
    iv[11] = 'C';
  }
  return iv;
}

// One global sequence space per key, with reordering across ENet channels.
// Call accept only after tag and structural validation; never roll it back.
class ReplayWindow {
public:
  bool accept(std::uint32_t seq) {
    if (!highest_) {
      highest_ = seq;
      seen_.set(0);
      return true;
    }
    if (seq > *highest_) {
      auto delta = static_cast<std::uint64_t>(seq) - *highest_;
      seen_ <<= delta < seen_.size() ? delta : seen_.size();
      highest_ = seq;
      seen_.set(0);
      return true;
    }
    auto age = static_cast<std::uint64_t>(*highest_) - seq;
    if (age >= seen_.size() || seen_.test(age))
      return false;
    seen_.set(age);
    return true;
  }

private:
  std::optional<std::uint32_t> highest_;
  std::bitset<1024> seen_;
};

struct ControlKeyState {
  ReplayWindow received;
  std::uint64_t next_sequence = 0;

  std::optional<std::uint32_t> reserve_sequence() {
    if (next_sequence > std::numeric_limits<std::uint32_t>::max())
      return {};
    return static_cast<std::uint32_t>(next_sequence++);
  }
};

// Owned exclusively by the control thread, not a process-global mutable cache.
// No eviction: reconnect/resume with a reused key must not reset GCM counters.
// At capacity only unseen keys are refused, existing streams keep operating.
class ControlKeyRegistry {
public:
  static constexpr size_t capacity = 1024;
  ControlKeyState *find_or_insert(const std::string &binary_key) {
    if (binary_key.size() != 16)
      return nullptr;
    if (auto it = keys_.find(binary_key); it != keys_.end())
      return &it->second;
    if (keys_.size() == capacity)
      return nullptr;
    return &keys_.try_emplace(binary_key).first->second;
  }

private:
  std::map<std::string, ControlKeyState> keys_;
};

} // namespace moonlight::control
