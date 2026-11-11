#pragma once

// Linux-only additive backend. No legacy input handler or factory calls this API.
#ifdef __linux__
#include <inputtino/checked_xbox.hpp>

namespace control {
using DeviceGeneration = inputtino::checked::DeviceGeneration;
class CheckedXboxBackend {
  std::unique_ptr<inputtino::checked::Xbox> xbox_;

public:
  explicit CheckedXboxBackend(std::unique_ptr<inputtino::checked::Xbox> xbox);
  DeviceGeneration generation() const;
  inputtino::checked::WriteReceipt apply(const DeviceGeneration &, const inputtino::checked::Frame &);
  inputtino::checked::WriteReceipt neutral(const DeviceGeneration &);
  void request_stop() noexcept;
  inputtino::checked::Retirement retire_until(std::chrono::steady_clock::time_point deadline);
  // This owner must remain alive on Unknown; destructor joins, it never detaches.
};
} // namespace control
#endif
