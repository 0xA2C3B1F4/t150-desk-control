#ifndef DESK_MOTION_TRIM_LAYOUT_H
#define DESK_MOTION_TRIM_LAYOUT_H

#include "motion_trim_i2c.h"
#include "motion_trim_adapter.h"
#include "motion_trim_owner.h"
#include "motion_trim_storage.h"
#include "platform_layout.h"

/* Reserved-space allocations extend ABI 1 without moving existing state.
 * The protocol transaction still needs its own size check. */
_Static_assert(sizeof(TrimOwnerState) <= PLATFORM_TRIM_OWNER_SIZE,
               "Trim owner exceeds its RAM allocation");
_Static_assert(PLATFORM_TRIM_OWNER_ADDRESS % _Alignof(TrimOwnerState) == 0,
               "Trim owner RAM allocation is misaligned");
_Static_assert(sizeof(TrimStorageState) <= PLATFORM_TRIM_RECORD_SIZE,
               "Trim record sequencer exceeds its RAM allocation");
_Static_assert(PLATFORM_TRIM_RECORD_ADDRESS % _Alignof(TrimStorageState) == 0,
               "Trim record RAM allocation is misaligned");
_Static_assert(sizeof(TrimI2cState) <= PLATFORM_TRIM_I2C_SIZE,
               "Trim I2C driver exceeds its RAM allocation");
_Static_assert(PLATFORM_TRIM_I2C_ADDRESS % _Alignof(TrimI2cState) == 0,
               "Trim I2C RAM allocation is misaligned");
_Static_assert(sizeof(TrimAdapterState) <= PLATFORM_TRIM_ADAPTER_SIZE,
               "Trim adapter exceeds its RAM allocation");
_Static_assert(PLATFORM_TRIM_ADAPTER_ADDRESS % _Alignof(TrimAdapterState) == 0,
               "Trim adapter RAM allocation is misaligned");

#endif
