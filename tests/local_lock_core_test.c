#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "local_lock_core.h"

#define RAW_T 1u
#define RAW_M 64u
#define RAW_PAIR (RAW_T | RAW_M)
#define ACTION_CAPTURE 1u
#define ACTION_APPLY 2u
#define ACTION_STOP 4u

static LocalLockState state;
static LocalLockInput input;

static uint8_t poll(uint32_t now, uint8_t raw) {
  input.now = now;
  input.raw = raw;
  return local_lock_poll(&state, &input);
}

static void fresh(uint32_t now, bool locked) {
  local_lock_init(&state, now);
  input = (LocalLockInput){.now = now,
                           .raw = 0,
                           .locked = locked,
                           .safe = true,
                           .clean = true,
                           .capture_ok = true,
                           .motion = false};
  assert(poll(now, 0) == 0);
  assert(poll(now + 39, 0) == 0);
  assert((poll(now + 40, 0) & ACTION_APPLY) == 0);
}

static uint8_t advance(uint32_t start, uint32_t end, uint8_t raw) {
  uint8_t actions = 0;
  while ((uint32_t)(end - start) > 20) {
    start += 20;
    actions |= poll(start, raw);
  }
  actions |= poll(end, raw);
  return actions;
}

static void capture_pair(uint32_t now) {
  assert((poll(now, RAW_PAIR) & ACTION_CAPTURE) != 0);
  assert(local_lock_inputs_blocked(&state));
  assert(local_lock_sleep_blocked(&state));
}

static void test_exact_hold_and_one_shot(void) {
  fresh(0, false);
  capture_pair(60);
  assert(advance(60, 99, RAW_PAIR) == 0);
  assert(advance(99, 100, RAW_PAIR) == 0);
  assert(advance(100, 3099, RAW_PAIR) == 0);
  assert((poll(3100, RAW_PAIR) & ACTION_APPLY) != 0);
  local_lock_feedback(&state, 3100, 0, true);
  input.locked = true;
  assert(advance(3100, 8100, RAW_PAIR) == 0);
  assert(local_lock_motion_blocked(&state));
  assert(poll(8120, 0) == 0);
  assert(poll(8159, 0) == 0);
  assert(local_lock_inputs_blocked(&state));
  assert((poll(8160, 0) & ACTION_APPLY) == 0);
  assert(!local_lock_inputs_blocked(&state));
  assert(!local_lock_motion_blocked(&state));
}

static void test_pair_window_both_orders(void) {
  const uint8_t first_keys[] = {RAW_M, RAW_T};
  for (unsigned int index = 0; index < 2; ++index) {
    for (uint32_t delay = 1499; delay <= 1501; ++delay) {
      fresh(0, false);
      assert(poll(60, first_keys[index]) == 0);
      assert(!local_lock_inputs_blocked(&state));
      assert(advance(60, 60 + delay - 1, first_keys[index]) == 0);
      uint8_t action = poll(60 + delay, RAW_PAIR);
      assert((action & ACTION_CAPTURE) != 0);
      assert((state.phase == LOCAL_LOCK_PAIR) == (delay <= 1500));
      if (delay > 1500) {
        assert((advance(60 + delay, 60 + delay + 3100, RAW_PAIR) & ACTION_APPLY) == 0);
      }
    }
  }
}

static void test_bounce_third_key_and_gap_consume(void) {
  const uint8_t interrupted_raw[] = {0, RAW_M, RAW_T, RAW_PAIR | 2u};
  for (unsigned int index = 0; index < 4; ++index) {
    fresh(0, false);
    capture_pair(60);
    assert((poll(80, interrupted_raw[index]) & ACTION_APPLY) == 0);
    assert((advance(80, 4000, RAW_PAIR) & ACTION_APPLY) == 0);
    assert(local_lock_inputs_blocked(&state));
  }
  fresh(0, false);
  capture_pair(60);
  assert(poll(160, RAW_PAIR) == 0);
  assert((advance(160, 3160, RAW_PAIR) & ACTION_APPLY) != 0);
  fresh(0, false);
  capture_pair(60);
  assert(poll(161, RAW_PAIR) == 0);
  assert((advance(161, 4000, RAW_PAIR) & ACTION_APPLY) == 0);
}

static void test_other_contacts_before_capture_stay_native(void) {
  const uint8_t first_keys[] = {RAW_M, RAW_T};
  const uint8_t other_keys[] = {RAW_M | 2u | 4u, RAW_T | 8u};
  const uint32_t second_times[] = {100, 1600};
  for (unsigned int key = 0; key < 2; ++key) {
    for (unsigned int boundary = 0; boundary < 2; ++boundary) {
      fresh(0, false);
      assert(poll(60, first_keys[key]) == 0);
      assert(advance(60, second_times[boundary] - 20, first_keys[key]) == 0);
      assert(poll(second_times[boundary], other_keys[key]) == 0);
      assert(!local_lock_inputs_blocked(&state));
      assert(!local_lock_motion_blocked(&state));
      assert((advance(second_times[boundary], second_times[boundary] + 400, other_keys[key]) &
              ACTION_APPLY) == 0);
      assert(!local_lock_inputs_blocked(&state));
    }
  }
}

