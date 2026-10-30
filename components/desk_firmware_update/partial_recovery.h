#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace esphome::desk_firmware_update {
// A sequential halfword write interrupted between programming operations.
// Arbitrary corruption, holes, and a partially programmed halfword fail closed.
inline bool partial_prefix_matches(const uint8_t *observed, const uint8_t *expected) {
  size_t end=1024;
  while (end && observed[end-1]==255) --end;
  end=(end+1)&~size_t(1);
  return end>0 && end<1024 && !std::memcmp(observed,expected,end);
}
}  // namespace esphome::desk_firmware_update
