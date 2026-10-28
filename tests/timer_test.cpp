#include "desk_protocol/timer.h"
#include <cstdlib>
#include <iostream>
using namespace desk_protocol;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while (false)

static Frame ack(bool pressed) {
  Frame f; f.size = 6; f.bytes[0] = 0x18; f.bytes[1] = 9; f.bytes[2] = pressed; return f;
}
static Frame take(TimerSequence &t, bool pressed) {
  Frame f; CHECK(t.take_frame(f));
  CHECK(f.size == 3 && f.bytes[0] == 0x17 && f.bytes[1] == 9 && f.bytes[2] == pressed);
  Packet p; CHECK(encode(f, p));
  CHECK(p.size == 6 && p.bytes[3] == pressed && p.bytes[4] == (pressed ? 0x1F : 0x1E));
  return f;
}

// Independent model of the recovered T handler: a short key release produces
// a single-click event after 300 ms; selections 1..4 commit after 4000 ms and
// reset the selection byte. The fifth click disables the alarm. This model
// cannot establish real firmware timing, button acceptance or physical sound.
static void workflow(uint16_t minutes, unsigned initial, uint32_t origin = 0) {
  TimerSequence t;
  CHECK(t.begin(minutes, origin));
  unsigned selection = initial, alarm_minutes = 60, presses = 0, releases = 0;
  bool pending_click = false, held = false;
  uint32_t click_at = 0, menu_deadline = initial ? 4000 : 0, pressed_at = 0;
  Frame f;
  for (uint32_t elapsed = 0; elapsed <= 16000; elapsed += 10) {
    const uint32_t now = origin + elapsed;
    if (pending_click && elapsed >= click_at) {
      pending_click = false;
      ++selection;
      if (selection == 5) { alarm_minutes = 0; selection = 0; menu_deadline = 0; }
      else menu_deadline = elapsed + 4000;
    }
    if (menu_deadline && elapsed >= menu_deadline) {
      alarm_minutes = selection * 30;
      selection = 0;
      menu_deadline = 0;
    }
    t.tick(now, true, 2000);
    while (t.take_frame(f)) {
      CHECK(f.size == 3 && f.bytes[0] == 0x17 && f.bytes[1] == 9);
      if (f.bytes[2]) {
        CHECK(elapsed >= 5000 && !held);
        held = true; pressed_at = elapsed; ++presses;
      } else {
        CHECK(held && elapsed - pressed_at == 150);
        held = false; ++releases;
        pending_click = true; click_at = elapsed + 300;
      }
      t.accept(ack(f.bytes[2] != 0), now);
    }
  }
  CHECK(!t.busy() && t.completed() && !t.error() && !t.release_fault());
  CHECK(!held && !pending_click && selection == 0 && alarm_minutes == minutes);
  CHECK(presses == (minutes ? minutes / 30U : 5U) && presses == releases);
}

int main() {
  for (uint16_t minutes : {0, 30, 60, 90, 120})
    for (unsigned initial = 0; initial <= 4; ++initial) workflow(minutes, initial);
  workflow(0, 4, 0xFFFFF000U);
  Frame f;
  {
    TimerSequence t;
    t.tick(10000, true, 2000); CHECK(!t.take_frame(f));
    for (uint16_t invalid : {1, 29, 31, 121, 65535}) CHECK(!t.begin(invalid, 0));
    CHECK(t.begin(30, 0)); CHECK(!t.begin(0, 1));
    t.tick(4999, true, 2000); CHECK(!t.take_frame(f));
    t.cancel(4999); t.tick(20000, true, 2000); CHECK(!t.take_frame(f) && !t.completed());
  }
  {
    TimerSequence t; CHECK(t.begin(120, 0));
    t.tick(5000, true, 2000); take(t, true);
    t.cancel(5050); take(t, false); CHECK(t.busy());
    t.accept(ack(false), 5060); t.tick(5060, true, 2000);
    CHECK(!t.busy() && t.error() && !t.completed());
    t.tick(10000, true, 2000); CHECK(!t.take_frame(f));
  }
  {
    TimerSequence t; CHECK(t.begin(0, 0));
    t.tick(5000, true, 2000); take(t, true); t.accept(ack(true), 5001);
    t.tick(5020, false, 0); take(t, false);
    t.accept(ack(false), 5021); t.tick(5021, false, 0);
    CHECK(!t.busy() && t.error() && !t.release_fault());
    t.tick(15000, true, 2000); CHECK(!t.take_frame(f));
  }
  {
    TimerSequence t; CHECK(t.begin(0, 0));
    t.tick(5000, true, 2000); take(t, true);
    Frame wrong = ack(true); wrong.bytes[1] = 8; t.accept(wrong, 5001);
    t.poll_release(5150); take(t, false);
    t.accept(ack(false), 5151);
    t.accept(ack(true), 5500); // Expired press ACK must not advance the sequence.
    t.tick(5500, true, 2000);
    CHECK(!t.busy() && t.error() && !t.release_fault());
    t.tick(10000, true, 2000); CHECK(!t.take_frame(f));
  }
  {
    TimerSequence t; CHECK(t.begin(30, 0));
    t.tick(5000, true, 2000); take(t, true); t.accept(ack(true), 5001);
    t.poll_release(5150); take(t, false);
    t.accept(ack(false), 5650); t.tick(5650, true, 2000);
    CHECK(t.release_fault() && !t.busy() && !t.begin(30, 10000));
  }
  {
    TimerSequence t; CHECK(t.begin(60, 0));
    t.tick(5000, true, 2000); take(t, true); t.accept(ack(true), 5001);
    t.poll_release(5150); take(t, false); t.accept(ack(false), 5151);
    t.tick(5151, true, 2000); t.tick(7000, true, 2000);
    CHECK(!t.busy() && t.error() && !t.take_frame(f));
  }
  {
    TimerSequence t; CHECK(t.begin(30, 0));
    t.tick(5000, true, 2000); take(t, true); t.accept(ack(true), 5001);
    t.poll_release(6000); take(t, false); // A stalled loop must release first.
    t.accept(ack(false), 6001); t.tick(6001, true, 2000);
    CHECK(!t.busy() && !t.completed() && t.error() && t.release_fault());
    CHECK(!t.begin(30, 10000));
  }
  {
    // Stop must not replace an existing failure while its release is draining.
    TimerSequence t; CHECK(t.begin(30, 0));
    t.tick(5000, true, 2000); take(t, true);
    t.poll_release(5150); take(t, false);
    t.tick(5500, true, 2000); // Neither ACK arrived; release has 150 ms left.
    CHECK(t.busy() && t.error() && !t.release_fault());
    const char *error = t.error();
    t.cancel(5510);
    CHECK(t.error() == error && t.busy() && !t.take_frame(f));
    t.tick(5650, true, 2000);
    CHECK(t.release_fault() && !t.busy() && t.error() != error);
  }
  {
    TimerSequence t; CHECK(t.begin(30, 0));
    t.tick(4999, true, 1499); CHECK(!t.busy() && !t.take_frame(f));
  }
  std::cout << "Timer durations/cancel, all initial selections, framing, ACK faults, interruption, delays and wrap passed\n";
}
