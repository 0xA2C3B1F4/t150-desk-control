#include "motion_trim_storage.h"

enum { INITIAL_READS, VALUE_WRITE, VERIFY_READS };

static bool is_active(const TrimStorageState* state) {
  return state->mode == TRIM_STORAGE_BOOT || state->mode == TRIM_STORAGE_SET;
}

static bool can_begin(const TrimStorageState* state) {
  return state->mode == TRIM_STORAGE_IDLE || state->mode == TRIM_STORAGE_TERMINAL;
}

static void zero_active(TrimStorageState* state) {
  for (unsigned i = 0; i < TRIM_COUNT; ++i) {
    state->active[i] = 0;
  }
}

static void begin(TrimStorageState* state, uint8_t mode) {
  state->mode = mode;
  state->phase = INITIAL_READS;
  state->index = 0;
  state->outstanding = 0;
  state->write_issued = 0;
  for (unsigned pass = 0; pass < 2; ++pass) {
    for (unsigned i = 0; i < TRIM_RECORD_SIZE; ++i) {
      state->reads[pass][i] = 0;
    }
  }
}

void trim_storage_init(TrimStorageState* state) {
  *state = (TrimStorageState){0};
  state->selected = TRIM_COUNT;
  state->status = TRIM_DEFAULT;
}

bool trim_storage_begin_boot(TrimStorageState* state) {
  if (!can_begin(state)) {
    return false;
  }
  begin(state, TRIM_STORAGE_BOOT);
  zero_active(state);
  state->status = TRIM_DEFAULT;
  return true;
}

bool trim_storage_begin_set(TrimStorageState* state, const int8_t values[TRIM_COUNT]) {
  if (!can_begin(state) || !trim_values_valid(values) ||
      (state->status != TRIM_DEFAULT && state->status != TRIM_VERIFIED)) {
    return false;
  }
  unsigned selected = TRIM_COUNT;
  for (unsigned i = 0; i < TRIM_COUNT; ++i) {
    if (values[i] != state->active[i]) {
      if (selected != TRIM_COUNT) {
        return false;
      }
      selected = i;
    }
  }
  trim_record_encode(values, state->desired);
  begin(state, TRIM_STORAGE_SET);
  state->selected = (uint8_t)selected;
  return true;
}

static TrimStorageOperation current_operation(const TrimStorageState* state) {
  TrimStorageOperation operation = {state->token, TRIM_STORAGE_READ, state->selected, 0};
  if (state->phase == INITIAL_READS) {
    operation.offset = (uint8_t)(state->index % TRIM_RECORD_SIZE);
  } else if (state->phase == VALUE_WRITE) {
    operation.kind = TRIM_STORAGE_WRITE;
    operation.value = state->desired[state->selected];
  }
  return operation;
}

bool trim_storage_next(TrimStorageState* state, TrimStorageOperation* operation) {
  if (!is_active(state)) {
    return false;
  }
  if (!state->outstanding) {
    ++state->token;
    if (state->token == 0) {
      ++state->token;
    }
    state->outstanding = 1;
  }
  *operation = current_operation(state);
  if (operation->kind == TRIM_STORAGE_WRITE) {
    state->write_issued = 1;
  }
  return true;
}

static void fail(TrimStorageState* state) {
  if (state->mode == TRIM_STORAGE_BOOT) {
    zero_active(state);
    state->status = TRIM_FAULT;
  } else {
    state->status = TRIM_UNCERTAIN;
  }
  state->mode = TRIM_STORAGE_TERMINAL;
  state->outstanding = 0;
}

static void finish_initial_reads(TrimStorageState* state) {
  int8_t values[TRIM_COUNT];
  uint8_t status = trim_record_decode(state->reads[0], state->reads[1], values);
  if (status == TRIM_FAULT) {
    fail(state);
    return;
  }
  if (state->mode == TRIM_STORAGE_BOOT) {
    for (unsigned i = 0; i < TRIM_COUNT; ++i) {
      state->active[i] = values[i];
    }
  } else {
    /* The controller's expected-value check used this RAM snapshot. Do not
     * overwrite a byte if the fresh EEPROM reads contradict that snapshot. */
    for (unsigned i = 0; i < TRIM_COUNT; ++i) {
      if (state->active[i] != values[i]) {
        fail(state);
        return;
      }
    }
    if (state->selected != TRIM_COUNT) {
      state->phase = VALUE_WRITE;
      state->index = 0;
      return;
    }
  }
  state->status = status;
  state->mode = TRIM_STORAGE_TERMINAL;
}

bool trim_storage_complete(TrimStorageState* state, const TrimStorageOperation* operation,
                           bool succeeded, uint8_t read_value) {
  if (!is_active(state) || !state->outstanding) {
    return false;
  }
  TrimStorageOperation expected = current_operation(state);
  if (operation->token != expected.token || operation->kind != expected.kind ||
      operation->offset != expected.offset ||
      (expected.kind == TRIM_STORAGE_WRITE && operation->value != expected.value)) {
    return false;
  }
  state->outstanding = 0;
  if (!succeeded) {
    fail(state);
    return true;
  }
  if (state->phase == INITIAL_READS) {
    state->reads[state->index / TRIM_RECORD_SIZE][state->index % TRIM_RECORD_SIZE] = read_value;
    if (++state->index == TRIM_STORAGE_BOOT_READS) {
      finish_initial_reads(state);
    }
  } else if (state->phase == VALUE_WRITE) {
    state->phase = VERIFY_READS;
    state->index = 0;
  } else if (read_value != state->desired[state->selected]) {
    fail(state);
  } else {
    state->reads[state->index][state->selected] = read_value;
    if (++state->index == 2) {
      state->status = trim_record_decode(state->reads[0], state->reads[1], state->active);
      state->mode = TRIM_STORAGE_TERMINAL;
    }
  }
  return true;
}

bool trim_storage_cancel(TrimStorageState* state) {
  if (!is_active(state)) {
    return false;
  }
  fail(state);
  return true;
}
