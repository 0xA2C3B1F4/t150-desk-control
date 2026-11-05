#include "local_lock_core.h"

enum { CLEAN = 1, INHIBIT = 2, CAPTURED = 4, STOPPED = 8 };

_Static_assert(sizeof(LocalLockState) == 32, "Local lock state must fit its slot");
_Static_assert(_Alignof(LocalLockState) == 4, "Local lock state alignment changed");

void local_lock_init(LocalLockState* state, uint32_t now) {
  *state = (LocalLockState){0};
  state->last_poll = now;
  state->raw_since = now;
  state->magic = LOCAL_LOCK_MAGIC;
}

void local_lock_cancel(LocalLockState* state, uint32_t now) {
  state->phase = LOCAL_LOCK_CONSUMED;
  state->flags = (state->flags & (INHIBIT | STOPPED)) | CAPTURED;
  state->raw_since = now;
}

void local_lock_end_attempt(LocalLockState* state, uint32_t now) {
  if (state->phase == LOCAL_LOCK_FIRST || state->phase == LOCAL_LOCK_SINGLE) {
    /* An uncaptured single key still belongs to the native scanner. Hiding it
     * now would create a synthetic release and could trigger its click action. */
    state->phase = LOCAL_LOCK_SINGLE;
    state->first_key = 0;
  } else {
    local_lock_cancel(state, now);
  }
}

bool local_lock_inputs_blocked(const LocalLockState* state) {
  return state->magic != LOCAL_LOCK_MAGIC || state->phase == LOCAL_LOCK_RELEASE ||
         (state->flags & (CAPTURED | INHIBIT));
}

bool local_lock_motion_blocked(const LocalLockState* state) {
  return state->magic != LOCAL_LOCK_MAGIC || (state->flags & INHIBIT);
}

void local_lock_inhibit_motion(LocalLockState* state) {
  state->flags |= INHIBIT;
}

bool local_lock_sleep_blocked(const LocalLockState* state) {
  return state->magic == LOCAL_LOCK_MAGIC &&
         (state->phase == LOCAL_LOCK_FIRST || local_lock_inputs_blocked(state));
}

void local_lock_feedback(LocalLockState* state, uint32_t now, uint8_t result, bool locked) {
  state->result = result;
  state->feedback = result == 0 ? (locked ? 1 : 2) : (result == 3 ? 3 : 4);
  state->feedback_until = now + LOCAL_LOCK_FEEDBACK_MS;
}

