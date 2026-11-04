#include "motion_trim_native.h"

enum { NATIVE_MODE_RESETTING = 2, NATIVE_MODE_RESET_REQUIRED = 10 };
_Static_assert(TRIM_CAPTURE_PASSTHROUGH == 1, "Native shim disposition changed");

static uint32_t native_position(uint32_t bits) {
  uint32_t exponent = (bits >> 23) & 255u;
  if (exponent < 127u) {
    return 0;
  }
  uint32_t significand = (bits & 0x7FFFFFu) | 0x800000u;
  if (exponent <= 150u) {
    return significand >> (150u - exponent);
  }
  uint32_t shift = exponent - 150u;
  return shift < 32u ? significand << shift : 0;
}

uint8_t trim_native_capture(TrimAdapterState* state) {
  /* Match 2EDE's bit conversion, including its ignored sign and large shifts.
   * A floating-point cast would change negative, NaN and overflow behavior. */
  uint32_t position = native_position(*(volatile uint32_t*)0x200007D4u);
  uint32_t height = *(volatile uint32_t*)0x20000878u;
  uint8_t mode = *(volatile uint8_t*)0x2000088Du;
  uint8_t bytes[TRIM_NATIVE_BYTES] = {
      (uint8_t)(position >> 8),
      (uint8_t)position,
      (uint8_t)(height >> 8),
      (uint8_t)height,
      0,
      mode == NATIVE_MODE_RESETTING ? NATIVE_MODE_RESET_REQUIRED : mode,
  };
  return trim_adapter_capture_native(state, bytes);
}