static void test_pre_capture_interruption_stays_native(void) {
  const uint8_t keys[] = {RAW_M, RAW_T};
  for (unsigned int index = 0; index < 2; ++index) {
    fresh(0, false);
    assert(poll(60, keys[index]) == 0);
    assert(poll(80, keys[index]) == 0);
    assert(poll(181, keys[index]) == 0);
    assert(!local_lock_inputs_blocked(&state));
    assert(!local_lock_motion_blocked(&state));
    assert((advance(181, 400, keys[index]) & ACTION_APPLY) == 0);
    assert((poll(420, RAW_PAIR) & ACTION_CAPTURE) != 0);
    assert((advance(420, 4000, RAW_PAIR) & ACTION_APPLY) == 0);

    fresh(0, false);
    assert(poll(60, keys[index]) == 0);
    local_lock_end_attempt(&state, 80);
    assert(!local_lock_inputs_blocked(&state));
    assert((advance(80, 400, keys[index]) & ACTION_APPLY) == 0);
    assert((poll(420, RAW_PAIR) & ACTION_CAPTURE) != 0);
    assert((advance(420, 4000, RAW_PAIR) & ACTION_APPLY) == 0);
  }
  fresh(0, false);
  capture_pair(60);
  local_lock_end_attempt(&state, 80);
  assert(local_lock_inputs_blocked(&state));
  assert((advance(80, 4000, RAW_PAIR) & ACTION_APPLY) == 0);
}

static void test_admission_and_remote_change_consume(void) {
  for (unsigned int failure = 0; failure < 3; ++failure) {
    fresh(0, false);
    capture_pair(60);
    assert(advance(60, 100, RAW_PAIR) == 0);
    if (failure == 0) {
      input.safe = false;
    } else if (failure == 1) {
      input.locked = true;
    } else {
      input.motion = true;
      input.safe = false;
    }
    assert((poll(120, RAW_PAIR) & ACTION_APPLY) == 0);
    input.safe = true;
    input.motion = false;
    input.locked = false;
    assert((advance(120, 4000, RAW_PAIR) & ACTION_APPLY) == 0);
  }
  for (unsigned int gate = 0; gate < 2; ++gate) {
    fresh(0, false);
    if (gate == 0) {
      input.clean = false;
      poll(50, 0);
    } else {
      input.capture_ok = false;
    }
    assert((poll(60, RAW_PAIR) & ACTION_APPLY) == 0);
    input.clean = true;
    input.capture_ok = true;
    assert((advance(60, 4000, RAW_PAIR) & ACTION_APPLY) == 0);
  }
}

static void test_stop_survives_remote_unlock_and_poll_gap(void) {
  const uint8_t raw_keys[] = {1, 2, 4, 8, 16, 32, 64, RAW_PAIR};
  for (unsigned int index = 0; index < sizeof(raw_keys); ++index) {
    fresh(0, true);
    input.motion = true;
    assert((poll(500, raw_keys[index]) & ACTION_STOP) != 0);
    assert(local_lock_motion_blocked(&state));
    input.motion = false;
    input.locked = false;
    assert((advance(500, 4000, raw_keys[index]) & (ACTION_STOP | ACTION_APPLY)) == 0);
    assert(local_lock_motion_blocked(&state));
    assert(poll(4020, 0) == 0);
    assert(poll(4059, 0) == 0);
    assert(local_lock_motion_blocked(&state));
    assert((poll(4060, 0) & ACTION_APPLY) == 0);
    assert(!local_lock_motion_blocked(&state));
  }
}

static void test_wake_reset_and_clock_wrap(void) {
  fresh(UINT32_MAX - 79u, false);
  capture_pair(UINT32_MAX - 19u);
  assert(advance(UINT32_MAX - 19u, UINT32_MAX, RAW_PAIR) == 0);
  assert(poll(20, RAW_PAIR) == 0);
  assert(advance(20, 3019, RAW_PAIR) == 0);
  assert((poll(3020, RAW_PAIR) & ACTION_APPLY) != 0);
  fresh(0, true);
  input.motion = true;
  assert((poll(60, RAW_PAIR) & ACTION_STOP) != 0);
  local_lock_cancel(&state, 60);
  input.motion = false;
  input.locked = false;
  assert(local_lock_motion_blocked(&state));
  assert((advance(60, 4000, RAW_PAIR) & ACTION_APPLY) == 0);
  local_lock_init(&state, 4000);
  assert(!local_lock_motion_blocked(&state));
  assert((advance(4000, 8000, RAW_PAIR) & ACTION_APPLY) == 0);
}

int main(void) {
  assert(sizeof(LocalLockState) == 32);
  test_exact_hold_and_one_shot();
  test_pair_window_both_orders();
  test_bounce_third_key_and_gap_consume();
  test_other_contacts_before_capture_stay_native();
  test_pre_capture_interruption_stays_native();
  test_admission_and_remote_change_consume();
  test_stop_survives_remote_unlock_and_poll_gap();
  test_wake_reset_and_clock_wrap();
  puts("local lock core tests passed");
  return 0;
}
