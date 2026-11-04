#include "motion_trim_owner.h"

static bool running(const TrimOwnerState* state) {
  return state->phase != TRIM_OWNER_IDLE && state->phase != TRIM_OWNER_TERMINAL;
}

static bool equal_snapshot(const uint8_t first[TRIM_NATIVE_BYTES],
                           const uint8_t second[TRIM_NATIVE_BYTES]) {
  for (unsigned i = 0; i < TRIM_NATIVE_BYTES; ++i) {
    if (first[i] != second[i]) {
      return false;
    }
  }
  return true;
}

static uint32_t remaining(const TrimOwnerState* state, uint32_t now) {
  uint32_t elapsed = now - state->started;
  return elapsed < state->limits.window ? state->limits.window - elapsed : 0;
}

bool trim_owner_native_unverified(const TrimOwnerState* state) {
  return state->pending_valid || (state->active_valid && !state->active_verified);
}

static void terminal(TrimOwnerState* state) {
  state->phase = TRIM_OWNER_TERMINAL;
  state->handoff = state->bus_ready && !state->outstanding;
  if (!state->handoff) {
    state->result = TRIM_OWNER_UNRELEASED;
  } else if (state->trim_verified && !state->trim_cancelled &&
             !trim_owner_native_unverified(state)) {
    state->result = TRIM_OWNER_OK;
  } else {
    state->result = TRIM_OWNER_STORAGE_ERROR;
  }
}

static void fail(TrimOwnerState* state, uint8_t reason) {
  state->reason = reason;
  state->trim_cancelled = 1;
  if (reason != TRIM_OWNER_TRIM_ERROR && reason != TRIM_OWNER_TRIM_BUDGET) {
    state->native_failed = 1;
  }
  state->phase = TRIM_OWNER_CLEANUP;
}

static void observe_deadline(TrimOwnerState* state, uint32_t now) {
  if (!running(state)) {
    return;
  }
  if (remaining(state, now) == 0 ||
      (state->outstanding && now - state->issued_at > state->issued_limit)) {
    fail(state, TRIM_OWNER_DEADLINE);
    terminal(state);
  }
}

void trim_owner_init(TrimOwnerState* state) {
  state->limits = (TrimOwnerLimits){0, 0, 0};
  state->operation = (TrimOwnerOperation){0, 0, 0, 0};
  state->started = 0;
  state->issued_at = 0;
  state->issued_limit = 0;
  state->token = 0;
  state->generation = 0;
  for (unsigned i = 0; i < TRIM_NATIVE_BYTES; ++i) {
    state->native_active[i] = 0;
    state->native_pending[i] = 0;
  }
  state->phase = TRIM_OWNER_IDLE;
  state->result = TRIM_OWNER_PENDING;
  state->reason = TRIM_OWNER_NO_FAULT;
  state->outstanding = 0;
  state->pending_valid = 0;
  state->active_valid = 0;
  state->active_verified = 0;
  state->native_index = 0;
  state->native_failed = 0;
  state->trim_verified = 0;
  state->trim_cancelled = 0;
  state->bus_ready = 0;
  state->handoff = 0;
}

bool trim_owner_begin(TrimOwnerState* state, const TrimOwnerLimits* limits, uint32_t now) {
  /* Division-first bounds make the later reserve arithmetic safe in uint32_t. */
  if (limits->window == 0 || limits->window >= UINT32_C(0x80000000) || limits->byte_limit == 0 ||
      limits->cleanup_limit == 0 || limits->byte_limit > limits->window / 13 ||
      limits->cleanup_limit > (limits->window - 13 * limits->byte_limit) / 2 ||
      13 * limits->byte_limit + 2 * limits->cleanup_limit >= limits->window || state->outstanding ||
      (state->phase != TRIM_OWNER_IDLE && !(state->phase == TRIM_OWNER_TERMINAL &&
                                            state->result == TRIM_OWNER_OK && state->handoff))) {
    return false;
  }
  /* Lifetime identities deliberately survive requests. */
  state->limits = *limits;
  state->started = now;
  state->phase = TRIM_OWNER_TRIM;
  state->result = TRIM_OWNER_PENDING;
  state->reason = TRIM_OWNER_NO_FAULT;
  state->pending_valid = 0;
  state->active_valid = 0;
  state->active_verified = 0;
  state->native_index = 0;
  state->native_failed = 0;
  state->trim_verified = 0;
  state->trim_cancelled = 0;
  state->bus_ready = 1;
  state->handoff = 0;
  return true;
}

bool trim_owner_capture(TrimOwnerState* state, const uint8_t bytes[TRIM_NATIVE_BYTES]) {
  if (!running(state)) {
    return false;
  }
  for (unsigned i = 0; i < TRIM_NATIVE_BYTES; ++i) {
    state->native_pending[i] = bytes[i];
  }
  state->pending_valid = 1;
  state->trim_cancelled = 1;
  if (state->reason == TRIM_OWNER_NO_FAULT) {
    state->reason = TRIM_OWNER_PREEMPTED;
  }
  if (state->phase == TRIM_OWNER_TRIM) {
    state->phase = TRIM_OWNER_CLEANUP;
  }
  return true;
}