uint8_t local_lock_poll(LocalLockState* state, const LocalLockInput* input) {
  if (state->magic != LOCAL_LOCK_MAGIC) {
    local_lock_init(state, input->now);
  }
  uint8_t action = 0;
  const uint8_t raw = input->raw & LOCAL_LOCK_ALL_KEYS;
  const bool gap = (uint32_t)(input->now - state->last_poll) > LOCAL_LOCK_MAX_GAP_MS;
  const bool previous_clean = (state->flags & CLEAN) != 0 && !gap;
  state->last_poll = input->now;
  if (raw != state->previous_raw || gap) {
    state->raw_since = input->now;
  }
  state->flags &= (uint8_t)~CLEAN;

  /* Stop is evaluated even after a missed poll. Its release latch is separate
   * from gesture consumption so a stationary locked pair can still unlock. */
  if ((input->locked || (state->flags & INHIBIT)) && raw) {
    state->flags |= INHIBIT;
    if (input->motion && !(state->flags & STOPPED)) {
      state->flags |= STOPPED;
      local_lock_cancel(state, input->now);
      action |= LOCAL_LOCK_STOP | LOCAL_LOCK_RESTART;
    }
  }
  if (gap && (state->phase == LOCAL_LOCK_FIRST || state->phase == LOCAL_LOCK_PAIR ||
              state->phase == LOCAL_LOCK_HOLD || local_lock_inputs_blocked(state))) {
    local_lock_end_attempt(state, input->now);
  }
  if (!raw && (uint32_t)(input->now - state->raw_since) >= LOCAL_LOCK_STABLE_MS) {
    const bool blocked = local_lock_inputs_blocked(state);
    state->phase = LOCAL_LOCK_IDLE;
    state->flags = input->clean ? CLEAN : 0;
    state->first_key = 0;
    if (blocked) {
      action |= LOCAL_LOCK_CAPTURE;
    }
  }

  switch (state->phase) {
    case LOCAL_LOCK_IDLE:
      if (!raw) {
        if (input->clean) {
          state->flags |= CLEAN;
        }
      } else if (previous_clean && input->safe &&
                 (raw == LOCAL_LOCK_M || raw == LOCAL_LOCK_T || raw == LOCAL_LOCK_PAIR_KEYS)) {
        state->first_at = input->now;
        state->first_key = raw;
        state->baseline_lock = input->locked;
        state->phase = LOCAL_LOCK_FIRST;
        if (raw != LOCAL_LOCK_PAIR_KEYS) {
          break;
        }
        /* A simultaneous pair uses the same capture rules as an early second
         * contact, including the scanner/GPIO interleaving case. */
        if (input->capture_ok) {
          state->phase = LOCAL_LOCK_PAIR;
          state->raw_since = input->now;
          state->flags |= CAPTURED;
          action |= LOCAL_LOCK_CAPTURE;
        } else {
          local_lock_cancel(state, input->now);
          action |= LOCAL_LOCK_CAPTURE;
        }
      }
      break;
    case LOCAL_LOCK_FIRST:
    case LOCAL_LOCK_SINGLE:
      if (state->phase == LOCAL_LOCK_SINGLE && !state->first_key) {
        if (raw == LOCAL_LOCK_PAIR_KEYS) {
          local_lock_cancel(state, input->now);
          action |= LOCAL_LOCK_CAPTURE;
        }
      } else if (!input->safe || input->locked != state->baseline_lock) {
        local_lock_cancel(state, input->now);
        action |= LOCAL_LOCK_CAPTURE;
      } else if (raw && raw != state->first_key && raw != LOCAL_LOCK_PAIR_KEYS) {
        /* Before capture, another contact belongs to the native dispatcher,
         * including its existing M + Up + Down chord. Only end this window. */
        local_lock_end_attempt(state, input->now);
      } else if (!raw || !(raw & state->first_key)) {
        /* Ordinary single-key releases keep native click timing. They cannot
         * open a fresh window until the full release and clean scan boundary. */
        state->phase = LOCAL_LOCK_SINGLE;
        state->first_key = 0;
      } else if (raw == LOCAL_LOCK_PAIR_KEYS) {
        if (state->phase == LOCAL_LOCK_FIRST && input->capture_ok &&
            (uint32_t)(input->now - state->first_at) <= LOCAL_LOCK_WINDOW_MS) {
          state->phase = LOCAL_LOCK_PAIR;
          state->raw_since = input->now;
          state->flags |= CAPTURED;
        } else {
          local_lock_cancel(state, input->now);
        }
        action |= LOCAL_LOCK_CAPTURE;
      } else if ((uint32_t)(input->now - state->first_at) > LOCAL_LOCK_WINDOW_MS) {
        state->phase = LOCAL_LOCK_SINGLE;
      }
      break;
    case LOCAL_LOCK_PAIR:
    case LOCAL_LOCK_HOLD:
      if (raw != LOCAL_LOCK_PAIR_KEYS || !input->safe || !input->clean ||
          input->locked != state->baseline_lock || (state->flags & STOPPED)) {
        local_lock_cancel(state, input->now);
      } else if (state->phase == LOCAL_LOCK_PAIR &&
                 (uint32_t)(input->now - state->raw_since) >= LOCAL_LOCK_STABLE_MS) {
        state->stable_at = input->now;
        state->phase = LOCAL_LOCK_HOLD;
      } else if (state->phase == LOCAL_LOCK_HOLD &&
                 (uint32_t)(input->now - state->stable_at) >= LOCAL_LOCK_HOLD_MS) {
        local_lock_cancel(state, input->now);
        action |= LOCAL_LOCK_APPLY;
      }
      break;
    default:
      break;
  }
  state->previous_raw = raw;
  return action;
}
