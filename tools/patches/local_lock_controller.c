#include "local_lock_controller.h"
#include "display_controller.h"
#include "local_lock_core.h"
#include "motion_trim_controller.h"
#include "platform_runtime.h"

#define BYTE(offset) (*(volatile uint8_t*)(0x20000000u + (offset)))
#define HALF(offset) (*(volatile uint16_t*)(0x20000000u + (offset)))
#define WORD(offset) (*(volatile uint32_t*)(0x20000000u + (offset)))
#define REGISTER(address) (*(volatile uint32_t*)(address))
#define STATE ((LocalLockState*)PLATFORM_LOCAL_LOCK_ADDRESS)
#define NATIVE(address) ((void (*)(void))(address))

_Static_assert(sizeof(LocalLockState) == PLATFORM_LOCAL_LOCK_SIZE, "Local lock allocation changed");
_Static_assert(PLATFORM_LOCAL_LOCK_ADDRESS == 0x20001298u, "Local lock address changed");
_Static_assert(PLATFORM_LOCAL_LOCK_ADDRESS % _Alignof(LocalLockState) == 0,
               "Local lock allocation is unaligned");

extern uint32_t local_lock_original_apply(uint32_t desired);
extern void trim_original_reset_scanners(void);
extern uint32_t local_lock_original_gate_refresh(void);

static uint32_t mask(void) {
  uint32_t previous;
  __asm volatile("mrs %0, PRIMASK\ncpsid i" : "=r"(previous)::"memory");
  return previous;
}

static void restore(uint32_t previous) {
  __asm volatile("msr PRIMASK, %0" ::"r"(previous) : "memory");
}

static bool ready(void) {
  return platform_ready() && STATE->magic == LOCAL_LOCK_MAGIC;
}

static uint8_t raw_keys(void) {
  uint32_t a = ~REGISTER(0x48000010u), b = ~REGISTER(0x48000410u);
  return (uint8_t)(((a >> 8) & 1u) | ((a >> 11) & 2u) | ((a >> 9) & 4u) | ((a >> 7) & 8u) |
                   ((a >> 5) & 16u) | ((b >> 9) & 32u) | ((b >> 9) & 64u));
}

static bool events(unsigned object) {
  return HALF(object + 12) || BYTE(object + 14);
}

static bool scanners_idle(void) {
  for (unsigned object = 0xa10; object < 0xa90; object += 16) {
    if (BYTE(object + 4) || events(object)) {
      return false;
    }
  }
  return true;
}

static bool deadlines_due(uint32_t now) {
  return (int32_t)(now - WORD(0xaac)) >= 0 && (int32_t)(now - WORD(0xabc)) >= 0;
}

static bool accepted_motion(void) {
  return BYTE(0x148) || BYTE(0x7ae) || BYTE(0x7af) || BYTE(0x7b1) || BYTE(0x7b2) || BYTE(0x7b3);
}

static bool safe(void) {
  static const uint16_t busy_offsets[] = {0x88d, 0x895, 0x88c, 0x754, 0x7ad, 0x7ae, 0x7af, 0x7b0,
                                          0x7b1, 0x7b2, 0x7b3, 0x7b4, 0x184, 0x185, 0x186, 0x187,
                                          0x188, 0x189, 0x18a, 0x18d, 0x88f, 0x890, 0x148, 0x8b8,
                                          0x892, 0x893, 0x894, 0x8b9, 0xaa0, 0xaa1, 0x18e, 0x8ba};
  for (unsigned i = 0; i < sizeof(busy_offsets) / sizeof(busy_offsets[0]); ++i) {
    if (BYTE(busy_offsets[i])) {
      return false;
    }
  }
  return !HALF(0x75a) && (BYTE(0xfe0) == 0 || BYTE(0xfe0) == 4) && !REGISTER(0x4000043cu) &&
         !REGISTER(0x40000440u) && !trim_controller_blocks_native();
}

bool local_lock_admitted(void) {
  /* This predicate never arms the trim release gate. Its caller holds PRIMASK
   * across the final check and the synchronous legacy EEPROM transaction. */
  return ready() && safe() && scanners_idle();
}

static bool capture_allowed(uint32_t now) {
  if (!deadlines_due(now)) {
    return false;
  }
  unsigned active = 0;
  for (unsigned object = 0xa10; object < 0xa90; object += 16) {
    if (events(object)) {
      return false;
    }
    uint8_t phase = BYTE(object + 4);
    if (phase) {
      if (phase > 3 || (object != 0xa60 && object != 0xa70)) {
        return false;
      }
      if (STATE->first_key != LOCAL_LOCK_PAIR_KEYS &&
          object != (STATE->first_key == LOCAL_LOCK_M ? 0xa60u : 0xa70u)) {
        return false;
      }
      ++active;
    }
  }
  return active <= 1;
}

