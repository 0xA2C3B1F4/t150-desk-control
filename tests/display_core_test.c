#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "display_core.h"

static void test_initialization_and_invalid_state(void) {
  DisplayState state = {0};
  assert(!display_valid(&state));
  assert(!display_render_blank(&state, 100000, false));
  assert(display_status(&state, 0, false) == DISPLAY_STATE_UNAVAILABLE);
  assert(!display_configure(&state, DISPLAY_AUTO, 1, 0));
  display_init(&state, 123);
  assert(display_valid(&state));
  assert(state.timeout_seconds == 60 && state.mode == DISPLAY_AUTO);
  assert(state.deadline == 60123);
  DisplayState saved = state;
  assert(!display_configure(&state, 2, 60, 0));
  assert(!display_configure(&state, DISPLAY_AUTO, 0, 0));
  assert(memcmp(&state, &saved, sizeof(state)) == 0);
  state.magic ^= 1;
  assert(!display_render_blank(&state, 100000, false));
  assert(display_status(&state, 100000, true) == DISPLAY_STATE_UNAVAILABLE);
  state = saved;
  state.reserved = 1;
  assert(!display_valid(&state));
}

static void test_deadlines_and_wrap(void) {
  const uint16_t seconds[] = {1, 60, 65535};
  const uint32_t starts[] = {0, 345, UINT32_MAX - 100};
  for (unsigned i = 0; i < sizeof(seconds) / sizeof(seconds[0]); ++i) {
    for (unsigned j = 0; j < sizeof(starts) / sizeof(starts[0]); ++j) {
      DisplayState state;
      display_init(&state, starts[j]);
      assert(display_configure(&state, DISPLAY_AUTO, seconds[i], starts[j]));
      uint32_t deadline = starts[j] + (uint32_t)seconds[i] * 1000u;
      assert(state.deadline == deadline);
      assert(!display_render_blank(&state, deadline - 1, false));
      assert(display_render_blank(&state, deadline, false));
      assert(display_render_blank(&state, deadline + 1, false));
      assert(display_render_blank(&state, deadline + 120000, false));
    }
  }
}

static void test_expiration_stays_latched(void) {
  DisplayState state;
  display_init(&state, 0);
  assert(display_render_blank(&state, 60000, false));
  const uint32_t late[] = {60000u + 0x80000000u, 60000u + 0xffffffffu};
  for (unsigned i = 0; i < sizeof(late) / sizeof(late[0]); ++i) {
    assert(display_render_blank(&state, late[i], false));
    assert(!(display_status(&state, late[i], false) & DISPLAY_STATE_TIME_LEFT));
  }
  display_restart(&state, late[1]);
  assert(!display_render_blank(&state, late[1], false));
  assert(display_status(&state, late[1], false) & DISPLAY_STATE_TIME_LEFT);
  assert(display_render_blank(&state, late[1] + 60000u, false));
  assert(display_configure(&state, DISPLAY_NORMAL, 60, 0));
  assert(!display_render_blank(&state, 60000, false));
  assert(!display_render_blank(&state, 60000u + 0x80000000u, false));
  assert(!(display_status(&state, 60000u + 0x80000000u, false) & DISPLAY_STATE_TIME_LEFT));
}

static void test_settings_and_observation(void) {
  DisplayState state;
  display_init(&state, 0);
  assert(display_render_blank(&state, 60000, false));
  DisplayState saved = state;
  for (unsigned i = 0; i < 100; ++i) {
    assert(display_status(&state, 60000 + i, false) == (DISPLAY_STATE_AUTO | DISPLAY_STATE_BLANK));
    assert(display_configure(&state, DISPLAY_AUTO, 60, 60000 + i));
  }
  assert(memcmp(&state, &saved, sizeof(state)) == 0);
  assert(display_configure(&state, DISPLAY_AUTO, 1, 70000));
  assert(!display_render_blank(&state, 70999, false));
  assert(display_render_blank(&state, 71000, false));
  assert(display_configure(&state, DISPLAY_NORMAL, 1, 80000));
  assert(!display_render_blank(&state, 90000, false));
  assert(display_configure(&state, DISPLAY_AUTO, 1, 100000));
  assert(!display_render_blank(&state, 100999, false));
  assert(display_render_blank(&state, 101000, false));
}

static void test_attention_and_sleep(void) {
  DisplayState state;
  display_init(&state, 0);
  assert(display_render_blank(&state, 60000, false));
  assert(!display_render_blank(&state, 60001, true));
  assert(!display_render_blank(&state, 200000, true));
  assert(display_status(&state, 200000, true) ==
         (DISPLAY_STATE_AUTO | DISPLAY_STATE_ATTENTION | DISPLAY_STATE_TIME_LEFT));
  assert(!display_render_blank(&state, 201234, false));
  assert(state.deadline == 261234);
  assert(!display_render_blank(&state, 261233, false));
  assert(display_render_blank(&state, 261234, false));
  display_sleep(&state);
  assert(state.wake_pending);
  assert(!display_render_blank(&state, 261234, false));
  assert(state.deadline == 321234 && !state.wake_pending);
  assert(display_render_blank(&state, 321234, false));
  display_restart(&state, 400000);
  assert(!display_render_blank(&state, 459999, false));
  assert(display_render_blank(&state, 460000, false));
}

int main(void) {
  test_initialization_and_invalid_state();
  test_deadlines_and_wrap();
  test_expiration_stays_latched();
  test_settings_and_observation();
  test_attention_and_sleep();
  return 0;
}
