#include "motion_trim_adapter.h"

static const uint8_t native_addresses[TRIM_NATIVE_BYTES] = {0x40, 0x41, 0x4a, 0x4b, 0x2a, 0x2b};

_Static_assert(TRIM_STORAGE_BOOT_READS == 8 && TRIM_STORAGE_MAX_READS == 10 &&
                   TRIM_STORAGE_MAX_WRITES == 1 && TRIM_STORAGE_MAX_OPERATIONS == 11,
               "Recalculate adapter admission when the record sequence changes");

bool trim_adapter_budget(const TrimI2cProfile* profile, uint32_t window, uint32_t delivery,
                         TrimAdapterBudget* budget) {
  TrimI2cBounds bounds;
  if (!budget || !trim_i2c_bounds(profile, &bounds) || window == 0 || window >= 0x80000000u ||
      delivery == 0 || delivery >= 0x80000000u) {
    return false;
  }
  /* G accounts for the next foreground admission. D includes delivery and the
   * exclusive-deadline tick. Acquisition occurs once along the optional path;
   * the owner's uniform byte bound also covers reacquisition for native work. */
  uint64_t gap = profile->max_poll_gap;
  uint64_t acquire = (uint64_t)bounds.acquire + delivery;
  uint64_t read = (uint64_t)bounds.read + gap + delivery;
  uint64_t write = (uint64_t)bounds.write + gap + delivery;
  uint64_t byte = acquire + (read > write ? read : write);
  uint64_t cleanup = (uint64_t)bounds.cleanup + gap + delivery;
  uint64_t reserve = 13 * byte + 2 * cleanup;
  uint64_t boot = acquire + TRIM_STORAGE_BOOT_READS * read + gap + reserve;
  uint64_t set =
      acquire + TRIM_STORAGE_MAX_READS * read + TRIM_STORAGE_MAX_WRITES * write + gap + reserve;
  if (boot >= window || set >= window) {
    return false;
  }
  *budget = (TrimAdapterBudget){
      {window, (uint32_t)byte, (uint32_t)cleanup}, (uint32_t)boot, (uint32_t)set, delivery};
  return true;
}

static bool record_range_valid(uint8_t address) {
  unsigned end = (unsigned)address + TRIM_RECORD_SIZE;
  if (end > 256 || (address <= 0x61 && end > 0x60)) {
    return false;
  }
  for (unsigned i = 0; i < TRIM_NATIVE_BYTES; ++i) {
    if (address <= native_addresses[i] && end > native_addresses[i]) {
      return false;
    }
  }
  return true;
}

bool trim_adapter_init(TrimAdapterState* state, TrimOwnerState* owner, TrimStorageState* record,
                       TrimI2cState* driver, const TrimI2cPort* port, const TrimI2cProfile* profile,
                       const TrimAdapterLock* lock, uint8_t record_address, uint32_t window,
                       uint32_t delivery) {
  TrimAdapterBudget budget;
  if (!state || !owner || !record || !driver || !port || !port->read || !port->write ||
      !port->lines_high || !port->now || !lock || !lock->enter || !lock->leave ||
      !record_range_valid(record_address) ||
      !trim_adapter_budget(profile, window, delivery, &budget)) {
    return false;
  }
  if (!trim_i2c_init(driver, port, profile)) {
    return false;
  }
  trim_owner_init(owner);
  trim_storage_init(record);
  *state = (TrimAdapterState){.owner = owner,
                              .record = record,
                              .driver = driver,
                              .lock = *lock,
                              .budget = budget,
                              .record_address = record_address,
                              .storage = TRIM_FAULT,
                              .handoff = 1};
  return true;
}

static uint32_t enter(TrimAdapterState* state) {
  return state->lock.enter(state->lock.context);
}

static void leave(TrimAdapterState* state, uint32_t previous) {
  state->lock.leave(state->lock.context, previous);
}

static uint32_t now_ticks(const TrimAdapterState* state) {
  return state->driver->port.now(state->driver->port.context);
}

static bool closed(const TrimAdapterState* state) {
  return trim_i2c_closed(state->driver) && !state->owner->outstanding;
}

static bool begin_locked(TrimAdapterState* state, uint8_t mode, const int8_t* values) {
  if (!closed(state) || state->late_valid ||
      (mode == TRIM_STORAGE_BOOT ? state->phase != TRIM_ADAPTER_IDLE
                                 : !state->boot_coherent || state->phase != TRIM_ADAPTER_DONE) ||
      (state->record->mode != TRIM_STORAGE_IDLE && state->record->mode != TRIM_STORAGE_TERMINAL)) {
    return false;
  }
  TrimStorageState prepared = *state->record;
  bool prepared_ok = mode == TRIM_STORAGE_BOOT ? trim_storage_begin_boot(&prepared)
                                               : trim_storage_begin_set(&prepared, values);
  if (!prepared_ok || !trim_owner_begin(state->owner, &state->budget.owner, now_ticks(state))) {
    return false;
  }
  *state->record = prepared;
  state->mode = mode;
  state->result = TRIM_OWNER_PENDING;
  state->handoff = 0;
  state->phase = TRIM_ADAPTER_WAIT;
  if (mode == TRIM_STORAGE_BOOT) {
    state->boot_coherent = 0;
  }
  return true;
}

