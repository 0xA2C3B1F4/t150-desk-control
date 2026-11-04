#ifndef DESK_MOTION_TRIM_MOTION_H
#define DESK_MOTION_TRIM_MOTION_H

#include "motion_trim_core.h"
#include "platform_layout.h"

#define TRIM_MOTION_MAGIC 0x54523031u

typedef struct {
  uint32_t values;
  uint32_t magic;
} TrimMotionState;

_Static_assert(sizeof(TrimMotionState) <= PLATFORM_TRIM_SIZE, "Trim motion RAM allocation");
_Static_assert(PLATFORM_TRIM_ADDRESS % 4 == 0, "Trim tuple must use aligned word access");

/* Call only after the adapter publishes its final active tuple. Boot may
 * publish zeros for Default/Fault. A failed SET keeps the previous tuple. */
bool trim_motion_publish(const int8_t values[TRIM_COUNT]);
uint32_t trim_motion_target(uint32_t requested, uint32_t current);

#endif