uint32_t local_lock_request(uint32_t desired) {
  uint32_t previous = mask();
  uint32_t result = 2;
  /* Observe a physical stop press before an absolute remote unlock changes
   * CL01. The gate cannot disappear between that write and foreground poll. */
  (void)local_lock_controller_motion_blocked();
  if (desired <= 1 && local_lock_admitted()) {
    result = local_lock_original_apply(desired);
    /* DMA reception continues with IRQs masked. Recover a pending overrun
     * before restoring the caller mask; USART ICR ORECF is write-one-clear. */
    if (REGISTER(0x4001381cu) & 8u) {
      REGISTER(0x40013820u) = 8u;
    }
  }
  restore(previous);
  return result;
}

void local_lock_native_stop(void) {
  /* Retain native 17/01 braking at 7D7E..7D94 and consume already accepted
   * UART movement overrides. Otherwise a queued press can restart the native
   * dispatcher after this Stop or after the physical release gate opens. */
  BYTE(0x148) = 0;
  BYTE(0x7ae) = 0;
  BYTE(0x7af) = 0;
  BYTE(0x7b1) = 0;
  BYTE(0x7b2) = 0;
  BYTE(0x7b3) = 0;
  trim_original_reset_scanners();
  if (BYTE(0x88d) == 1 && !BYTE(0x88f)) {
    NATIVE(0x080034e1u)();
  }
  display_controller_restart();
}

void local_lock_controller_init(void) {
  if (platform_ready()) {
    local_lock_init(STATE, WORD(0x75c));
  }
}

void local_lock_controller_before_sleep(void) {
  uint32_t previous = mask();
  if (ready()) {
    local_lock_cancel(STATE, WORD(0x75c));
  }
  restore(previous);
}

void local_lock_controller_after_wake(void) {
  uint32_t previous = mask();
  if (ready()) {
    /* Retain the independent motion latch over Stop/WFI. A wake edge is not
     * a fresh gesture and only a stable release may clear that latch. */
    local_lock_cancel(STATE, WORD(0x75c));
    STATE->last_poll = WORD(0x75c);
    STATE->previous_raw = raw_keys();
  }
  restore(previous);
}

bool local_lock_controller_inputs_blocked(void) {
  return !ready() || local_lock_inputs_blocked(STATE);
}

bool local_lock_controller_motion_blocked(void) {
  uint32_t previous = mask();
  bool blocked = true;
  if (ready()) {
    if (BYTE(0xfc8) && raw_keys()) {
      local_lock_inhibit_motion(STATE);
    }
    blocked = local_lock_motion_blocked(STATE);
  }
  restore(previous);
  return blocked;
}

bool local_lock_controller_sleep_blocked(void) {
  return ready() && local_lock_sleep_blocked(STATE);
}

void local_lock_controller_poll(void) {
  uint32_t previous = mask();
  if (!ready()) {
    restore(previous);
    return;
  }
  const uint32_t now = WORD(0x75c);
  const uint8_t raw = raw_keys();
  bool admitted = safe();
  /* Simultaneous capture can contain one newly scanned M or T because the
   * contact may change between the foreground detector and native GPIO read. */
  if (STATE->phase == LOCAL_LOCK_IDLE && raw == LOCAL_LOCK_PAIR_KEYS) {
    STATE->first_key = LOCAL_LOCK_PAIR_KEYS;
  }
  LocalLockInput input = {now,
                          raw,
                          BYTE(0xfc8) != 0,
                          admitted,
                          admitted && scanners_idle() && deadlines_due(now),
                          admitted && capture_allowed(now),
                          BYTE(0x88d) == 1 || BYTE(0x895) || accepted_motion() || HALF(0x75a) ||
                              REGISTER(0x4000043cu) || REGISTER(0x40000440u) || BYTE(0xfe0) == 1 ||
                              BYTE(0xfe0) == 2 || BYTE(0xfe0) == 3};
  const uint8_t action = local_lock_poll(STATE, &input);
  if (action & LOCAL_LOCK_CAPTURE) {
    trim_original_reset_scanners();
  }
  if (action & LOCAL_LOCK_STOP) {
    /* RS01 retains its existing SysTick raw-key watchdog and pause_reset(3).
     * Do not run an additional ordinary brake during its reset ownership. */
    if (BYTE(0xfe0) == 0 || BYTE(0xfe0) == 4) {
      local_lock_native_stop();
    }
  }
  if (action & LOCAL_LOCK_APPLY) {
    /* GPIO can change independently of PRIMASK. Recheck the exact pair at the
     * commit boundary as well as the shared storage/motion admission. */
    const uint32_t result =
        raw_keys() == LOCAL_LOCK_PAIR_KEYS && BYTE(0xfc8) == STATE->baseline_lock
            ? local_lock_request(!BYTE(0xfc8))
            : 2;
    local_lock_feedback(STATE, now, (uint8_t)result, BYTE(0xfc8) != 0);
    /* Polled EEPROM waits are absent from SysTick time. Reset both continuity
     * timestamps so masked wall time cannot be credited to another gesture. */
    STATE->last_poll = WORD(0x75c);
    STATE->raw_since = WORD(0x75c);
    display_controller_restart();
  } else if (action & LOCAL_LOCK_RESTART) {
    display_controller_restart();
  }
  restore(previous);
}