bool trim_adapter_begin_boot(TrimAdapterState* state) {
  uint32_t previous = enter(state);
  bool accepted = begin_locked(state, TRIM_STORAGE_BOOT, 0);
  leave(state, previous);
  return accepted;
}

bool trim_adapter_begin_set(TrimAdapterState* state, const int8_t values[TRIM_COUNT]) {
  if (!values || !trim_values_valid(values)) {
    return false;
  }
  uint32_t previous = enter(state);
  bool accepted = begin_locked(state, TRIM_STORAGE_SET, values);
  leave(state, previous);
  return accepted;
}

static void cancel_record(TrimAdapterState* state) {
  if (state->owner->trim_cancelled) {
    trim_storage_cancel(state->record);
  }
}

static void publish_terminal(TrimAdapterState* state) {
  if (state->owner->phase != TRIM_OWNER_TERMINAL || state->phase == TRIM_ADAPTER_DONE) {
    return;
  }
  state->result = state->owner->result;
  if (state->result == TRIM_OWNER_OK) {
    for (unsigned i = 0; i < TRIM_COUNT; ++i) {
      state->active[i] = state->record->active[i];
    }
    state->storage = state->record->status;
    state->boot_coherent = 1;
  } else {
    if (state->mode == TRIM_STORAGE_BOOT) {
      for (unsigned i = 0; i < TRIM_COUNT; ++i) {
        state->active[i] = 0;
      }
      state->storage = TRIM_FAULT;
    } else {
      state->storage = TRIM_UNCERTAIN;
    }
    state->boot_coherent = 0;
  }
  state->handoff = state->owner->handoff && closed(state);
  state->phase = TRIM_ADAPTER_DONE;
}

static bool coherent_boot(const TrimStorageState* record) {
  for (unsigned i = 0; i < TRIM_RECORD_SIZE; ++i) {
    if (record->reads[0][i] != record->reads[1][i]) {
      return false;
    }
  }
  return true;
}

static void complete_step(TrimAdapterState* state, bool succeeded) {
  TrimOwnerState* owner = state->owner;
  if (!owner->outstanding || owner->operation.token != state->operation.token ||
      owner->operation.kind != state->operation.kind ||
      owner->operation.index != state->operation.index ||
      owner->operation.value != state->operation.value) {
    return;
  }
  TrimOwnerOperation unused;
  trim_owner_next(owner, now_ticks(state), &unused);
  cancel_record(state);
  uint8_t completion = succeeded ? TRIM_OWNER_STEP_OK : TRIM_OWNER_STEP_FAILED;
  if (state->operation.kind == TRIM_OWNER_TRIM_STEP) {
    if (state->owner->trim_cancelled) {
      trim_storage_cancel(state->record);
      completion = TRIM_OWNER_STEP_FAILED;
    } else if (!trim_storage_complete(state->record, &state->record_operation, succeeded,
                                      state->driver->value)) {
      completion = TRIM_OWNER_STEP_FAILED;
    } else if (state->record->mode == TRIM_STORAGE_TERMINAL) {
      bool verified = succeeded && (state->mode == TRIM_STORAGE_BOOT
                                        ? coherent_boot(state->record)
                                        : state->record->status == TRIM_VERIFIED ||
                                              state->record->status == TRIM_DEFAULT);
      completion = verified ? TRIM_OWNER_TRIM_VERIFIED : TRIM_OWNER_STEP_FAILED;
    }
  }
  trim_owner_complete(state->owner, &state->operation, now_ticks(state), completion,
                      state->driver->value);
  cancel_record(state);
  if (state->phase != TRIM_ADAPTER_DONE) {
    state->phase = TRIM_ADAPTER_WAIT;
  }
  publish_terminal(state);
}

static bool begin_byte(TrimAdapterState* state) {
  uint8_t address;
  bool write;
  uint8_t value;
  if (state->operation.kind == TRIM_OWNER_TRIM_STEP) {
    if (state->owner->trim_cancelled) {
      return false;
    }
    address = (uint8_t)(state->record_address + state->record_operation.offset);
    write = state->record_operation.kind == TRIM_STORAGE_WRITE;
    value = state->record_operation.value;
  } else {
    address = native_addresses[state->operation.index];
    write = state->operation.kind == TRIM_OWNER_NATIVE_WRITE;
    value = state->operation.value;
  }
  bool accepted = write ? trim_i2c_write(state->driver, address, value, state->driver_deadline)
                        : trim_i2c_read(state->driver, address, state->driver_deadline);
  if (accepted) {
    state->phase = TRIM_ADAPTER_BYTE;
  }
  return accepted;
}

