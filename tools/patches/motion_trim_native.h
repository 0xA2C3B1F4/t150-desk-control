#ifndef DESK_MOTION_TRIM_NATIVE_H
#define DESK_MOTION_TRIM_NATIVE_H

#include "motion_trim_adapter.h"

/* The controller checks readiness and supplies the initialized adapter. The
 * entry shim masks interrupts before these request-time RAM loads. This call
 * captures intent only; it performs no I2C operation or persistence claim. */
uint8_t trim_native_capture(TrimAdapterState* state);

#endif
