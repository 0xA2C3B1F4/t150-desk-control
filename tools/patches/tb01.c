/* TB01: native RTC timer plus tick-driven active-buzzer sequence.
 * Exact RS01 R7 base only. Hardware acceptance is recorded separately.
 * RTC timing intentionally matches the native T timer (seconds become zero).
 */
#include "tb01_core.h"
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#define B(o) (*(volatile u8*)(0x20000000u + (o)))
#define H(o) (*(volatile u16*)(0x20000000u + (o)))
#define W(o) (*(volatile u32*)(0x20000000u + (o)))
#define REG(a) (*(volatile u32*)(a))
#define FN(a, t) ((t)(a))
#ifndef TB01_STATE_ADDRESS
#define TB01_STATE_ADDRESS 0x20000ff0u
#endif
#ifndef TB01_READY
#define TB01_READY() true
#endif
#ifndef TB01_INITIALIZED
#define TB01_INITIALIZED() true
#endif
#ifndef TB01_MARK_READY
#define TB01_MARK_READY() ((void)0)
#endif
#ifndef TB01_ACCEPT_VERSION
#define TB01_ACCEPT_VERSION(version) true
#endif
#define STATE ((tb_state*)TB01_STATE_ADDRESS)
#define RTC_CR REG(0x40002808u)
#define RTC_ISR REG(0x4000280cu)
#define RTC_ALARM REG(0x4000281cu)
#define EXTI_PR REG(0x40010414u)
#define PF_SET REG(0x48001418u)
#define PF_OFF REG(0x48001428u)
#define PB_SET REG(0x48000418u)
#define PB_OFF REG(0x48000428u)
_Static_assert(sizeof(tb_state) == 12, "TB01 state reservation changed");

static u32 lock(void) {
  u32 v;
  __asm volatile("mrs %0, PRIMASK\ncpsid i" : "=r"(v)::"memory");
  return v;
}
static void unlock(u32 v) {
  __asm volatile("msr PRIMASK, %0" ::"r"(v) : "memory");
}
static bool ready(void* unused) {
  if (!TB01_READY())
    return false;
  /* Request context points to the decoded opcode; SysTick passes null.
   * Only setting a timer requires the RTC alarm to be unarmed. */
  if (unused && *(const u8*)unused == 1 && (RTC_CR & 0x1100u))
    return false;
  if (B(0x88d) || H(0x75a) || REG(0x4000043cu) || REG(0x40000440u) || B(0x14d) || B(0x8a8) ||
      B(0x18c) || B(0x754) || B(0x88f) || B(0x890) || B(0x148) || B(0x8b8) || B(0x892) ||
      B(0x894) || B(0xaa0) || B(0xaa1) || B(0x18e) || B(0x8ba) || B(0xfc8) || B(0xfc9) || B(0xfca))
    return false;
  for (u32 i = 0x7ad; i <= 0x7b4; ++i)
    if (B(i))
      return false;
  for (u32 i = 0xa10; i < 0xa90; i += 16)
    if (B(i + 4) || H(i + 12) || B(i + 14))
      return false;
  /* A previous partial failure may leave ALRAF latched with delivery off.
   * Permit cancel to drain it, while keeping set/beep blocked. */
  bool recover = unused && *(const u8*)unused == 2 && !(RTC_CR & 0x1000u);
  return (REG(0x48000010u) & 0x1f00u) == 0x1f00u && (REG(0x48000410u) & 0xc000u) == 0xc000u &&
         (recover || (!(RTC_ISR & 0x100u) && !(EXTI_PR & (1u << 17))));
}
static u32 bcd(u32 n) {
  u32 tens = 0;
  while (n >= 10) {
    n -= 10;
    tens += 16;
  }
  return tens + n;
}
static bool timer_cancel(void* unused) {
  (void)unused;
  u32 previous = RTC_CR;
  /* Same bounded native helpers as the T-off path, with IRQs left enabled. */
  FN(0x08006aa1u, void (*)(u32, u32))(0x1000u, 0);
  u32 ok = FN(0x0800670du, u32 (*)(u32, u32))(0x100u, 0);
  if (RTC_CR & 0x100u) {
    /* A failed disable must not silently turn a live alarm into a timer
     * without an interrupt. Restore its previous delivery policy. */
    if (previous & 0x1000u)
      FN(0x08006aa1u, void (*)(u32, u32))(0x1000u, 1);
    if (!(RTC_CR & 0x1000u)) {
      u32 saved = lock();
      if (!B(0x8a8) && !B(0x14d)) {
        W(0x8b4) = 0;
        PB_OFF = 4;
      }
      unlock(saved);
    }
    return false;
  }
  u32 saved = lock();
  /* The alarm is disabled now. Drain a possibly latched event using the
   * exact stock rc_w0 helper and stock EXTI W1C helper, then clear UI state. */
  FN(0x08006871u, void (*)(u32))(0x1000u);
  FN(0x08004065u, void (*)(u32))(1u << 17);
  /* A8 identifies delivered timer expiry; 14d also describes unrelated
   * sounds. Preserve a delivered timer event, but not a stale timer display
   * merely because a key click is playing. Hardware pending is drained in
   * both cases now that ALRAE is confirmed off. */
  bool delivered = B(0x8a8) != 0;
  if (!delivered) {
    W(0x8b4) = 0;
    B(0x18c) = 0;
    PB_OFF = 4;
  }
  unlock(saved);
  return !delivered && ok != 0 && !(RTC_CR & 0x1100u);
}
static bool timer_set(void* unused, u16 minutes) {
  (void)unused;
  /* No live RTC alarm can race the native setup. Cancel it explicitly first.
   * This runs in the normal UART dispatcher, never under inherited PRIMASK. */
  u32 saved = lock();
  bool blocked = (RTC_CR & 0x1140u) || !ready(0);
  unlock(saved);
  if (blocked)
    return false; /* ALRAE, ALRAIE, 12-hour mode or delivered event */
  FN(0x08006985u, void (*)(u32))(0x200000d8u);
  u32 h = B(0xdc), m = B(0xdd);
  if (h >= 24 || m >= 60)
    return false;
  m += minutes;
  while (m >= 60) {
    m -= 60;
    if (++h == 24)
      h = 0;
  }
  /* Native initializer stores day=1, with the day comparator masked. */
  u32 expected = 0x81000000u | (bcd(h) << 16) | (bcd(m) << 8);
  /* 0x67ec uses the same 0xdc/0xdd RAM snapshot above; it does not reread RTC. */
  FN(0x080067edu, void (*)(u32))(minutes);
  if ((RTC_CR & 0x1100u) != 0x1100u || RTC_ALARM != expected) {
    (void)timer_cancel(0);
    return false;
  }
  W(0x8b4) = minutes;
  PB_SET = 4;
  return true;
}
static bool buzzer_start(void* unused) {
  (void)unused;
  PF_SET = 0x80;
  return true;
}
static void buzzer_off(void* unused) {
  (void)unused;
  PF_OFF = 0x80;
}
static void buzzer_on(void* unused) {
  (void)unused;
  PF_SET = 0x80;
}
static const tb_platform platform = {ready,      timer_set, timer_cancel, buzzer_start,
                                     buzzer_off, buzzer_on, buzzer_off};
