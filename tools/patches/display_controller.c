#include "display_controller.h"
#include "display_core.h"
#include "motion_trim_controller.h"
#include "platform_runtime.h"
#ifdef DESK_LOCAL_PANEL_LOCK
#include "local_lock_controller.h"
#endif

#define BYTE(offset) (*(volatile uint8_t*)(0x20000000u + (offset)))
#define HALF(offset) (*(volatile uint16_t*)(0x20000000u + (offset)))
#define WORD(offset) (*(volatile uint32_t*)(0x20000000u + (offset)))
#define REGISTER(address) (*(volatile uint32_t*)(address))
#define DISPLAY ((DisplayState*)PLATFORM_DISPLAY_ADDRESS)
#define NATIVE(address) ((void (*)(void))(address))

_Static_assert(sizeof(DisplayState) == PLATFORM_DISPLAY_SIZE, "Display slot size changed");

static uint32_t mask(void) {
  uint32_t previous;
  __asm volatile("mrs %0, PRIMASK\ncpsid i" : "=r"(previous)::"memory");
  return previous;
}

static void restore(uint32_t previous) {
  __asm volatile("msr PRIMASK, %0" ::"r"(previous) : "memory");
}

static bool ready(void) {
  return platform_ready() && display_valid(DISPLAY);
}

static bool attention(void) {
  /* All non-idle renderer modes, motion outputs and native alarm paths remain
   * visible. The stale symbol selector at 0x899 alone is not an active mode. */
  if (BYTE(0x88d) || HALF(0x75a) || REGISTER(0x4000043cu) || REGISTER(0x40000440u) || BYTE(0x754) ||
      BYTE(0x148) || BYTE(0x88f) || BYTE(0x890) || BYTE(0x892) || BYTE(0x893) || BYTE(0x894) ||
      BYTE(0x8b8) || BYTE(0x8b9) || BYTE(0x8ba) || BYTE(0xaa0) || BYTE(0xaa1) || BYTE(0x18e) ||
      BYTE(0x18c) || BYTE(0x8a8) || BYTE(0xfc9) || BYTE(0xfca) ||
      (REGISTER(0x4000280cu) & 0x100u) || (REGISTER(0x40010414u) & (1u << 17))) {
    return true;
  }
  if ((REGISTER(0x48000010u) & 0x1f00u) != 0x1f00u ||
      (REGISTER(0x48000410u) & 0xc000u) != 0xc000u) {
    return true;
  }
  for (unsigned offset = 0x7ad; offset <= 0x7b4; ++offset) {
    if (BYTE(offset)) {
      return true;
    }
  }
  for (unsigned offset = 0xa10; offset <= 0xa80; offset += 16) {
    if (BYTE(offset + 4) || HALF(offset + 12) || BYTE(offset + 14)) {
      return true;
    }
  }
  return false;
}

void display_controller_init(void) {
  if (platform_ready()) {
    display_init(DISPLAY, WORD(0x75c));
  }
}

void display_controller_restart(void) {
  uint32_t previous = mask();
  if (ready()) {
    display_restart(DISPLAY, WORD(0x75c));
  }
  restore(previous);
}

void display_controller_sleep(void) {
  uint32_t previous = mask();
  if (ready()) {
    display_sleep(DISPLAY);
  }
  restore(previous);
}

void display_controller_render(void) {
#ifdef DESK_LOCAL_PANEL_LOCK
  if (local_lock_controller_render()) {
    return;
  }
#endif
  uint32_t previous = mask();
  bool blank = ready() && display_render_blank(DISPLAY, WORD(0x75c), attention());
  restore(previous);
  /* The blank routine sets only PB5/PB10/PB11. PB2 belongs to the timer. */
  NATIVE(blank ? 0x080036adu : 0x08003f1du)();
}

void display_controller_native_stop(void) {
  const volatile uint8_t* request = (const volatile uint8_t*)WORD(0xe8);
  if (BYTE(0x124) == 5 && request[0] == 0x17 && request[1] == 1 && request[3] == 0) {
    display_controller_restart();
  }
}

void display_controller_reset_stop(int handled) {
  if (!handled) {
    return;
  }
  const volatile uint8_t* request = (const volatile uint8_t*)WORD(0xe8);
  if (BYTE(0x124) == 11 && request[0] == 0xd5 && request[1] == 1 && request[2] == 4 &&
      request[9] == 0) {
    display_controller_restart();
  } else {
    /* RS01 can cut output without a TX acknowledgement when the ring is full. */
    display_controller_native_stop();
  }
}

int display_controller_route(void) {
  const volatile uint8_t* request = (const volatile uint8_t*)WORD(0xe8);
  uint8_t length = BYTE(0x124);
  if (request[0] != 0xd7 || length < 3 || request[1] != 4) {
    return 0;
  }
  if (length != 13) {
    return 1;
  }
  uint32_t previous = mask();
  /* Preserve the trim transaction's promised response space as well as the
   * display frame's worst-case escaped size. Nothing changes without both. */
  if (!trim_controller_reply_room(25)) {
    restore(previous);
    return 1;
  }
  uint8_t reply[10] = {0xd8, 4, request[2], request[3], request[4], 1, 3, DISPLAY_STATE_UNAVAILABLE,
                       0,    0};
  uint16_t seconds = ((uint16_t)request[7] << 8) | request[8];
  bool valid = request[2] <= 1 && request[5] == 0 && request[9] == 0 && request[10] == 0 &&
               request[11] == 0 &&
               (request[2] == 0 ? (request[6] == 0 && seconds == 0)
                                : (request[6] <= DISPLAY_NORMAL && seconds != 0));
  if (valid) {
    reply[5] = 2;
    if (!previous && ready()) {
      uint32_t now = WORD(0x75c);
      if (request[2] == 1) {
        display_configure(DISPLAY, request[6], seconds, now);
      }
      reply[5] = 0;
      reply[7] = display_status(DISPLAY, now, attention());
      reply[8] = (uint8_t)(DISPLAY->timeout_seconds >> 8);
      reply[9] = (uint8_t)DISPLAY->timeout_seconds;
    }
  }
  trim_controller_reply(reply, sizeof(reply));
  restore(previous);
  return 1;
}
