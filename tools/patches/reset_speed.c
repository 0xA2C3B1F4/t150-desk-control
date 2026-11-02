/* RS01. No heap, persistent writes, parameter changes, or libc. All addresses
 * are bound to the exact DT02+CL01R3 image by build_reset_speed_patch.py. */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef int i32;
#define B(o) (*(volatile u8*)(0x20000000u + (o)))
#define H(o) (*(volatile u16*)(0x20000000u + (o)))
#define W(o) (*(volatile u32*)(0x20000000u + (o)))
#define FN(a, t) ((t)(a))
#define NOW W(0x75c)
#define LEASE 1500u
enum { IDLE, DESCENT, REBOUND, PAUSED, COMPLETE, FAULT };
enum { OK, BAD, STALE, INELIGIBLE, BUSY, LOCKED, GUARD };
typedef struct {
  u32 token, deadline, challenge, serial;
  u8 state, reason, speed, phase;
  u32 magic;
  u32 hold_cleanup_pending;
} Rs;
#define S (*(volatile Rs*)0x20000fd0u)
#define MAGIC 0x52533031u
static u32 mask(void) {
  u32 v;
  __asm volatile("mrs %0, PRIMASK\ncpsid i" : "=r"(v)::"memory");
  return v;
}
static void unmask(u32 v) {
  __asm volatile("msr PRIMASK, %0" ::"r"(v) : "memory");
}
static int active(void) {
  return S.state == DESCENT || S.state == REBOUND;
}
static int due(u32 now, u32 deadline) {
  return (i32)(now - deadline) >= 0;
}
static int raw_key(void) {
  return ((*(volatile u32*)0x48000010u & 0x1f00u) != 0x1f00u ||
          (*(volatile u32*)0x48000410u & 0xc000u) != 0xc000u);
}
static void cut(void) {
  FN(0x0800606du, void (*)(void))();
  H(0x75a) = 0;
  FN(0x0800605du, void (*)(u32))(0x200009c8u);
  FN(0x0800605du, void (*)(u32))(0x200009ecu);
}
extern void child_lock_reset_scanners(void);
extern int child_lock_gate_refresh(void);
static int off(void) {
  return !H(0x75a) && !*(volatile u32*)0x4000043cu && !*(volatile u32*)0x40000440u;
}
static void release_owned_hold(void) {
  /* The stock Down handler at 08004BFA sets AA0 for our held override,
   * including mode-2 calibration. Its release path clears AA0 only for
   * ordinary mode-1 descent (88C=2), so a frozen reset cannot clear it.
   * Release that owned action marker only in the preserved homing phases.
   * Other states/values and every admission guard remain untouched. */
  if (B(0xaa0) == 1 &&
      ((S.phase == DESCENT && B(0x88d) == 2 && B(0x88c) == 3) ||
       ((S.phase == DESCENT || S.phase == REBOUND) && B(0x88d) == 1 && B(0x88f) == 1)))
    B(0xaa0) = 0;
}
static void finish_hold_cleanup(void) {
  /* Main-loop boundary after any interrupted stock scanner/motion call.
   * Consume once: do not erase a new independent guard on later polls. */
  if (S.hold_cleanup_pending) {
    release_owned_hold();
    S.hold_cleanup_pending = 0;
  }
}
static void pause_reset(u8 reason) {
  /* Called with interrupts masked, or by SysTick. Disable ownership before
   * clearing output so nested observations can never renew a paused reset. */
  S.phase = S.state;
  S.state = PAUSED;
  S.reason = reason;
  S.token = 0;
  S.deadline = 0;
  S.hold_cleanup_pending = 1;
  release_owned_hold();
  B(0x7ad) = 0;
  B(0x7af) = 0;
  B(0x187) = 0;
  B(0xfc9) = 1;
  child_lock_reset_scanners();
  cut();
}
void rs_init(void) {
  S.magic = 0;
  volatile u32* p = (volatile u32*)0x20000fd0u;
  for (u32 i = 0; i < sizeof(Rs) / sizeof(u32); i++)
    p[i] = 0;
  S.magic = MAGIC;
}
void rs_watchdog(void) {
  if (S.magic != MAGIC || !active())
    return;
  if (due(NOW, S.deadline))
    pause_reset(2);
  else if (raw_key())
    pause_reset(3);
}
void rs_output(u32 direction, u32 pwm) {
  /* Atomic at the final actuator boundary. If SysTick interrupted an earlier
   * stock calculation, that stale calculation cannot restore motor output. */
  u32 saved = mask();
  rs_watchdog();
  if (S.state == PAUSED || S.state == FAULT) {
    H(0x75a) = 0;
    *(volatile u32*)0x4000043cu = 0;
    *(volatile u32*)0x40000440u = 0;
  } else if (direction < 2) {
    *(volatile u32*)0x4000043cu = direction == 1 ? pwm : 0;
    *(volatile u32*)0x40000440u = direction == 0 ? pwm : 0;
  }
  unmask(saved);
}
static int guards(void) {
  static const u16 offsets[] = {0x88f, 0x890, 0x148, 0x8b8, 0x892, 0x894,
                                0xaa0, 0xaa1, 0x18e, 0x8ba, 0x754};
  for (u32 i = 0; i < sizeof(offsets) / 2; i++)
    if (B(offsets[i]))
      return 1;
  return 0;
}
static int keys_busy(void) {
  if (raw_key())
    return 1;
  for (u32 i = 0x7ad; i <= 0x7b4; i++)
    if (B(i))
      return 1;
  static const u16 offsets[] = {0x184, 0x185, 0x186, 0x187, 0x188, 0x189, 0x18a, 0x18d};
  for (u32 i = 0; i < sizeof(offsets) / 2; i++)
    if (B(offsets[i]))
      return 1;
  for (u32 i = 0xa10; i < 0xa90; i += 16)
    if (B(i + 4) || H(i + 12) || B(i + 14))
      return 1;
  return 0;
}
static int admission(int resume) {
  child_lock_gate_refresh();
  if (B(0xfc8) || B(0xfc9) || B(0xfca))
    return LOCKED;
  if (!off() || keys_busy())
    return BUSY;
  if (resume) {
    if (S.state != PAUSED || (S.phase != DESCENT && S.phase != REBOUND))
      return INELIGIBLE;
    /* Rebound owns 88F. All other guards remain live on explicit resume. */
    u8 rebound = B(0x88f);
    B(0x88f) = 0;
    int blocked = guards();
    B(0x88f) = rebound;
    if (blocked)
      return GUARD;
    if ((S.phase == DESCENT && (B(0x88d) != 2 || B(0x88c) != 3)) ||
        (S.phase == REBOUND && (B(0x88d) != 1 || B(0x88f) != 1)))
      return INELIGIBLE;
    return OK;
  }
  if (active() || S.state == PAUSED || B(0x88d) == 1 || B(0x88d) == 2)
    return BUSY;
  u8 mode = B(0x88d);
  if (guards() || (mode != 0 && mode != 8 && mode != 10 && mode != 11 && mode != 12))
    return GUARD;
  /* Explicit remote calibration is allowed at any height in an otherwise
   * admitted idle/recoverable mode. Do not require the stock long-Down
   * startup/fault flags or a near-bottom position estimate: calibration
   * establishes that reference and may be needed after a fault at height. */
  return OK;
}
static void begin(void) {
  /* The stock long-Down homing-entry writes. No EEPROM or geometry edits. */
  B(0x88d) = 2;
  B(0x891) = 0;
  B(0x88c) = 3;
  B(0x893) = 0;
  W(0x7d4) = 0;
  H(0x75a) = 0;
  B(0x88f) = 0;
  B(0x899) = 7;
  FN(0x0800605du, void (*)(u32))(0x200009c8u);
  FN(0x0800605du, void (*)(u32))(0x200009ecu);
  B(0x8b9) = 0;
  S.state = DESCENT;
  S.phase = DESCENT;
  S.reason = 0;
  S.hold_cleanup_pending = 0;
  B(0x7af) = 1;
  B(0x187) = 1;
}
static void token(u16 nonce) {
  S.serial += 0x9e3779b9u;
  u32 next = S.serial + NOW + ((u32)nonce << 16) + nonce;
  S.token = next ? next : 1;
  S.challenge = NOW + LEASE;
}
int rs_before_main(void) {
  u32 saved = mask();
  rs_watchdog();
  int paused = S.state == PAUSED || S.state == FAULT;
  if (paused) {
    finish_hold_cleanup();
    B(0x7ad) = 0;
    B(0x7af) = 0;
    B(0x187) = 0;
    child_lock_reset_scanners();
    cut();
  }
  unmask(saved);
  return paused;
}
void rs_after_main(void) {
  u32 saved = mask();
  if (S.state == COMPLETE && (B(0x88d) != 0 || B(0x895) != 0)) {
    /* Completion describes this calibration, not every later controller
     * state. A subsequent motion or fault needs an ordinary idle snapshot. */
    S.state = IDLE;
    S.phase = 0;
    S.reason = 0;
    S.token = 0;
    S.deadline = 0;
  } else if (S.state == PAUSED) {
    /* A SysTick cut can interrupt the stock bottom-contact transition.
     * Reconcile its completed phase change without granting a new lease. */
    if (S.phase == DESCENT && B(0x88d) == 1 && B(0x88f) == 1)
      S.phase = REBOUND;
    finish_hold_cleanup();
  } else if (active()) {
    if (B(0x88d) == 1 && B(0x88f) == 1) {
      S.state = REBOUND;
      S.phase = REBOUND;
    } else if (B(0x88d) != 2 || B(0x88c) != 3) {
      pause_reset(4);
      S.state = FAULT;
      S.phase = 0;
    }
  }
  unmask(saved);
}
void rs_completed(void) {
  /* Called only after the exact stock homing-success save returned. An ISR
   * pause during that save still represents a completed calibration. */
  u32 saved = mask();
  if (S.magic == MAGIC && (S.state == REBOUND || (S.state == PAUSED && S.phase == REBOUND))) {
    cut();
    S.state = COMPLETE;
    S.phase = COMPLETE;
    S.reason = 0;
    S.token = 0;
    S.deadline = 0;
    S.hold_cleanup_pending = 0;
  }
  unmask(saved);
}
void rs_cap(void) {
  /* Stock position-controller has already applied RAM158. This only lowers
   * a positive ordinary-motion setpoint and never changes the stock limit. */
  if (S.magic == MAGIC && S.speed == 1 && B(0x88d) == 1 && !B(0x88f) && !B(0x890) && !B(0x8b8) &&
      !B(0x148) && !active() && S.state != PAUSED && (i32)W(0x9e8) > 2)
    W(0x9e8) = 2;
}
static u32 get32(volatile u8* p) {
  return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}
