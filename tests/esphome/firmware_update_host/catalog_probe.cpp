// Compile the production admission code with the existing host stand-ins.
#include "desk_firmware_update.cpp"
#include <iostream>
namespace fw = esphome::desk_firmware_update;
struct Probe : fw::FirmwareUpdate {
  bool prepares(const char* source, const char* action) {
    preflight_ = tail_erased_ = backup_ = true;
    preflight_at_ = esphome::millis();
    size_ = fw::IMAGE_SIZE;
    phase_ = Phase::Ready;
    std::strcpy(hash_, source);
    buffer_ = std::shared_ptr<uint8_t>(new uint8_t[size_], std::default_delete<uint8_t[]>());
    std::memset(buffer_.get(), 255, size_);
    // Synthetic input reaches the existing DT02 transformation. It is not a
    // supported image and must fail the complete output hash check.
    const uint8_t sleep[] = {0x18, 0x18, 0x3c, 0x28}, hook[] = {0xca, 0x48, 0x02, 0x68};
    std::memcpy(buffer_.get() + 0x1656, sleep, sizeof(sleep));
    std::memcpy(buffer_.get() + 0x53ae, hook, sizeof(hook));
    execute_(action);
    if (!error_)
      std::exit(3);
    if (!std::strcmp(action, "patch"))
      return !std::strcmp(error_, "Generated patch hash mismatch");
    if (!std::strcmp(action, "patch_child_lock"))
      return !std::strcmp(
                 error_,
                 "Cannot prepare panel lock; firmware does not match the required starting version") ||
             !std::strcmp(error_, "Prepared panel-lock firmware failed its integrity check");
    if (!std::strcmp(action, "patch_timer_buzzer") || !std::strcmp(action, "patch_platform"))
      return !std::strcmp(error_, "Cannot prepare the exact reset and speed source") ||
             !std::strcmp(error_,
                          "Cannot restore the exact reset and speed source from timer firmware") ||
             !std::strcmp(error_,
                          "Intermediate reset and speed firmware failed its integrity check");
    return !std::strcmp(
               error_,
               "Cannot prepare reset and speed controls; firmware does not match the required starting version") ||
           !std::strcmp(error_, "Prepared reset and speed firmware failed its integrity check");
  }
  size_t payload(const char* hash) {
    std::strcpy(hash_, hash);
    return payload_size_();
  }
  bool admits(const char* source, const char* target) {
    preflight_ = tail_erased_ = backup_ = true;
    preflight_at_ = esphome::millis();
    size_ = fw::IMAGE_SIZE;
    std::strcpy(source_hash_, source);
    std::strcpy(hash_, target);
    return admit_();
  }
};
int main() {
  const char* ids[] = {"stock", "dt02", "cl01", "rs01-r7", "tb01", "fp01"};
  const char* hashes[] = {fw::STOCK_SHA, fw::DT02_SHA, fw::CL01_SHA,
                          fw::RS01_SHA,  fw::TB01_SHA, fw::FP01_SHA};
  Probe probe;
  std::cout << "{\"application_size\":" << fw::IMAGE_SIZE << ",\"images\":{";
  for (unsigned i = 0; i < 6; ++i) {
    if (i)
      std::cout << ',';
    if (!fw::known(hashes[i]))
      return 1;
    std::cout << '"' << ids[i] << "\":\"" << hashes[i] << '"';
  }
  std::cout << "},\"payload_sizes\":{";
  for (unsigned i = 0; i < 6; ++i) {
    if (i)
      std::cout << ',';
    std::cout << '"' << ids[i] << "\":" << probe.payload(hashes[i]);
  }
  std::cout << "},\"erase_size\":" << desk_protocol::firmware_layout::ERASE_SIZE
            << ",\"install\":[";
  bool first = true;
  for (unsigned i = 0; i < 6; ++i)
    for (unsigned j = 0; j < 6; ++j) {
      if (!probe.admits(hashes[i], hashes[j]))
        continue;
      if (!first)
        std::cout << ',';
      first = false;
      std::cout << "[\"" << ids[i] << "\",\"" << ids[j] << "\"]";
    }
  for (const char* hash : hashes)
    if (probe.admits("unknown", hash) || probe.admits(hash, "unknown"))
      return 2;
  std::cout << "],\"prepare\":{";
  const char* actions[] = {"patch", "patch_child_lock", "patch_reset_speed", "patch_timer_buzzer",
                           "patch_platform"};
  for (unsigned target = 1; target < 6; ++target) {
    if (target > 1)
      std::cout << ',';
    std::cout << '"' << ids[target] << "\":[";
    bool initial = true;
    for (unsigned source = 0; source < 6; ++source) {
      if (!probe.prepares(hashes[source], actions[target - 1]))
        continue;
      if (!initial)
        std::cout << ',';
      initial = false;
      std::cout << '"' << ids[source] << '"';
    }
    if (probe.prepares("unknown", actions[target - 1]))
      return 4;
    std::cout << ']';
  }
  std::cout << "}}\n";
}
