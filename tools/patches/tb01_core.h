#ifndef DESK_TIMER_BUZZER_CORE_H
#define DESK_TIMER_BUZZER_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Offline controller logic. The platform MUST serialize all calls and provide
 * a monotonic millisecond tick and safe buzzer ownership. Tick service must
 * continue independently of the main loop; abort before a planned tick stop
 * or CPU sleep. Unexpected permanent IRQ loss/faults are not bounded here. */
typedef struct {
  /* No reset, motion, key/menu, or native alarm. The output claimed by this
   * component is permitted: this predicate is also called while it plays. */
  bool (*ready)(void* ctx);
  bool (*timer_set)(void* ctx, uint16_t minutes);
  bool (*timer_cancel)(void* ctx);
  bool (*buzzer_start)(void* ctx);   /* atomically claim output, turn it on */
  void (*buzzer_off)(void* ctx);     /* finish owned pulse, retaining claim */
  void (*buzzer_on)(void* ctx);      /* begin next pulse while owned */
  void (*buzzer_release)(void* ctx); /* off and release; not native sounds */
} tb_platform;

typedef struct {
  uint32_t deadline;
  uint16_t on_ms, pause_ms;
  uint8_t phase, left;
} tb_state;

enum { TB_OK = 0, TB_BAD = 1, TB_BUSY = 2, TB_BACKEND = 3 };
enum { TB_NOT_MINE = 0, TB_CONSUMED_NO_REPLY = 1, TB_REPLIED = 2 };
void tb_init(tb_state* s);
/* Request is the ten-byte decoded body without checksum. After preserved
 * RS01 priority routing, malformed D7 is consumed without falling through
 * into the ordinary stock dispatcher. */
int tb_request(tb_state* s, const tb_platform* p, void* ctx, const uint8_t* request, size_t n,
               uint32_t now, uint8_t out[6]);
void tb_tick(tb_state* s, const tb_platform* p, void* ctx, uint32_t now);
/* Call before sleep or reset while platform still owns the buzzer. */
void tb_abort(tb_state* s, const tb_platform* p, void* ctx);

#endif
