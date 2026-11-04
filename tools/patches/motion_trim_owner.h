#ifndef DESK_MOTION_TRIM_OWNER_H
#define DESK_MOTION_TRIM_OWNER_H

#include <stdbool.h>
#include <stdint.h>

enum { TRIM_NATIVE_BYTES = 6 };
enum {
  TRIM_OWNER_IDLE,
  TRIM_OWNER_TRIM,
  TRIM_OWNER_CLEANUP,
  TRIM_OWNER_DRAIN,
  TRIM_OWNER_TERMINAL
};
enum { TRIM_OWNER_PENDING, TRIM_OWNER_OK, TRIM_OWNER_STORAGE_ERROR, TRIM_OWNER_UNRELEASED };
enum {
  TRIM_OWNER_NO_FAULT,
  TRIM_OWNER_PREEMPTED,
  TRIM_OWNER_TRIM_ERROR,
  TRIM_OWNER_TRIM_BUDGET,
  TRIM_OWNER_NATIVE_ERROR,
  TRIM_OWNER_VERIFY_ERROR,
  TRIM_OWNER_DEADLINE,
  TRIM_OWNER_IDENTITY_EXHAUSTED,
  TRIM_OWNER_CLEANUP_ERROR
};
enum {
  TRIM_OWNER_TRIM_STEP,
  TRIM_OWNER_NATIVE_WRITE,
  TRIM_OWNER_NATIVE_READ,
  TRIM_OWNER_BUS_CLEANUP
};
enum { TRIM_OWNER_STEP_FAILED, TRIM_OWNER_STEP_OK, TRIM_OWNER_TRIM_VERIFIED };

typedef struct {
  uint32_t window;
  uint32_t byte_limit;
  uint32_t cleanup_limit;
} TrimOwnerLimits;

typedef struct {
  uint32_t token;
  uint8_t kind;
  uint8_t index;
  uint8_t value;
} TrimOwnerOperation;

/* All calls require caller-provided serialization. No function masks interrupts
 * or accesses hardware. The adapter owns the trim sequencer and byte driver.
 * Native indices map to 40,41,4A,4B,2A,2B in that order, not trim record offsets.
 * Only read fields; changing them bypasses admission and lifetime checks. */
typedef struct {
  TrimOwnerLimits limits;
  TrimOwnerOperation operation;
  uint32_t started;
  uint32_t issued_at;
  uint32_t issued_limit;
  uint32_t token;
  uint32_t generation;
  uint8_t native_active[TRIM_NATIVE_BYTES];
  uint8_t native_pending[TRIM_NATIVE_BYTES];
  uint8_t phase;
  uint8_t result;
  uint8_t reason;
  uint8_t outstanding;
  uint8_t pending_valid;
  uint8_t active_valid;
  uint8_t active_verified;
  uint8_t native_index;
  uint8_t native_failed;
  uint8_t trim_verified;
  uint8_t trim_cancelled;
  uint8_t bus_ready;
  uint8_t handoff;
} TrimOwnerState;

/* Separate from the record sequencer, byte driver and protocol transaction.
 * Firmware integration must allocate the combined state in the shared layout. */
_Static_assert(sizeof(TrimOwnerState) <= 72, "Trim owner exceeds its state budget");

/* Initialize only with no live or deliverable old callbacks. This is not bus
 * recovery. Initial admission requires an independently verified idle bus. */
void trim_owner_init(TrimOwnerState* state);

/* Reject invalid budgets and a faulted/nonterminal previous request unchanged.
 * A successful request may be followed by another; identities are not reset.
 * The caller has already checked token, stopped admission and native owners.
 * All durations use one monotonic tick source; window must be below 2^31 ticks.
 * The ownership deadline is exclusive: completion at started+window is late.
 * Byte limits include programming/abort completion. Cleanup includes STOP,
 * released lines, idle peripheral and verified native bus configuration. */
bool trim_owner_begin(TrimOwnerState* state, const TrimOwnerLimits* limits, uint32_t now);

/* Capture the exact native snapshot. The active generation never changes.
 * False is NOT permission for native pass-through: inspect handoff separately.
 * trim_cancelled tells the adapter to cancel the record sequencer while still
 * draining its issued byte. Do not expose its provisional tuple to motion. */
bool trim_owner_capture(TrimOwnerState* state, const uint8_t bytes[TRIM_NATIVE_BYTES]);

/* Issue at most one action. False leaves operation untouched. An outstanding
 * action is never reissued. Call periodically even while awaiting completion
 * so deadlines terminate the request. False can also follow an internal state
 * transition with more work ready; it does not mean a callback is pending.
 * Terminal states never issue actions. */
bool trim_owner_next(TrimOwnerState* state, uint32_t now, TrimOwnerOperation* operation);

/* Each matching completion is consumed once. STEP_OK for a write means EEPROM
 * programming finished. TRIM_VERIFIED is allowed only for TRIM_STEP, after SET
 * verification or two successful coherent boot read passes. The boot record's
 * decode status is separate: coherent invalid bytes select per-trim defaults.
 * STEP_FAILED includes failed transfers and mismatched read passes.
 * After a terminal deadline, a matching completion only clears outstanding;
 * it cannot change the outcome, retained bytes or handoff. No new transaction
 * is admitted after a fault. The adapter must close callback lifetime before
 * explicit recovery/reinitialization, including after a late completion. */
bool trim_owner_complete(TrimOwnerState* state, const TrimOwnerOperation* operation, uint32_t now,
                         uint8_t completion, uint8_t read_value);

/* True means a native generation remains unverified. Prefer native_pending if
 * pending_valid; otherwise native_active holds the latest attempted snapshot.
 * A storage error does not prove the old trim record survived; recovery must
 * read it again before reporting which tuple is persistent.
 * handoff certifies only bus release, never persistence. An unreleased terminal
 * result needs a separately reviewed live fault policy before firmware use.
 * This module rejects captures after any terminal result; the live policy must
 * route or retain subsequent mandatory requests instead of dropping them. */
bool trim_owner_native_unverified(const TrimOwnerState* state);

#endif
