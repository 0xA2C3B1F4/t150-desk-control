#ifdef DESK_DISPLAY_TIMEOUT
#include "display_controller.h"
#endif
#include "motion_trim_motion.h"
#include "platform_runtime.h"

#define WORD(address) (*(volatile uint32_t*)(address))
#define BYTE(address) (*(volatile uint8_t*)(address))
#define MOTION ((volatile TrimMotionState*)PLATFORM_TRIM_ADDRESS)

bool trim_motion_publish(const int8_t values[TRIM_COUNT]) {
  if (!values || !trim_values_valid(values) || !platform_ready()) {
    return false;
  }
  uint32_t packed = 0;
  for (unsigned i = 0; i < TRIM_COUNT; ++i) {
    packed |= (uint32_t)(uint8_t)values[i] << (8 * i);
  }
  /* A reader sees one complete old or new tuple, never mixed byte updates. */
  MOTION->values = packed;
  MOTION->magic = TRIM_MOTION_MAGIC;
  return true;
}

uint32_t trim_motion_target(uint32_t requested, uint32_t current) {
#ifdef DESK_DISPLAY_TIMEOUT
  display_controller_restart();
#endif
  int8_t values[TRIM_COUNT] = {0};
  uint8_t speed = 0;
  bool admitted = false;
  /* Platform readiness checks the MCU profile before touching upper SRAM. */
  if (platform_ready() && MOTION->magic == TRIM_MOTION_MAGIC && WORD(0x20000fe4u) == 0x52533031u &&
      WORD(0x20000850u) == 24 && WORD(0x20000854u) == 39 && WORD(0x20000160u) == 6 &&
      WORD(0x2000015cu) == 747) {
    uint8_t state = BYTE(0x20000fe0u);
    uint8_t mode = BYTE(0x2000088du);
    /* Preset recall already selected mode 1. DT02 selects it after this
     * calculation, so admit its idle mode with the same ordinary-motion guards. */
    admitted = (state == 0 || state == 4) && mode <= 1 && !BYTE(0x2000088fu) &&
               !BYTE(0x20000890u) && !BYTE(0x200008b8u) && !BYTE(0x20000148u);
    speed = BYTE(0x20000fe2u);
    uint32_t packed = MOTION->values;
    for (unsigned i = 0; i < TRIM_COUNT; ++i) {
      unsigned byte = (packed >> (8 * i)) & 0xffu;
      values[i] = (int8_t)(byte < 128 ? (int)byte : (int)byte - 256);
    }
  }
  return trim_target(requested, current, values, speed, admitted);
}
