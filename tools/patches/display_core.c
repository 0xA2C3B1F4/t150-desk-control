#include "display_core.h"

#define DISPLAY_MAGIC 0x44533031u

_Static_assert(sizeof(DisplayState) == 16, "Display state must fit its platform slot");

static bool expired(const DisplayState* state, uint32_t now) {
  return (int32_t)(now - state->deadline) >= 0;
}

void display_init(DisplayState* state, uint32_t now) {
  state->magic = 0;
  state->deadline = now + 60000u;
  state->timeout_seconds = 60;
  state->mode = DISPLAY_AUTO;
  state->attention = 0;
  state->blank = 0;
  state->wake_pending = 0;
  state->expired = 0;
  state->reserved = 0;
  /* No caller may treat zeroed or partly initialized storage as ready. */
  __asm volatile("" ::: "memory");
  state->magic = DISPLAY_MAGIC;
}

bool display_valid(const DisplayState* state) {
  return state->magic == DISPLAY_MAGIC && state->timeout_seconds != 0 &&
         state->mode <= DISPLAY_NORMAL && state->attention <= 1 && state->blank <= 1 &&
         state->wake_pending <= 1 && state->expired <= 1 && state->reserved == 0;
}

void display_restart(DisplayState* state, uint32_t now) {
  if (display_valid(state)) {
    state->deadline = now + (uint32_t)state->timeout_seconds * 1000u;
    state->expired = 0;
  }
}

bool display_configure(DisplayState* state, uint8_t mode, uint16_t seconds, uint32_t now) {
  if (!display_valid(state) || mode > DISPLAY_NORMAL || seconds == 0) {
    return false;
  }
  if (state->mode != mode || state->timeout_seconds != seconds) {
    state->mode = mode;
    state->timeout_seconds = seconds;
    display_restart(state, now);
  }
  return true;
}

void display_sleep(DisplayState* state) {
  if (display_valid(state)) {
    state->wake_pending = 1;
  }
}

bool display_render_blank(DisplayState* state, uint32_t now, bool attention) {
  if (!display_valid(state)) {
    return false;
  }
  /* Native sleep stops SysTick. The first resumed render starts a full period,
   * as does leaving attention, even if no foreground iteration ran at exit. */
  if (attention || state->attention || state->wake_pending) {
    display_restart(state, now);
  }
  state->attention = attention;
  state->wake_pending = 0;
  /* Keep an elapsed timeout elapsed across later signed-half-range crossings. */
  state->expired = state->expired || expired(state, now);
  state->blank = state->mode == DISPLAY_AUTO && !attention && state->expired;
  return state->blank;
}

uint8_t display_status(const DisplayState* state, uint32_t now, bool attention) {
  if (!display_valid(state)) {
    return DISPLAY_STATE_UNAVAILABLE;
  }
  return (state->mode == DISPLAY_AUTO ? DISPLAY_STATE_AUTO : 0) |
         (attention ? DISPLAY_STATE_ATTENTION : 0) |
         (!state->expired && !expired(state, now) ? DISPLAY_STATE_TIME_LEFT : 0) |
         (state->blank ? DISPLAY_STATE_BLANK : 0);
}
