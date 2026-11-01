#include "desk_protocol/reset_speed.h"
#include <cstdlib>
#include <iostream>
using namespace desk_protocol;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while (false)

static Frame reply(const Frame &request, uint16_t sequence = 1, uint8_t state = 0,
                   uint8_t flags = 5, uint8_t result = 0, uint32_t token = 0x12345678) {
  Frame f;
  f.size = 48; f.bytes[0] = 0xD6; f.bytes[1] = 1; f.bytes[2] = 0x0F;
  f.bytes[3] = request.bytes[2]; f.bytes[4] = result; f.bytes[5] = state; f.bytes[8] = flags;
  f.bytes[12] = request.bytes[3]; f.bytes[13] = request.bytes[4];
  for (unsigned i = 0; i < 4; ++i) f.bytes[14 + i] = static_cast<uint8_t>(token >> (24 - 8 * i));
  if (flags & 8) { f.bytes[18] = 5; f.bytes[19] = 0xDC; }
  f.bytes[36] = static_cast<uint8_t>(sequence >> 8); f.bytes[37] = static_cast<uint8_t>(sequence);
  return f;
}
static void discover(ResetSpeed &rs, uint32_t now = 0, uint8_t flags = 5, uint8_t state = 0,
                     uint16_t sequence = 1, uint32_t token = 0x12345678) {
  Frame f; CHECK(rs.probe(now)); CHECK(rs.take_frame(f));
  CHECK(f.size == 10 && f.bytes[0] == 0xD5 && f.bytes[1] == 1 && f.bytes[2] == 0);
  CHECK(direct_be32(f.bytes + 5) == 0 && f.bytes[9] == 0);
  rs.accept(reply(f, sequence, state, flags, 0, token), now + 1);
  CHECK(rs.known() && rs.supported() && rs.fresh(now + 1) && !rs.busy());
}
static void start(ResetSpeed &rs, uint32_t now = 2, uint16_t sequence = 2) {
  Frame f; CHECK(rs.begin(false, now)); CHECK(rs.take_frame(f));
  CHECK(f.bytes[2] == 1 && direct_be32(f.bytes + 5) == rs.reply().token);
  rs.accept(reply(f, sequence, 1, 8), now + 1); CHECK(rs.owned() && rs.blocking());
}
int main() {
  Frame f;
  { ResetSpeed rs; CHECK(!rs.begin(false, 0)); CHECK(!rs.set_speed(true, 0)); CHECK(!rs.cancel(0));
    CHECK(!rs.take_frame(f)); discover(rs); CHECK(rs.begin(false, 2)); CHECK(!rs.begin(false, 2));
    CHECK(rs.take_frame(f)); CHECK(!rs.take_frame(f)); }
  // Strict advertisement, payload extent, revision, op, enum and reserved bits.
  { Frame request; request.bytes[2] = 0; ResetSpeedReply value;
    const Frame good = reply(request); CHECK(reset_speed_reply(good, value));
    for (unsigned offset : {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 11U, 46U, 47U}) {
      Frame bad = good; bad.bytes[offset] = 0xFF; CHECK(!reset_speed_reply(bad, value));
    }
    for (unsigned size : {0U, 15U, 37U, 47U, 49U, 64U}) {
      Frame bad = good; bad.size = size; CHECK(!reset_speed_reply(bad, value));
    }
    Frame bad = reply(request, 1, 1, 8, 0, 0); CHECK(!reset_speed_reply(bad, value));
    bad = reply(request, 1, 3, 8); CHECK(!reset_speed_reply(bad, value));
    bad = reply(request, 1, 4, 4); bad.bytes[9] = 2; CHECK(!reset_speed_reply(bad, value));
    bad = reply(request, 1, 3, 6, 0, 0); CHECK(reset_speed_reply(bad, value));
  }
  // START/RESUME eligibility and token are independent from a stable Q value.
  { Frame request; ResetSpeedReply value;
    Frame modern = reply(request); modern.bytes[2] = 0x1F; modern.bytes[46] = 0; modern.bytes[47] = 3;
    CHECK(reset_speed_reply(modern, value) && value.admission_diagnostics);
    CHECK(value.start_result == 0 && value.resume_result == 3);
    for (unsigned offset : {46U, 47U}) {
      Frame bad = modern; bad.bytes[offset] = 7; CHECK(!reset_speed_reply(bad, value));
    }
    Frame bad = modern; bad.bytes[46] = 4; CHECK(!reset_speed_reply(bad, value));
    bad = modern; bad.bytes[47] = 0; CHECK(!reset_speed_reply(bad, value));
    bad = modern; bad.bytes[2] = 0x0F; CHECK(!reset_speed_reply(bad, value));
    modern = reply(request, 1, 3, 4); modern.bytes[2] = 0x1F; modern.bytes[46] = 4; modern.bytes[47] = 6;
    CHECK(reset_speed_reply(modern, value) && value.resume_result == 6);
    Frame legacy = reply(request); CHECK(reset_speed_reply(legacy, value) && !value.admission_diagnostics);
  }
  { ResetSpeed rs; discover(rs, 0, 4); CHECK(!rs.begin(false, 2)); CHECK(!rs.begin(true, 2)); }
  { ResetSpeed rs; discover(rs, 0, 6, 3, 1, 0); CHECK(rs.blocking()); CHECK(!rs.begin(true, 2)); }
  { ResetSpeed rs; discover(rs, 0, 6, 3); CHECK(!rs.begin(false, 2)); CHECK(rs.begin(true, 2)); }
  { ResetSpeed rs; discover(rs); CHECK(!rs.begin(false, 751)); }
  // Only a nonce/op/sequence-bound own ACK starts renewal. GET never owns it.
  { ResetSpeed rs; discover(rs); CHECK(rs.begin(false, 2)); CHECK(rs.take_frame(f));
    Frame bad = reply(f, 2, 1, 8); bad.bytes[13] ^= 1; rs.accept(bad, 3);
    CHECK(rs.busy() && !rs.owned()); rs.tick(802, true);
    CHECK(!rs.owned() && rs.take_brake() && !rs.take_brake()); CHECK(rs.take_failure()); CHECK(!rs.take_frame(f)); }
  { ResetSpeed rs; discover(rs, 0, 8, 1); rs.tick(301, true); CHECK(!rs.owned() && !rs.take_frame(f)); }
  { ResetSpeed rs; discover(rs); start(rs); CHECK(!rs.probe(4));
    rs.tick(302, true); CHECK(!rs.take_frame(f)); rs.tick(303, true);
    CHECK(rs.take_frame(f) && f.bytes[2] == 3 && direct_be32(f.bytes + 5) == 0x12345678);
    rs.accept(reply(f, 3, 2, 8, 0, 0x87654321), 304); CHECK(rs.owned());
    rs.tick(604, true); CHECK(rs.take_frame(f) && direct_be32(f.bytes + 5) == 0x87654321);
    rs.accept(reply(f, 4, 4, 4, 2, 0), 605);
    CHECK(!rs.owned() && !rs.blocking() && !rs.error() && !rs.take_brake());
    rs.tick(905, true); CHECK(!rs.take_frame(f)); CHECK(!rs.set_speed(true, 606));
    // A later stock error is a new idle eligibility, not a malformed retained
    // completion. Firmware reconciles COMPLETE to IDLE before this reply.
    CHECK(rs.probe(906)); CHECK(rs.take_frame(f));
    Frame eligible = reply(f, 5, 0, 5); eligible.bytes[9] = 8;
    rs.accept(eligible, 907); CHECK(rs.known() && rs.begin(false, 908)); }
  // Loss, reset, timeout and explicit Stop relinquish ownership. Status reads
  // can expose a paused reset but never continue it without a new request.
  { ResetSpeed rs; discover(rs); start(rs); rs.tick(4, false);
    CHECK(!rs.known() && !rs.owned() && rs.blocking() && rs.take_brake());
    CHECK(rs.cancel(5)); CHECK(rs.take_frame(f)); CHECK(f.bytes[2] == 4 && direct_be32(f.bytes + 5) == 0);
    rs.accept(reply(f, 1, 3, 6, 0, 0), 6); CHECK(!rs.owned() && rs.blocking() && rs.maintenance_safe(6));
    CHECK(!rs.begin(true, 7)); discover(rs, 8, 6, 3, 2); rs.tick(310, true); CHECK(!rs.take_frame(f));
    CHECK(rs.begin(true, 311)); }
  { ResetSpeed rs; discover(rs); start(rs); rs.observe_sequence(10);
    rs.tick(303, true); CHECK(rs.take_frame(f)); rs.accept(reply(f, 9, 1, 8), 304);
    CHECK(!rs.known() && !rs.owned() && rs.take_brake()); }
  { ResetSpeed rs; discover(rs); start(rs); rs.tick(303, true); CHECK(rs.take_frame(f));
    rs.accept(reply(f, 3, 1, 8, 2), 304); CHECK(!rs.owned() && rs.take_brake()); }
  { ResetSpeed rs; discover(rs); start(rs); rs.tick(303, true); CHECK(rs.take_frame(f));
    // A non-cleared token is not the firmware's successful-completion reply.
    rs.accept(reply(f, 3, 4, 4, 2, 123), 304); CHECK(rs.take_brake() && rs.error()); }
  { ResetSpeed rs; discover(rs); start(rs); rs.tick(303, true); CHECK(rs.take_frame(f));
    Frame wrong_nonce = reply(f, 3, 4, 4, 2, 0); wrong_nonce.bytes[13] ^= 1;
    rs.accept(wrong_nonce, 304); CHECK(rs.busy() && rs.owned() && !rs.take_brake());
    rs.accept(reply(f, 3, 4, 4, 2, 0), 305); CHECK(!rs.owned() && !rs.error() && !rs.take_brake()); }
  { ResetSpeed rs; CHECK(rs.probe(0)); CHECK(rs.take_frame(f));
    rs.accept(reply(f, 1, 4, 4, 2, 0), 1); CHECK(!rs.owned() && rs.error() && !rs.take_brake()); }
  { ResetSpeed rs; CHECK(rs.probe(0)); CHECK(rs.take_frame(f)); rs.tick(800, true);
    CHECK(!rs.known() && !rs.blocking() && !rs.take_brake());
    CHECK(std::strstr(rs.error(), "status unavailable") && !std::strstr(rs.error(), "reset paused")); }
  { ResetSpeed rs; discover(rs); start(rs); CHECK(rs.cancel(5)); CHECK(rs.take_frame(f));
    rs.tick(805, true); CHECK(!rs.owned() && !rs.take_brake() && rs.blocking()); CHECK(!rs.take_frame(f)); }
  // SPEED changes have confirmed readback, no persistence, no optimistic state
  // and no automatic retry. Reset blocks them even while paused.
  { ResetSpeed rs; discover(rs, 0, 4); CHECK(rs.set_speed(true, 2)); CHECK(rs.take_frame(f));
    CHECK(f.bytes[2] == 5 && f.bytes[9] == 1 && direct_be32(f.bytes + 5) == 0x12345678);
    Frame r = reply(f, 2, 0, 4); r.bytes[7] = 1; rs.accept(r, 3);
    CHECK(rs.reply().speed == 1 && !rs.busy() && !rs.owned() && !rs.blocking()); }
  { ResetSpeed rs; discover(rs, 0, 4); CHECK(rs.set_speed(true, 2)); CHECK(rs.take_frame(f));
    rs.accept(reply(f, 2, 0, 4), 3); CHECK(!rs.known() && !rs.owned() && !rs.take_brake()); }
  { ResetSpeed rs; discover(rs, 0, 6, 3); CHECK(!rs.set_speed(true, 2)); }
  { ResetSpeed rs; discover(rs, 0, 4); CHECK(rs.set_speed(true, 2)); CHECK(rs.take_frame(f));
    rs.tick(802, true); CHECK(!rs.known() && !rs.take_brake() && !rs.take_frame(f)); }
  // Sequence and timestamp wrap both retain the same admission rules.
  { ResetSpeed rs; discover(rs, 0xFFFFFFFE, 5, 0, 0xFFFF); CHECK(rs.begin(false, 0)); CHECK(rs.take_frame(f));
    rs.accept(reply(f, 0, 1, 8), 1); CHECK(rs.owned()); }
  std::cout << "RS01 protocol, lease ownership, cancellation and speed tests passed\n";
}