void tb01_abort(void) {
  u32 saved = lock();
  if (TB01_READY())
    tb_abort(STATE, &platform, 0);
  unlock(saved);
}
void tb01_native_priority(u32 mode, u32 count) {
  /* Native main-loop service calls continue even after the AA pattern ends.
   * They must not cancel us when the native body will not touch its output. */
  if (mode == 0xffu || (mode == 0xaau && B(0x14e) < count))
    tb01_abort();
}
void tb01_init(void) {
  /* Initialize our unzeroed reservation before native init can enable work. */
  u32 saved = lock();
  if (TB01_INITIALIZED()) {
    tb_init(STATE);
    TB01_MARK_READY();
  }
  unlock(saved);
  FN(0x080094a1u, void (*)(void))();
}
void tb01_watchdog(void) {
  FN(0x080094c1u, void (*)(void))();
  /* A higher-priority native-sound IRQ must not reset STATE mid-update. */
  u32 saved = lock();
  if (TB01_READY())
    tb_tick(STATE, &platform, 0, W(0x75c));
  unlock(saved);
}
void tb01_sleep(void) {
  tb01_abort();
  FN(0x08007569u, void (*)(void))();
}
int tb01_route(void) {
#ifndef TB01_EXTENSION_ONLY
  int original = FN(0x080097e1u, int (*)(void))();
  if (original)
    return original;
#endif
  const volatile u8* r = (const volatile u8*)W(0xe8);
  if (r[0] != 0xd7)
    return 0;
  u8 n = B(0x124), q[10], out[6];
  if (n < 6 || n > 11)
    return 1;
  for (u8 i = 0; i < (u8)(n - 1); ++i)
    q[i] = r[i];
  if (!TB01_ACCEPT_VERSION(q[1]))
    return 1;
  if ((u8)(B(0xf4) - B(0xf5) - 1) < 16)
    return 1;
  u32 mask;
  __asm volatile("mrs %0, PRIMASK" : "=r"(mask));
  if (mask)
    return 1; /* Do not enter native RTC waits with IRQs masked. */
  int result;
  if (!TB01_READY()) {
    out[0] = 0xd8;
    out[1] = 1;
    out[2] = q[2];
    out[3] = q[3];
    out[4] = q[4];
    out[5] = TB_BACKEND;
    result = TB_REPLIED;
  } else if (q[2] == 1 || q[2] == 2) {
    result = tb_request(STATE, &platform, &q[2], q, n - 1, W(0x75c), out);
  } else {
    u32 saved = lock();
    result = tb_request(STATE, &platform, 0, q, n - 1, W(0x75c), out);
    unlock(saved);
  }
  if (result == TB_REPLIED) {
    volatile u8* tx = (volatile u8*)W(0xe0);
    tx[0] = 0xfa;
    for (u8 i = 0; i < 6; ++i)
      tx[i + 1] = out[i];
    B(0xe4) = 6;
    FN(0x08003611u, void (*)(u32, u32))(0x200000f0u, 0x200000e0u);
    H(0x120)++;
  }
  return 1;
}
