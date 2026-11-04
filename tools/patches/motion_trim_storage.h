#ifndef DESK_MOTION_TRIM_STORAGE_H
#define DESK_MOTION_TRIM_STORAGE_H

#include "motion_trim_core.h"

enum { TRIM_STORAGE_NONE = 0, TRIM_STORAGE_READ = 1, TRIM_STORAGE_WRITE = 2 };
enum {
  TRIM_STORAGE_IDLE = 0,
  TRIM_STORAGE_BOOT = 1,
  TRIM_STORAGE_SET = 2,
  TRIM_STORAGE_TERMINAL = 3
};
enum {
  TRIM_STORAGE_BOOT_READS = 2 * TRIM_RECORD_SIZE,
  TRIM_STORAGE_MAX_READS = TRIM_STORAGE_BOOT_READS + 2,
  TRIM_STORAGE_MAX_WRITES = 1,
  TRIM_STORAGE_MAX_OPERATIONS = TRIM_STORAGE_MAX_READS + TRIM_STORAGE_MAX_WRITES
};

typedef struct {
  uint32_t token;
  uint8_t kind;
  uint8_t offset;
  uint8_t value;
} TrimStorageOperation;

/* Caller-owned state. Inspect active, status and write_issued, but do not edit
 * fields between init and the final result. Status is final in idle/terminal.
 * The size limit covers only this sequencer, not a future driver transaction. */
typedef struct {
  uint32_t token;
  int8_t active[TRIM_COUNT];
  uint8_t desired[TRIM_RECORD_SIZE];
  uint8_t reads[2][TRIM_RECORD_SIZE];
  uint8_t mode;
  uint8_t phase;
  uint8_t index;
  uint8_t outstanding;
  uint8_t write_issued;
  uint8_t status;
  uint8_t selected;
} TrimStorageState;

_Static_assert(sizeof(TrimStorageState) <= 64, "Trim record sequencer exceeds its state budget");

void trim_storage_init(TrimStorageState* state);
bool trim_storage_begin_boot(TrimStorageState* state);
/* Admit at most one changed slot. A no-op reads without writing, including
 * when zero came from an invalid saved byte. Reject Fault/Uncertain state. */
bool trim_storage_begin_set(TrimStorageState* state, const int8_t values[TRIM_COUNT]);

/* Each request addresses a record byte, never a physical bus address. Repeated
 * next calls return the outstanding request unchanged. Idle/terminal returns
 * false without changing operation. Issuing a write sets write_issued even if
 * it later fails or is cancelled before completion. */
bool trim_storage_next(TrimStorageState* state, TrimStorageOperation* operation);

/* Success for a write means programming has finished. Read data is ignored for
 * writes. A mismatched token, kind, offset or write value is rejected unchanged.
 * Failure terminates without retry or rollback. SET retains the old RAM tuple.
 * A failed or cancelled boot gives Fault and zero trims.
 * The caller holds exclusive storage ownership throughout, and must finish bus
 * cleanup and mandatory saves before releasing it. No API does that work here.
 * Drain/cancel a previous asynchronous operation before reusing this state;
 * request identity does not establish callback lifetime or electrical cleanup. */
bool trim_storage_complete(TrimStorageState* state, const TrimStorageOperation* operation,
                           bool succeeded, uint8_t read_value);
bool trim_storage_cancel(TrimStorageState* state);

#endif