static void after_cleanup(TrimOwnerState* state, uint32_t now) {
  if (state->active_verified && state->pending_valid &&
      equal_snapshot(state->native_active, state->native_pending)) {
    state->pending_valid = 0;
  }
  if (state->native_failed || !state->pending_valid) {
    terminal(state);
    return;
  }
  uint32_t cost = 12 * state->limits.byte_limit + state->limits.cleanup_limit;
  if (cost >= remaining(state, now) || state->generation == UINT32_MAX) {
    fail(state,
         state->generation == UINT32_MAX ? TRIM_OWNER_IDENTITY_EXHAUSTED : TRIM_OWNER_DEADLINE);
    terminal(state);
    return;
  }
  ++state->generation;
  for (unsigned i = 0; i < TRIM_NATIVE_BYTES; ++i) {
    state->native_active[i] = state->native_pending[i];
  }
  state->pending_valid = 0;
  state->active_valid = 1;
  state->active_verified = 0;
  state->native_index = 0;
  state->phase = TRIM_OWNER_DRAIN;
}

static bool issue(TrimOwnerState* state, uint32_t now, uint8_t kind, uint8_t index, uint8_t value,
                  TrimOwnerOperation* operation) {
  if (state->token == UINT32_MAX) {
    fail(state, TRIM_OWNER_IDENTITY_EXHAUSTED);
    terminal(state);
    return false;
  }
  state->operation = (TrimOwnerOperation){++state->token, kind, index, value};
  state->issued_at = now;
  state->issued_limit =
      kind == TRIM_OWNER_BUS_CLEANUP ? state->limits.cleanup_limit : state->limits.byte_limit;
  state->outstanding = 1;
  state->bus_ready = 0;
  *operation = state->operation;
  return true;
}

bool trim_owner_next(TrimOwnerState* state, uint32_t now, TrimOwnerOperation* operation) {
  observe_deadline(state, now);
  if (!running(state) || state->outstanding) {
    return false;
  }
  uint32_t available = remaining(state, now);
  /* Preserve one identity for cleanup even when byte identities are exhausted. */
  if (state->phase != TRIM_OWNER_CLEANUP && state->token >= UINT32_MAX - 1) {
    fail(state, TRIM_OWNER_IDENTITY_EXHAUSTED);
  }
  if (state->phase == TRIM_OWNER_TRIM &&
      13 * state->limits.byte_limit + 2 * state->limits.cleanup_limit >= available) {
    fail(state, TRIM_OWNER_TRIM_BUDGET);
  }
  if (state->phase == TRIM_OWNER_DRAIN &&
      (2 * TRIM_NATIVE_BYTES - state->native_index) * state->limits.byte_limit +
              state->limits.cleanup_limit >=
          available) {
    fail(state, TRIM_OWNER_DEADLINE);
  }
  if (state->phase == TRIM_OWNER_CLEANUP) {
    if (state->bus_ready) {
      after_cleanup(state, now);
      return false;
    }
    if (state->limits.cleanup_limit >= available) {
      fail(state, TRIM_OWNER_DEADLINE);
      terminal(state);
      return false;
    }
    return issue(state, now, TRIM_OWNER_BUS_CLEANUP, 0, 0, operation);
  }
  if (state->phase == TRIM_OWNER_TRIM) {
    return issue(state, now, TRIM_OWNER_TRIM_STEP, 0, 0, operation);
  }
  uint8_t index = state->native_index % TRIM_NATIVE_BYTES;
  bool writing = state->native_index < TRIM_NATIVE_BYTES;
  return issue(state, now, writing ? TRIM_OWNER_NATIVE_WRITE : TRIM_OWNER_NATIVE_READ, index,
               writing ? state->native_active[index] : 0, operation);
}

static bool matches(const TrimOwnerOperation* first, const TrimOwnerOperation* second) {
  return first->token == second->token && first->kind == second->kind &&
         first->index == second->index && first->value == second->value;
}

bool trim_owner_complete(TrimOwnerState* state, const TrimOwnerOperation* operation, uint32_t now,
                         uint8_t completion, uint8_t read_value) {
  if (!state->outstanding || !matches(operation, &state->operation) ||
      completion > TRIM_OWNER_TRIM_VERIFIED ||
      (completion == TRIM_OWNER_TRIM_VERIFIED && operation->kind != TRIM_OWNER_TRIM_STEP)) {
    return false;
  }
  observe_deadline(state, now);
  state->outstanding = 0;
  if (state->phase == TRIM_OWNER_TERMINAL) {
    return true;
  }
  if (operation->kind == TRIM_OWNER_BUS_CLEANUP) {
    if (completion == TRIM_OWNER_STEP_FAILED) {
      fail(state, TRIM_OWNER_CLEANUP_ERROR);
      terminal(state);
    } else {
      state->bus_ready = 1;
      after_cleanup(state, now);
    }
    return true;
  }
  if (operation->kind == TRIM_OWNER_TRIM_STEP) {
    if (state->trim_cancelled) {
      state->phase = TRIM_OWNER_CLEANUP;
    } else if (completion == TRIM_OWNER_STEP_FAILED) {
      fail(state, TRIM_OWNER_TRIM_ERROR);
    } else if (completion == TRIM_OWNER_TRIM_VERIFIED) {
      state->trim_verified = 1;
      state->phase = TRIM_OWNER_CLEANUP;
    }
    return true;
  }
  if (completion == TRIM_OWNER_STEP_FAILED) {
    fail(state, TRIM_OWNER_NATIVE_ERROR);
  } else if (operation->kind == TRIM_OWNER_NATIVE_READ &&
             read_value != state->native_active[operation->index]) {
    fail(state, TRIM_OWNER_VERIFY_ERROR);
  } else {
    if (++state->native_index == 2 * TRIM_NATIVE_BYTES) {
      state->active_verified = 1;
      state->phase = TRIM_OWNER_CLEANUP;
    }
  }
  return true;
}
