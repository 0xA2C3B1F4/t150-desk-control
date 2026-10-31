#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace esphome::desk_firmware_update {
// A sequential halfword write interrupted between programming operations.
// Arbitrary corruption, holes, and a partially programmed halfword fail closed.
// Returns the halfword-aligned programmed length, or 0 if the block is not a
// non-empty programmed prefix followed by erased bytes.
inline size_t partial_prefix_length(const uint8_t *observed) {
  size_t end=1024;
  while (end && observed[end-1]==255) --end;
  end=(end+1)&~size_t(1);
  return end<1024 ? end : 0;
}
inline bool partial_prefix_matches(const uint8_t *observed, const uint8_t *expected) {
  const size_t end=partial_prefix_length(observed);
  return end>0 && !std::memcmp(observed,expected,end);
}
}  // namespace esphome::desk_firmware_update
