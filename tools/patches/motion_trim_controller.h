#ifndef DESK_MOTION_TRIM_CONTROLLER_H
#define DESK_MOTION_TRIM_CONTROLLER_H

#include "motion_trim_adapter.h"

enum { TRIM_REPLY_RESERVE = 37, TRIM_REPLY_WAIT = 100, TRIM_STORAGE_WINDOW = 1500 };

/* The offline linker supplies explicit parameters or leaves startup inactive.
 * Parameter presence alone is not EEPROM/timing qualification. */
void trim_controller_boot(void);

/* Called once, after native startup and before normal input processing. No
 * profile is selected here; all timing and EEPROM parameters are explicit.
 * This is boot setup, not a recovery or reinitialization entry point. */
bool trim_controller_start(const TrimI2cProfile* profile, uint8_t record_address,
                           uint32_t delivery);

/* Activate only after native startup/CL01 loading and successful initialization
 * of the adapter in its named platform slot. The caller has already started
 * its boot read with an independently qualified EEPROM/timing profile. */
bool trim_controller_activate(void);
bool trim_controller_started(void);
bool trim_controller_ready(void);
void trim_controller_poll(void);

/* Called before the old dispatcher, with a checksum-validated parser frame.
 * Returns one for every D9 frame, including malformed or unanswered requests.
 * While storage owns the bus, other kinds pass only for known reads and Stop
 * or button release. Blocked writes are consumed without deferred execution. */
int trim_controller_route(void);
bool trim_controller_blocks_native(void);
bool trim_controller_inputs_blocked(void);
uint8_t trim_controller_capture_native(void);

/* All legacy TX producers must use this reservation before activation. The
 * replacement encoder publishes the ring head only after a complete frame.
 * Input is the native FA-prefixed body and its body length, excluding FA/XOR.
 * It returns the native XOR result even when insufficient space drops a reply.
 * The legacy caller still owns its normal shared-sequence increment. */
bool trim_controller_tx_reserved(void);
uint32_t trim_controller_send_native(volatile uint8_t* ring, const volatile uint8_t* frame);

#ifdef DESK_DISPLAY_TIMEOUT
/* The caller holds PRIMASK across the capacity check, mutation and enqueue. */
bool trim_controller_reply_room(uint8_t needed);
void trim_controller_reply(const uint8_t* reply, uint8_t length);
#endif
#endif
