#include "checked_xbox_backend.hpp"
#ifdef __linux__
#include <stdexcept>
namespace control {
CheckedXboxBackend::CheckedXboxBackend(std::unique_ptr<inputtino::checked::Xbox> xbox) : xbox_(std::move(xbox)) {
  if (!xbox_)
    throw std::invalid_argument("CheckedXboxBackend requires an owned backend");
}
DeviceGeneration CheckedXboxBackend::generation() const {
  return xbox_->generation();
}
inputtino::checked::WriteReceipt CheckedXboxBackend::apply(const DeviceGeneration &generation,
                                                           const inputtino::checked::Frame &frame) {
  return xbox_->apply(generation, frame);
}
inputtino::checked::WriteReceipt CheckedXboxBackend::neutral(const DeviceGeneration &generation) {
  return xbox_->neutral(generation);
}
void CheckedXboxBackend::request_stop() noexcept {
  xbox_->request_stop();
}
inputtino::checked::Retirement CheckedXboxBackend::retire_until(std::chrono::steady_clock::time_point deadline) {
  return xbox_->retire_until(deadline);
}
} // namespace control
#endif
