#ifndef DESK_LOCAL_LOCK_CORE_H
#define DESK_LOCAL_LOCK_CORE_H

#include <stdbool.h>
#include <stdint.h>

enum {
  LOCAL_LOCK_MAGIC = 0x4c4b3031u,
  LOCAL_LOCK_T = 1,
  LOCAL_LOCK_M = 64,
  LOCAL_LOCK_PAIR_KEYS = LOCAL_LOCK_T | LOCAL_LOCK_M,
  LOCAL_LOCK_ALL_KEYS = 127,
  LOCAL_LOCK_WINDOW_MS = 1500,
  LOCAL_LOCK_STABLE_MS = 40,
  LOCAL_LOCK_HOLD_MS = 3000,
  LOCAL_LOCK_MAX_GAP_MS = 100,
  LOCAL_LOCK_FEEDBACK_MS = 1000
};

enum {
  LOCAL_LOCK_RELEASE,
  LOCAL_LOCK_IDLE,
  LOCAL_LOCK_FIRST,
  LOCAL_LOCK_SINGLE,
  LOCAL_LOCK_PAIR,
  LOCAL_LOCK_HOLD,
  LOCAL_LOCK_CONSUMED
};

enum { LOCAL_LOCK_CAPTURE = 1, LOCAL_LOCK_APPLY = 2, LOCAL_LOCK_STOP = 4, LOCAL_LOCK_RESTART = 8 };

typedef struct {
  uint32_t magic;
  uint32_t first_at;
  uint32_t stable_at;
  uint32_t raw_since;
  uint32_t last_poll;
  uint32_t feedback_until;
  uint8_t phase;
  uint8_t previous_raw;
  uint8_t first_key;
  uint8_t flags;
  uint8_t baseline_lock;
  uint8_t result;
  uint8_t feedback;
  uint8_t reserved;
} LocalLockState;

typedef struct {
  uint32_t now;
  uint8_t raw;
  bool locked;
  bool safe;
  bool clean;
  bool capture_ok;
  bool motion;
} LocalLockInput;

void local_lock_init(LocalLockState* state, uint32_t now);
uint8_t local_lock_poll(LocalLockState* state, const LocalLockInput* input);
void local_lock_cancel(LocalLockState* state, uint32_t now);
void local_lock_end_attempt(LocalLockState* state, uint32_t now);
void local_lock_feedback(LocalLockState* state, uint32_t now, uint8_t result, bool locked);
bool local_lock_inputs_blocked(const LocalLockState* state);
bool local_lock_motion_blocked(const LocalLockState* state);
void local_lock_inhibit_motion(LocalLockState* state);
bool local_lock_sleep_blocked(const LocalLockState* state);

#endif
