#include "motion_trim_core.h"

bool trim_values_valid(const int8_t values[TRIM_COUNT]) {
  for (unsigned i = 0; i < TRIM_COUNT; ++i) {
    if (values[i] < -3 || values[i] > 3)
      return false;
  }
  return true;
}

static bool signed_greater(uint32_t a, uint32_t b) {
  return (a ^ UINT32_C(0x80000000)) > (b ^ UINT32_C(0x80000000));
}

uint32_t trim_target(uint32_t requested, uint32_t current, const int8_t values[TRIM_COUNT],
                     uint8_t speed, bool admitted) {
  bool up = signed_greater(requested, current + UINT32_C(4));
  bool down = !up && signed_greater(current - UINT32_C(4), requested);
  uint32_t base = up ? requested + UINT32_C(7) : down ? requested - UINT32_C(6) : requested;
  if ((!up && !down) || !admitted || speed > 1 || base < 6 || base > 747 ||
      !trim_values_valid(values))
    return base;
  int32_t correction = values[2 * speed + (up ? 0 : 1)];
  int32_t result = (int32_t)base + (up ? correction : -correction);
  return result < 6 ? 6u : result > 747 ? 747u : (uint32_t)result;
}

/* Four-bit codewords with pairwise distance four detect up to three changed
 * bits. Arbitrary corruption can still produce a different valid codeword. */
static const uint8_t codewords[7] = {0x1e, 0x2d, 0x33, 0x4b, 0x55, 0x66, 0x78};

bool trim_record_encode(const int8_t values[TRIM_COUNT], uint8_t record[TRIM_RECORD_SIZE]) {
  if (!trim_values_valid(values)) {
    return false;
  }
  for (unsigned i = 0; i < TRIM_COUNT; ++i) {
    record[i] = codewords[values[i] + 3];
  }
  return true;
}

uint8_t trim_record_decode(const uint8_t first[TRIM_RECORD_SIZE],
                           const uint8_t second[TRIM_RECORD_SIZE], int8_t values[TRIM_COUNT]) {
  for (unsigned i = 0; i < TRIM_COUNT; ++i) {
    values[i] = 0;
  }
  for (unsigned i = 0; i < TRIM_RECORD_SIZE; ++i) {
    if (first[i] != second[i]) {
      return TRIM_FAULT;
    }
  }
  uint8_t status = TRIM_VERIFIED;
  for (unsigned i = 0; i < TRIM_COUNT; ++i) {
    unsigned code = 0;
    while (code < sizeof codewords && first[i] != codewords[code]) {
      ++code;
    }
    if (code == sizeof codewords) {
      status = TRIM_DEFAULT;
    } else {
      values[i] = (int8_t)((int)code - 3);
    }
  }
  return status;
}
