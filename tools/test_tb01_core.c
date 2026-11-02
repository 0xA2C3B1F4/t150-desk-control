#include "tb01_core.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  bool ready, output, claimed, backend_ok;
  unsigned timer_sets, cancels, starts, ons, offs, releases;
  uint16_t minutes;
} Mock;
static bool ready(void* v) {
  return ((Mock*)v)->ready;
}
static bool timer_set(void* v, uint16_t m) {
  Mock* x = v;
  x->timer_sets++;
  x->minutes = m;
  return x->backend_ok;
}
static bool timer_cancel(void* v) {
  Mock* x = v;
  x->cancels++;
  return x->backend_ok;
}
static bool start(void* v) {
  Mock* x = v;
  x->starts++;
  if (!x->backend_ok)
    return false;
  x->output = x->claimed = true;
  return true;
}
static void off(void* v) {
  Mock* x = v;
  assert(x->claimed);
  x->output = false;
  x->offs++;
}
static void on(void* v) {
  Mock* x = v;
  assert(x->claimed);
  x->output = true;
  x->ons++;
}
static void release(void* v) {
  Mock* x = v;
  assert(x->claimed);
  x->output = x->claimed = false;
  x->releases++;
}
static const tb_platform p = {ready, timer_set, timer_cancel, start, off, on, release};

static uint8_t req(tb_state* s, Mock* m, uint8_t op, uint16_t a, uint16_t b, uint8_t count,
                   uint32_t now) {
  uint8_t q[] = {0xd7,       1,    op, 0x4a, 0xfd, (uint8_t)(a >> 8), (uint8_t)a, (uint8_t)(b >> 8),
                 (uint8_t)b, count},
          o[6];
  assert(tb_request(s, &p, m, q, sizeof q, now, o) == TB_REPLIED);
  assert(o[0] == 0xd8 && o[1] == 1 && o[2] == op && o[3] == 0x4a && o[4] == 0xfd);
  return o[5];
}

