// Compile the production admission code with the existing host stand-ins.
#include "desk_firmware_update.cpp"
#include <iostream>
namespace fw = esphome::desk_firmware_update;
struct Probe : fw::FirmwareUpdate {
  bool prepares(const char* source) {
    preflight_ = tail_erased_ = backup_ = true;
    preflight_at_ = esphome::millis();
    size_ = fw::IMAGE_SIZE;
    phase_ = Phase::Ready;
    std::strcpy(hash_, source);
    buffer_ = std::shared_ptr<uint8_t>(new uint8_t[size_](), std::default_delete<uint8_t[]>());
    if (!can_prepare_current_()) {
      return false;
    }
    execute_("prepare_current");
    // Synthetic bytes must reach the source-specific route, then fail its
    // integrity check. Exact successful conversions use private images.
    const char* expected = "Cannot prepare the exact reset and speed source";
    if (!std::strcmp(source, fw::CURRENT_SHA)) {
      expected = "Current firmware failed its integrity check";
    } else if (!std::strcmp(source, fw::BZ01_SHA)) {
      expected = "Cannot restore the exact reset and speed source from sound firmware";
    } else if (!std::strcmp(source, fw::TR01_SHA)) {
      expected = "Cannot restore the exact reset and speed source from trim firmware";
    } else if (!std::strcmp(source, fw::FP01_SHA)) {
      expected = "Cannot restore the exact reset and speed source from platform firmware";
    } else if (!std::strcmp(source, fw::TB01_SHA)) {
      expected = "Cannot restore the exact reset and speed source from timer firmware";
    } else if (!std::strcmp(source, fw::RS01_SHA)) {
      expected = "Intermediate reset and speed firmware failed its integrity check";
    }
    return phase_ == Phase::Error && error_ && !std::strcmp(error_, expected);
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
  const char* ids[] = {"stock", "dt02", "cl01", "rs01-r7", "tb01", "fp01", "tr01", "bz01", "ds01"};
  const char* hashes[] = {fw::STOCK_SHA, fw::DT02_SHA, fw::CL01_SHA, fw::RS01_SHA, fw::TB01_SHA,
                          fw::FP01_SHA,  fw::TR01_SHA, fw::BZ01_SHA, fw::DS01_SHA};
  Probe probe;
  std::cout << "{\"application_size\":" << fw::IMAGE_SIZE << ",\"images\":{";
  for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i) {
    if (i)
      std::cout << ',';
    if (!fw::known(hashes[i]))
      return 1;
    std::cout << '"' << ids[i] << "\":\"" << hashes[i] << '"';
  }
  std::cout << "},\"payload_sizes\":{";
  for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i) {
    if (i)
      std::cout << ',';
    std::cout << '"' << ids[i] << "\":" << probe.payload(hashes[i]);
  }
  std::cout << "},\"erase_size\":" << desk_protocol::firmware_layout::ERASE_SIZE
            << ",\"install\":[";
  bool first = true;
  for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i)
    for (unsigned j = 0; j < sizeof(ids) / sizeof(ids[0]); ++j) {
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
  const char* target = nullptr;
  for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i) {
    if (!std::strcmp(hashes[i], fw::CURRENT_SHA)) {
      target = ids[i];
    }
  }
  if (!target) {
    return 3;
  }
  std::cout << "\"target\":\"" << target << "\",\"sources\":[";
  bool initial = true;
  for (unsigned source = 0; source < sizeof(ids) / sizeof(ids[0]); ++source) {
    if (!probe.prepares(hashes[source]))
      continue;
    if (!initial)
      std::cout << ',';
    initial = false;
    std::cout << '"' << ids[source] << '"';
  }
  if (probe.prepares("unknown"))
    return 4;
  std::cout << "]}}\n";
}
