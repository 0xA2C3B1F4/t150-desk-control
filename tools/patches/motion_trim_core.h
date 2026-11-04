#ifndef DESK_MOTION_TRIM_CORE_H
#define DESK_MOTION_TRIM_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { TRIM_COUNT = 4, TRIM_RECORD_SIZE = TRIM_COUNT };
enum { TRIM_DEFAULT = 0, TRIM_VERIFIED = 1, TRIM_FAULT = 2, TRIM_UNCERTAIN = 3 };

/* Values are Normal up/down, Slow up/down, in controller units, not mm. */
bool trim_values_valid(const int8_t values[TRIM_COUNT]);

/* Pure calculation only. The hook must independently establish the profile,
 * magic, storage readiness and ordinary target-motion admission on every call.
 * requested/current hold the original 32-bit register bits. Comparisons are
 * signed; additions wrap exactly as native Thumb instructions do. */
uint32_t trim_target(uint32_t requested, uint32_t current, const int8_t values[TRIM_COUNT],
                     uint8_t speed, bool admitted);

/* No I2C, EEPROM address or write ordering is selected here. Invalid input
 * leaves record untouched. Decode requires two successful complete reads;
 * the caller must reject bus errors before calling it, even for all-zero data.
 * Each invalid codeword selects zero for that slot. Matching invalid content
 * is Default; inconsistent reads are Fault and leave all values zero. */
bool trim_record_encode(const int8_t values[TRIM_COUNT], uint8_t record[TRIM_RECORD_SIZE]);
uint8_t trim_record_decode(const uint8_t first[TRIM_RECORD_SIZE],
                           const uint8_t second[TRIM_RECORD_SIZE], int8_t values[TRIM_COUNT]);

#endif