int local_lock_controller_route(void) {
  const volatile uint8_t* request = (const volatile uint8_t*)WORD(0xe8);
  const uint8_t length = BYTE(0x124);
  if (request[0] == 0xd3) {
    if (length != 6) {
      return 1;
    }
    uint32_t previous = mask();
    if (!trim_controller_reply_room(35)) {
      restore(previous);
      return 1;
    }
    const uint8_t operation = request[2];
    uint8_t result = 1;
    if (request[1] == 1 && operation <= 2) {
      if (!operation) {
        result = 0;
      } else {
        if (ready() && STATE->phase != LOCAL_LOCK_IDLE && STATE->phase != LOCAL_LOCK_RELEASE) {
          local_lock_end_attempt(STATE, WORD(0x75c));
        }
        result = (uint8_t)local_lock_request(2u - operation);
      }
    }
    uint32_t hidden = local_lock_original_gate_refresh();
    uint16_t sequence = HALF(0x120);
    uint8_t reply[15] = {0xd4,
                         1,
                         operation,
                         result,
                         BYTE(0xfc8),
                         hidden || local_lock_controller_inputs_blocked(),
                         BYTE(0xfca),
                         0x43,
                         0x4c,
                         0x30,
                         0x31,
                         request[3],
                         request[4],
                         (uint8_t)(sequence >> 8),
                         (uint8_t)sequence};
    trim_controller_reply(reply, sizeof(reply));
    restore(previous);
    return 1;
  }
  if (request[0] == 0xd9 && length == 18 && request[2] == 1 && ready() &&
      STATE->phase != LOCAL_LOCK_IDLE && STATE->phase != LOCAL_LOCK_RELEASE) {
    uint32_t previous = mask();
    local_lock_end_attempt(STATE, WORD(0x75c));
    restore(previous);
  }
  if (!local_lock_controller_motion_blocked()) {
    return 0;
  }
  /* Rejected starts produce no success ACK and leave all native/patch state
   * untouched. Reads, Stop and releases keep their existing route semantics. */
  if (request[0] == 0x17) {
    return !((length == 5 && request[1] == 1 && request[3] == 0) ||
             (length == 4 && request[1] >= 3 && request[1] <= 9 && request[2] == 0));
  }
  return (request[0] == 0xd1 && length == 8 && request[2] != 0) ||
         (request[0] == 0xd5 && length == 11 && request[2] >= 1 && request[2] <= 3);
}

bool local_lock_controller_render(void) {
  uint32_t previous = mask();
  if (!ready() || !STATE->feedback) {
    restore(previous);
    return false;
  }
  if ((int32_t)(WORD(0x75c) - STATE->feedback_until) >= 0) {
    STATE->feedback = 0;
    restore(previous);
    return false;
  }
  /* Feedback never replaces native motion, protection, maintenance or alarm
   * indications. CL01 fault/release and the captured raw pair are deliberately
   * absent, since those are the conditions this feedback describes. */
  static const uint16_t priorities[] = {0x88d, 0x895, 0x754, 0x148, 0x88f, 0x890,
                                        0x892, 0x893, 0x894, 0x8b8, 0x8b9, 0x8ba,
                                        0xaa0, 0xaa1, 0x18e, 0x18c, 0x8a8};
  for (unsigned i = 0; i < sizeof(priorities) / sizeof(priorities[0]); ++i) {
    if (BYTE(priorities[i])) {
      restore(previous);
      return false;
    }
  }
  if (HALF(0x75a) || REGISTER(0x4000043cu) || REGISTER(0x40000440u) ||
      (REGISTER(0x4000280cu) & 0x100u) || (REGISTER(0x40010414u) & (1u << 17))) {
    restore(previous);
    return false;
  }
  /* The native 86F9 font establishes a,b,c,d,e,f,g in bits0..6. Reuse the
   * native segment shifter and the same three active-low digit selects. U is
   * the standard b,c,d,e,f shape; the other glyphs occur in the native font. */
  static const uint8_t glyphs[4][3] = {
      {0x38, 0x5c, 0x58}, {0x3e, 0x54, 0x38}, {0x79, 0x50, 0x50}, {0x7c, 0x6d, 0x6e}};
  uint8_t digit = BYTE(0x180);
  if (digit > 2) {
    digit = 0;
  }
  uint8_t feedback = STATE->feedback <= 4 ? STATE->feedback - 1 : 2;
  NATIVE(0x080036adu)();
  ((void (*)(uint32_t))0x0800458du)(glyphs[feedback][digit]);
  REGISTER(0x48000428u) = digit == 0 ? 0x20u : (digit == 1 ? 0x800u : 0x400u);
  BYTE(0x180) = digit == 2 ? 0 : digit + 1;
  restore(previous);
  return true;
}