static void submit(TrimAdapterState* state) {
  state->driver_deadline =
      state->owner->issued_at + state->owner->issued_limit - state->budget.delivery;
  bool accepted;
  if (state->operation.kind == TRIM_OWNER_BUS_CLEANUP) {
    accepted = trim_i2c_cleanup(state->driver, state->driver_deadline);
    if (accepted) {
      state->phase = TRIM_ADAPTER_CLEANUP;
    }
  } else if (state->operation.kind == TRIM_OWNER_TRIM_STEP &&
             !trim_storage_next(state->record, &state->record_operation)) {
    accepted = false;
  } else if (!state->driver->owned || state->driver->released) {
    accepted = trim_i2c_acquire(state->driver, state->driver_deadline);
    if (accepted) {
      state->phase = TRIM_ADAPTER_ACQUIRE;
    }
  } else {
    accepted = begin_byte(state);
  }
  if (!accepted) {
    complete_step(state, false);
  }
}

void trim_adapter_poll(TrimAdapterState* state) {
  uint32_t previous = enter(state);
  if (state->phase == TRIM_ADAPTER_IDLE || (state->phase == TRIM_ADAPTER_DONE && closed(state))) {
    leave(state, previous);
    return;
  }
  bool issued = trim_owner_next(state->owner, now_ticks(state), &state->operation);
  cancel_record(state);
  publish_terminal(state);
  if (issued) {
    submit(state);
  }
  bool polling = !trim_i2c_closed(state->driver);
  leave(state, previous);
  if (!polling) {
    return;
  }

  trim_i2c_poll(state->driver);

  previous = enter(state);
  uint32_t now = now_ticks(state);
  /* An issued driver action always retains an outstanding owner identity.
   * Observe its deadline before feeding a provisional record completion. */
  TrimOwnerOperation unused;
  trim_owner_next(state->owner, now, &unused);
  cancel_record(state);
  publish_terminal(state);
  if (trim_i2c_closed(state->driver)) {
    if (state->owner->phase == TRIM_OWNER_TERMINAL) {
      complete_step(state, false);
    } else if (state->phase == TRIM_ADAPTER_ACQUIRE && state->driver->result == TRIM_I2C_OK) {
      if (!begin_byte(state)) {
        complete_step(state, false);
      }
    } else {
      bool succeeded = state->driver->result == TRIM_I2C_OK;
      if (state->phase == TRIM_ADAPTER_CLEANUP) {
        succeeded = succeeded && state->driver->released;
      } else if (state->phase != TRIM_ADAPTER_BYTE) {
        succeeded = false;
      } else if (state->driver->operation_action == TRIM_I2C_READ &&
                 state->driver->operation_berr) {
        succeeded = false;
      }
      complete_step(state, succeeded);
    }
  }
  leave(state, previous);
}

static void retain_latest(TrimAdapterState* state, const uint8_t bytes[TRIM_NATIVE_BYTES]) {
  for (unsigned i = 0; i < TRIM_NATIVE_BYTES; ++i) {
    state->late_native[i] = bytes[i];
  }
  state->late_valid = 1;
}

uint8_t trim_adapter_capture_native(TrimAdapterState* state,
                                    const uint8_t bytes[TRIM_NATIVE_BYTES]) {
  uint32_t previous = enter(state);
  uint8_t result;
  if (trim_owner_capture(state->owner, bytes)) {
    result = TRIM_CAPTURE_QUEUED;
  } else if (state->handoff && closed(state)) {
    if (state->late_valid || trim_owner_native_unverified(state->owner)) {
      /* Original native writes provide no verification result. Keep the newest
       * unverified intent, never an older generation that could be replayed. */
      retain_latest(state, bytes);
    }
    result = TRIM_CAPTURE_PASSTHROUGH;
  } else {
    retain_latest(state, bytes);
    result = TRIM_CAPTURE_RETAINED;
  }
  leave(state, previous);
  return result;
}

bool trim_adapter_pending_native(TrimAdapterState* state, uint8_t bytes[TRIM_NATIVE_BYTES]) {
  uint32_t previous = enter(state);
  bool present = state->late_valid || trim_owner_native_unverified(state->owner);
  if (present) {
    const uint8_t* source = state->late_valid             ? state->late_native
                            : state->owner->pending_valid ? state->owner->native_pending
                                                          : state->owner->native_active;
    for (unsigned i = 0; i < TRIM_NATIVE_BYTES; ++i) {
      bytes[i] = source[i];
    }
  }
  leave(state, previous);
  return present;
}

void trim_adapter_view(TrimAdapterState* state, TrimAdapterView* view) {
  uint32_t previous = enter(state);
  for (unsigned i = 0; i < TRIM_COUNT; ++i) {
    view->active[i] = state->active[i];
  }
  view->storage = state->storage;
  view->result = state->result;
  view->busy = state->phase != TRIM_ADAPTER_IDLE && state->phase != TRIM_ADAPTER_DONE;
  view->closed = closed(state);
  view->handoff = state->handoff;
  view->boot_coherent = state->boot_coherent;
  leave(state, previous);
}
