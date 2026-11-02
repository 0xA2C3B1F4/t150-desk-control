#include "tb01_core.h"

enum { IDLE, ON, PAUSE };

static uint16_t be16(const uint8_t* b) {
  return (uint16_t)(((uint16_t)b[0] << 8) | b[1]);
}
static bool due(uint32_t now, uint32_t deadline) {
  return (int32_t)(now - deadline) >= 0;
}
void tb_init(tb_state* s) {
  s->deadline = 0;
  s->on_ms = 0;
  s->pause_ms = 0;
  s->phase = IDLE;
  s->left = 0;
}
void tb_abort(tb_state* s, const tb_platform* p, void* ctx) {
  if (s->phase != IDLE)
    p->buzzer_release(ctx);
  tb_init(s);
}
void tb_tick(tb_state* s, const tb_platform* p, void* ctx, uint32_t now) {
  if (s->phase == IDLE)
    return;
  if ((s->phase != ON && s->phase != PAUSE) || !s->left || s->left > 20 ||
      (!due(now, s->deadline) && (uint32_t)(s->deadline - now) > 5000)) {
    tb_abort(s, p, ctx);
    return;
  }
  /* Do not replay pulses after a stalled loop. The platform must separately
   * keep tick service running and abort before planned tick stop/sleep. */
  if (!p->ready(ctx) || (due(now, s->deadline) && (uint32_t)(now - s->deadline) > 250)) {
    tb_abort(s, p, ctx);
    return;
  }
  if (!due(now, s->deadline))
    return;
  if (s->phase == ON) {
    p->buzzer_off(ctx);
    if (--s->left == 0) {
      p->buzzer_release(ctx);
      tb_init(s);
      return;
    }
    s->phase = PAUSE;
    s->deadline = now + s->pause_ms;
  } else {
    p->buzzer_on(ctx);
    s->phase = ON;
    s->deadline = now + s->on_ms;
  }
}
int tb_request(tb_state* s, const tb_platform* p, void* ctx, const uint8_t* q, size_t n,
               uint32_t now, uint8_t out[6]) {
  if (!n || q[0] != 0xd7)
    return TB_NOT_MINE;
  if (n < 5)
    return TB_CONSUMED_NO_REPLY;
  const uint8_t op = q[2];
  uint8_t result = TB_BAD;
  if (n == 10 && q[1] == 1 && op <= 3) {
    const uint16_t a = be16(q + 5), b = be16(q + 7);
    const uint8_t count = q[9];
    const bool valid =
        (op == 0 && !a && !b && !count) || (op == 1 && a >= 2 && a <= 720 && !b && !count) ||
        (op == 2 && !a && !b && !count) ||
        (op == 3 && a >= 50 && a <= 2000 && b >= 50 && b <= 5000 && count >= 1 && count <= 20);
    if (valid && op == 0)
      result = TB_OK;
    else if (valid && (!p->ready(ctx) || (s->phase != IDLE && op != 2)))
      result = TB_BUSY;
    else if (valid && op == 1)
      result = p->timer_set(ctx, a) ? TB_OK : TB_BACKEND;
    else if (valid && op == 2)
      result = p->timer_cancel(ctx) ? TB_OK : TB_BACKEND;
    else if (valid && op == 3) {
      if (!p->buzzer_start(ctx))
        result = TB_BACKEND;
      else {
        s->on_ms = a;
        s->pause_ms = b;
        s->left = count;
        s->phase = ON;
        s->deadline = now + a;
        result = TB_OK;
      }
    }
  }
  out[0] = 0xd8;
  out[1] = 1;
  out[2] = op;
  out[3] = q[3];
  out[4] = q[4];
  out[5] = result;
  return TB_REPLIED;
}