int main(void) {
  tb_state s;
  tb_init(&s);
  Mock m = {.ready = true, .backend_ok = true};
  assert(req(&s, &m, 0, 0, 0, 0, 0) == TB_OK);
  assert(req(&s, &m, 1, 15, 0, 0, 0) == TB_OK && m.minutes == 15 && m.timer_sets == 1);
  assert(req(&s, &m, 1, 0, 0, 0, 0) == TB_BAD && m.timer_sets == 1);
  assert(req(&s, &m, 1, 1, 0, 0, 0) == TB_BAD && m.timer_sets == 1);
  assert(req(&s, &m, 1, 721, 0, 0, 0) == TB_BAD && m.timer_sets == 1);
  assert(req(&s, &m, 2, 0, 0, 0, 0) == TB_OK && m.cancels == 1);
  m.backend_ok = false;
  assert(req(&s, &m, 1, 15, 0, 0, 0) == TB_BACKEND);
  assert(req(&s, &m, 3, 100, 200, 1, 0) == TB_BACKEND && !m.claimed);
  m.backend_ok = true;
  m.ready = false;
  assert(req(&s, &m, 0, 0, 0, 0, 0) == TB_OK);
  assert(req(&s, &m, 3, 100, 200, 1, 0) == TB_BUSY);
  m.ready = true;
  assert(req(&s, &m, 3, 49, 200, 1, 0) == TB_BAD);
  assert(req(&s, &m, 3, 2001, 200, 1, 0) == TB_BAD);
  assert(req(&s, &m, 3, 100, 5001, 1, 0) == TB_BAD);
  assert(req(&s, &m, 3, 100, 49, 1, 0) == TB_BAD);
  assert(req(&s, &m, 3, 100, 200, 0, 0) == TB_BAD);
  assert(req(&s, &m, 3, 100, 200, 21, 0) == TB_BAD);
  assert(req(&s, &m, 3, 100, 200, 3, 0) == TB_OK && m.output);
  assert(req(&s, &m, 1, 15, 0, 0, 0) == TB_BUSY);
  assert(req(&s, &m, 2, 0, 0, 0, 0) == TB_OK && m.output);
  tb_tick(&s, &p, &m, 99);
  assert(m.output);
  tb_tick(&s, &p, &m, 100);
  assert(!m.output && m.claimed);
  tb_tick(&s, &p, &m, 299);
  assert(!m.output);
  tb_tick(&s, &p, &m, 300);
  assert(m.output);
  tb_tick(&s, &p, &m, 400);
  assert(!m.output);
  tb_tick(&s, &p, &m, 600);
  assert(m.output);
  tb_tick(&s, &p, &m, 700);
  assert(!m.output && !m.claimed && m.releases == 1);
  assert(m.ons == 2 && m.offs == 3);
  assert(req(&s, &m, 3, 100, 200, 2, 0xfffffff0u) == TB_OK);
  tb_tick(&s, &p, &m, 83);
  assert(m.output);
  tb_tick(&s, &p, &m, 84);
  assert(!m.output && m.claimed);
  tb_abort(&s, &p, &m);
  assert(!m.claimed);
  assert(req(&s, &m, 3, 100, 200, 2, 0) == TB_OK);
  tb_tick(&s, &p, &m, 351);
  assert(!m.claimed && !m.output);
  assert(req(&s, &m, 3, 100, 200, 2, 0) == TB_OK);
  m.ready = false;
  tb_tick(&s, &p, &m, 1);
  assert(!m.claimed && !m.output);
  uint8_t bad[] = {0xd7, 1, 3, 9, 0xfa}, out[6];
  assert(tb_request(&s, &p, &m, bad, sizeof bad, 0, out) == TB_REPLIED);
  assert(out[2] == 3 && out[3] == 9 && out[4] == 0xfa && out[5] == TB_BAD);
  memset(out, 0x5a, sizeof out);
  assert(tb_request(&s, &p, &m, bad, 4, 0, out) == TB_CONSUMED_NO_REPLY && out[0] == 0x5a);
  bad[0] = 0xd5;
  memset(out, 0x5a, sizeof out);
  assert(tb_request(&s, &p, &m, bad, sizeof bad, 0, out) == TB_NOT_MINE && out[0] == 0x5a);
  m.ready = true;
  assert(req(&s, &m, 3, 100, 200, 2, 0) == TB_OK);
  s.phase = 99;
  tb_tick(&s, &p, &m, 1);
  assert(!m.output && !m.claimed && s.phase == 0);
  for (unsigned fault = 0; fault < 3; ++fault) {
    assert(req(&s, &m, 3, 100, 200, 2, 0) == TB_OK);
    if (fault == 0)
      s.left = 0;
    else if (fault == 1)
      s.left = 21;
    else
      s.deadline = 6000;
    tb_tick(&s, &p, &m, 1);
    assert(!m.output && !m.claimed && s.phase == 0);
  }
  /* Exact upper bounds remain valid over the full twenty-pulse sequence. */
  assert(req(&s, &m, 3, 2000, 5000, 20, 0) == TB_OK);
  for (unsigned pulse = 0; pulse < 20; ++pulse) {
    tb_tick(&s, &p, &m, pulse * 7000 + 2000);
    assert(!m.output);
    if (pulse < 19) {
      assert(m.claimed);
      tb_tick(&s, &p, &m, (pulse + 1) * 7000);
      assert(m.output);
    }
  }
  assert(!m.claimed && !m.output && s.phase == 0);
  /* An empty body must not read a null input or modify the response buffer. */
  memset(out, 0x5a, sizeof out);
  assert(tb_request(&s, &p, &m, NULL, 0, 0, out) == TB_NOT_MINE && out[0] == 0x5a);
  assert(req(&s, &m, 3, 50, 50, 1, 0) == TB_OK);
  tb_tick(&s, &p, &m, 50);
  assert(!m.claimed && !m.output);
  assert(req(&s, &m, 1, 2, 0, 0, 0) == TB_OK && m.minutes == 2);
  assert(req(&s, &m, 1, 720, 0, 0, 0) == TB_OK && m.minutes == 720);
  assert(req(&s, &m, 0, 1, 0, 0, 0) == TB_BAD);
  assert(req(&s, &m, 2, 0, 1, 0, 0) == TB_BAD);
  puts("controller core behavior: pass");
  return 0;
}
