#ifndef DESK_MOTION_TRIM_ADAPTER_H
#define DESK_MOTION_TRIM_ADAPTER_H

#include "motion_trim_i2c.h"
#include "motion_trim_owner.h"
#include "motion_trim_storage.h"

enum { TRIM_CAPTURE_QUEUED, TRIM_CAPTURE_PASSTHROUGH, TRIM_CAPTURE_RETAINED };
enum {
  TRIM_ADAPTER_IDLE,
  TRIM_ADAPTER_WAIT,
  TRIM_ADAPTER_ACQUIRE,
  TRIM_ADAPTER_BYTE,
  TRIM_ADAPTER_CLEANUP,
  TRIM_ADAPTER_DONE
};

typedef struct {
  uint32_t (*enter)(void* context);
  void (*leave)(void* context, uint32_t previous);
  void* context;
} TrimAdapterLock;

typedef struct {
  TrimOwnerLimits owner;
  uint32_t boot_required;
  uint32_t set_required;
  uint32_t delivery;
} TrimAdapterBudget;

typedef struct {
  int8_t active[TRIM_COUNT];
  uint8_t storage;
  uint8_t result;
  uint8_t busy;
  uint8_t closed;
  uint8_t handoff;
  uint8_t boot_coherent;
} TrimAdapterView;

/* All objects and callback contexts outlive the adapter. Only poll changes the
 * driver; native ISR captures use the same short lock as state transitions.
 * The clock callback must be a bounded load usable with interrupts masked.
 * Keep sleep, clock reset and native I2C callers excluded during ownership.
 * No function validates EEPROM electrical timing or page/ECC isolation. */
typedef struct {
  TrimOwnerState* owner;
  TrimStorageState* record;
  TrimI2cState* driver;
  TrimAdapterLock lock;
  TrimAdapterBudget budget;
  TrimOwnerOperation operation;
  TrimStorageOperation record_operation;
  uint32_t driver_deadline;
  int8_t active[TRIM_COUNT];
  uint8_t late_native[TRIM_NATIVE_BYTES];
  uint8_t late_valid;
  uint8_t record_address;
  uint8_t phase;
  uint8_t mode;
  uint8_t storage;
  uint8_t result;
  uint8_t handoff;
  uint8_t boot_coherent;
} TrimAdapterState;

/* delivery >= 1 + maximum ticks from the driver's last clock sample to the
 * fresh sample delivered to the owner. Every call must meet max_poll_gap.
 * Bounds include a complete healthy record plus the owner's native reserve.
 * Invalid arguments leave output unchanged; all times use one clock. */
bool trim_adapter_budget(const TrimI2cProfile* profile, uint32_t window, uint32_t delivery,
                         TrimAdapterBudget* budget);

/* Boot initialization only, not recovery of an active or faulted peripheral.
 * Reject invalid profiles/overlapping record ranges without changing objects. */
bool trim_adapter_init(TrimAdapterState* state, TrimOwnerState* owner, TrimStorageState* record,
                       TrimI2cState* driver, const TrimI2cPort* port, const TrimI2cProfile* profile,
                       const TrimAdapterLock* lock, uint8_t record_address, uint32_t window,
                       uint32_t delivery);
bool trim_adapter_begin_boot(TrimAdapterState* state);
bool trim_adapter_begin_set(TrimAdapterState* state, const int8_t values[TRIM_COUNT]);
void trim_adapter_poll(TrimAdapterState* state);
void trim_adapter_view(TrimAdapterState* state, TrimAdapterView* view);

/* Only PASSTHROUGH permits the original native path. RETAINED is a terminal
 * unreleased fault: retain the newest snapshot and keep bus access excluded. */
uint8_t trim_adapter_capture_native(TrimAdapterState* state,
                                    const uint8_t bytes[TRIM_NATIVE_BYTES]);
/* Latest unverified intent, not a replay command or proof of EEPROM contents.
 * Clean-fault pass-through keeps this current, but cannot certify persistence.
 * Such a generation must be resolved independently before adapter recovery. */
bool trim_adapter_pending_native(TrimAdapterState* state, uint8_t bytes[TRIM_NATIVE_BYTES]);

#endif