static void put16(volatile u8* p, u32 n) {
  p[0] = n >> 8;
  p[1] = n;
}
static void put32(volatile u8* p, u32 n) {
  p[0] = n >> 24;
  p[1] = n >> 16;
  p[2] = n >> 8;
  p[3] = n;
}
static void reply(u8 op, u16 nonce, u8 result) {
  volatile u8* p = (volatile u8*)W(0xe0);
  int start_result = admission(0), resume_result = admission(1);
  p[0] = 0xfa;
  p++;
  p[0] = 0xd6;
  p[1] = 1;
  p[2] = 31;
  p[3] = op;
  p[4] = result;
  p[5] = S.state;
  p[6] = S.reason;
  p[7] = S.speed;
  p[8] = (start_result == OK ? 1 : 0) | (resume_result == OK ? 2 : 0) | (off() ? 4 : 0) |
         (active() ? 8 : 0);
  p[9] = B(0x88d);
  p[10] = B(0x895);
  p[11] = B(0xfc8) | (B(0xfc9) << 1) | (B(0xfca) << 2);
  put16(p + 12, nonce);
  put32(p + 14, S.token);
  put16(p + 18, active() && !due(NOW, S.deadline) ? S.deadline - NOW : 0);
  put16(p + 20, H(0x766));
  put16(p + 22, H(0x75a));
  put32(p + 24, W(0x7d4));
  put32(p + 28, W(0xa9c));
  put32(p + 32, NOW);
  put16(p + 36, H(0x120));
  p[38] = B(0x891);
  p[39] = B(0x8b9);
  p[40] = B(0x899);
  p[41] = B(0x88e);
  put32(p + 42, W(0x7e0));
  /* Capability bit 0x10 assigns the two legacy reserved bytes. */
  p[46] = start_result;
  p[47] = resume_result;
}
int rs_route(void) {
  volatile u8* p = (volatile u8*)W(0xe8);
  u32 saved = mask();
  rs_watchdog();
  if (p[0] != 0xd5) {
    if (active() || S.state == PAUSED || S.state == FAULT) {
      if (p[0] == 0x17 && B(0x124) == 5 && p[1] == 1 && p[3] == 0) {
        if (active())
          pause_reset(1);
        else
          cut();
        /* Let the preserved stock dispatcher emit its ordinary 18 ACK.
         * Output is cut even if that reply cannot fit in the TX ring. */
        int ack_room = (u8)(B(0xf4) - B(0xf5) - 1) >= 17;
        unmask(saved);
        return ack_room ? 0 : 1;
      }
      int read = (p[0] == 0xd1 && p[2] == 0) || (p[0] == 0xd3 && p[2] == 0) || p[0] == 0xa1 ||
                 p[0] == 0x11 || p[0] == 0x15;
      unmask(saved);
      return !read;
    }
    unmask(saved);
    return 0;
  }
  if (B(0x124) != 11) {
    unmask(saved);
    return 1;
  }
  u8 op = p[2], result = OK;
  u16 nonce = ((u16)p[3] << 8) | p[4];
  u32 request = get32(p + 5);
  int room = (u8)(B(0xf4) - B(0xf5) - 1) >= 101;
  if (p[1] != 1 || op > 5 || (op != 5 && p[9]) || (op == 5 && p[9] > 1) || (op == 0 && request))
    result = BAD;
  else if (op == 4) {
    if (active())
      pause_reset(1);
    else {
      S.token = 0;
      S.deadline = 0;
    }
  } else if (!room) {
    unmask(saved);
    return 1;
  } else if (op == 0) {
    if (!S.token || (!active() && due(NOW, S.challenge)))
      token(nonce);
    else if (!active())
      S.challenge = NOW + LEASE;
  } else if (!request || request != S.token || due(NOW, S.challenge))
    result = STALE;
  else if (op == 1 || op == 2) {
    result = admission(op == 2);
    if (result == OK) {
      if (op == 1)
        begin();
      else {
        S.state = S.phase;
        S.reason = 0;
        S.hold_cleanup_pending = 0;
        if (S.state == DESCENT) {
          B(0x7af) = 1;
          B(0x187) = 1;
        }
      }
      S.deadline = NOW + LEASE;
      token(nonce);
    }
  } else if (op == 3) {
    if (!active())
      result = INELIGIBLE;
    else {
      S.deadline = NOW + LEASE;
      token(nonce);
    }
  } else if (op == 5) {
    if (active() || S.state == PAUSED || H(0x75a) || B(0x88d) == 1 || B(0x88d) == 2)
      result = BUSY;
    else {
      S.speed = p[9];
      token(nonce);
    }
  }
  if (room)
    reply(op, nonce, result);
  unmask(saved);
  if (room) {
    B(0xe4) = 48;
    FN(0x08003611u, void (*)(u32, u32))(0x200000f0u, 0x200000e0u);
    H(0x120)++;
  }
  return 1;
}
