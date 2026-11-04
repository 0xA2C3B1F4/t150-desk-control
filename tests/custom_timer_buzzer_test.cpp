#include <cstdlib>
#include <iostream>
#include "desk_protocol/custom_timer_buzzer.h"
using namespace desk_protocol;
#define CHECK(x)                             \
  do {                                       \
    if (!(x)) {                              \
      std::cerr << __LINE__ << ": " #x "\n"; \
      std::exit(1);                          \
    }                                        \
  } while (false)

static Frame reply(uint8_t operation, uint16_t nonce, uint8_t result) {
  Frame f{};
  f.size = 6;
  f.bytes[0] = 0xD8;
  f.bytes[1] = 1;
  f.bytes[2] = operation;
  f.bytes[3] = nonce >> 8;
  f.bytes[4] = nonce;
  f.bytes[5] = result;
  return f;
}
int main() {
  {
    CustomBuzzerDeferral deferral;
    CHECK(!deferral.active(0));
    CHECK(deferral.begin(2000, 5000, 20, 100));
    CHECK(deferral.active(30100));
    CHECK(deferral.active(138599));
    CHECK(!deferral.active(138600));
    CHECK(deferral.begin(2000, 5000, 20, 0xFFFFFF00U));
    CHECK(deferral.active(0x00011070U));
    CHECK(deferral.active(static_cast<uint32_t>(0xFFFFFF00U + 138499U)));
    CHECK(!deferral.active(static_cast<uint32_t>(0xFFFFFF00U + 138500U)));
    deferral.expire(static_cast<uint32_t>(0xFFFFFF00U + 138500U));
    CHECK(!deferral.active(0xFFFFFF00U));
    CHECK(!deferral.begin(2001, 50, 1, 0));
    CHECK(deferral.begin(2000, 5000, 20, 100));
    CHECK(deferral.begin(50, 50, 1, 200));
    CHECK(deferral.active(138599) && !deferral.active(138600));
    CHECK(deferral.begin(2000, 5000, 20, 100000));
    CHECK(deferral.active(238499) && !deferral.active(238500));
    CHECK(deferral.begin(2000, 5000, 20, 0xFFFFFF00U));
    CHECK(deferral.begin(50, 50, 1, 0x00000300U));
    CHECK(deferral.active(static_cast<uint32_t>(0xFFFFFF00U + 138499U)));
    CHECK(!deferral.active(static_cast<uint32_t>(0xFFFFFF00U + 138500U)));
    deferral.expire(static_cast<uint32_t>(0xFFFFFF00U + 138500U));
    CHECK(!deferral.active(0xFFFFFF00U));
    CHECK(deferral.begin(50, 50, 1, 100));
    deferral.expire(3650);
    CHECK(!deferral.active(100));
  }
  {
    CustomAlarmDeferral alarm;
    CHECK(alarm.begin(2, 5, 5, 1, 0));
    CHECK(alarm.active(60000));
    alarm.clear();
    CHECK(!alarm.active(60000));
  }
  {
    CustomTimerBuzzer profiles;
    Frame request{}, response{};
    CHECK(profiles.begin(CustomTimerBuzzer::Probe, 0, 0, 0, 0, request));
    CHECK(profiles.accept(reply(0, be16(request.bytes + 3), 0), 1));
    CHECK(
        profiles.begin_profile(CustomTimerBuzzer::ProbeProfiles, 0, 0, 0, 0, 0, false, 2, request));
    CHECK(request.size == 5 && request.bytes[1] == 3 && request.bytes[2] == 0);
    response = reply(0, be16(request.bytes + 3), 0);
    CHECK(!profiles.accept(response, 3));
    response.size = 8;
    response.bytes[1] = 3;
    response.bytes[6] = 0xF3;
    CHECK(profiles.accept(response, 3));
    CHECK(profiles.profiles_available() && profiles.timer_profiles_available());
    CHECK(profiles.begin_profile(CustomTimerBuzzer::ProfileBeep, 0, 5, 5, 1, 50, true, 4, request));
    CHECK(request.size == 19 && request.bytes[7] == 50 && request.bytes[9] == 5 &&
          request.bytes[11] == 5 && request.bytes[13] == 2 && request.bytes[18] == 100);
    profiles.observe_outgoing(request);
    CHECK(profiles.pending());
    response = reply(9, be16(request.bytes + 3), 0);
    response.size = 8;
    response.bytes[1] = 3;
    CHECK(profiles.accept(response, 5));
    CHECK(!profiles.begin_profile(CustomTimerBuzzer::ProfileTimer, 1, 5, 5, 1, 50, false, 6,
                                  request));
    CHECK(
        !profiles.begin_profile(CustomTimerBuzzer::ProfileBeep, 0, 5, 5, 1, 0, false, 6, request));
    CHECK(profiles.begin_profile(CustomTimerBuzzer::ProfileTimer, 720, 2000, 5000, 20, 0, false, 6,
                                 request));
    CHECK(request.size == 21 && be16(request.bytes + 19) == 720 && request.bytes[7] == 0);
    Frame interrupt{};
    interrupt.size = 1;
    interrupt.bytes[0] = 0xA1;
    profiles.observe_outgoing(interrupt);
    CHECK(!profiles.pending() && !profiles.profiles_available());
    CHECK(profiles.take_event() == CustomTimerBuzzer::Event::LinkLost);
  }
  {
    CustomTimerBuzzer legacy;
    Frame request{};
    CHECK(legacy.begin(CustomTimerBuzzer::Probe, 0, 0, 0, 0, request));
    CHECK(legacy.accept(reply(0, be16(request.bytes + 3), 0), 1));
    CHECK(legacy.begin_profile(CustomTimerBuzzer::ProbeProfiles, 0, 0, 0, 0, 0, false, 2, request));
    legacy.tick(3502);
    CHECK(legacy.available() && legacy.profiles_checked() && !legacy.profiles_available());
    CHECK(legacy.begin(CustomTimerBuzzer::Beep, 50, 50, 1, 3503, request));
  }
  {
    CustomAlarmDeferral alarm;
    CHECK(alarm.begin(2, 2000, 5000, 20, 0xFFFFFF00U));
    CHECK(!alarm.active(static_cast<uint32_t>(0xFFFFFF00U + 59999U)));
    CHECK(alarm.active(static_cast<uint32_t>(0xFFFFFF00U + 60000U)));
    CHECK(alarm.active(static_cast<uint32_t>(0xFFFFFF00U + 258499U)));
    CHECK(!alarm.active(static_cast<uint32_t>(0xFFFFFF00U + 258500U)));
    alarm.expire(static_cast<uint32_t>(0xFFFFFF00U + 258500U));
    CHECK(!alarm.active(static_cast<uint32_t>(0xFFFFFF00U + 60000U)));
    CHECK(!alarm.begin(1, 5, 5, 1, 0));
  }
  CustomTimerBuzzer t;
  Frame f{}, resp{};
  CHECK(CustomTimerBuzzer::valid(CustomTimerBuzzer::SetTimer, 2, 0, 0));
  CHECK(CustomTimerBuzzer::valid(CustomTimerBuzzer::SetTimer, 720, 0, 0));
  CHECK(!CustomTimerBuzzer::valid(CustomTimerBuzzer::SetTimer, 2, 1, 0));
  CHECK(!CustomTimerBuzzer::valid(CustomTimerBuzzer::SetTimer, 2, 0, 1));
  CHECK(CustomTimerBuzzer::valid(CustomTimerBuzzer::Beep, 50, 50, 1));
  CHECK(CustomTimerBuzzer::valid(CustomTimerBuzzer::Beep, 2000, 5000, 20));
  CHECK(!CustomTimerBuzzer::valid(CustomTimerBuzzer::Beep, 2001, 50, 1));
  CHECK(!CustomTimerBuzzer::valid(CustomTimerBuzzer::Beep, 50, 49, 1));
  CHECK(!CustomTimerBuzzer::valid(CustomTimerBuzzer::Beep, 50, 50, 0));
  CHECK(!CustomTimerBuzzer::valid(CustomTimerBuzzer::Probe, 1, 0, 0));
  CHECK(!CustomTimerBuzzer::valid(CustomTimerBuzzer::CancelTimer, 0, 0, 1));
  t.seed(0x3a);
  CHECK(!t.begin(CustomTimerBuzzer::SetTimer, 15, 0, 0, 0, f));
  CHECK(t.begin(CustomTimerBuzzer::Probe, 0, 0, 0, 0, f));
  CHECK(f.size == 10 && f.bytes[0] == 0xD7 && be16(f.bytes + 3) == 0x3b);
  CHECK(!t.begin(CustomTimerBuzzer::Probe, 0, 0, 0, 0, f));
  resp = reply(0, 0x3a, 0);
  CHECK(!t.accept(resp, 20));
  resp = reply(0, 0x3b, 4);
  CHECK(!t.accept(resp, 20));
  resp = reply(1, 0x3b, 0);
  CHECK(!t.accept(resp, 20));
  resp = reply(0, 0x3b, 0);
  resp.bytes[1] = 2;
  CHECK(!t.accept(resp, 20));
  resp.bytes[1] = 1;
  resp.size = 5;
  CHECK(!t.accept(resp, 20));
  resp.size = 6;
  CHECK(t.accept(resp, 20));
  CHECK(t.available());
  CHECK(t.take_event() == CustomTimerBuzzer::Event::Available);
  CHECK(!t.begin(CustomTimerBuzzer::SetTimer, 0, 0, 0, 22, f));
  CHECK(!t.begin(CustomTimerBuzzer::SetTimer, 1, 0, 0, 22, f));
  CHECK(!t.begin(CustomTimerBuzzer::SetTimer, 721, 0, 0, 22, f));
  CHECK(t.begin(CustomTimerBuzzer::SetTimer, 15, 0, 0, 22, f));
  CHECK(f.bytes[2] == 1 && f.bytes[5] == 0 && f.bytes[6] == 15 && f.bytes[9] == 0);
  resp = reply(1, be16(f.bytes + 3), 2);
  CHECK(t.accept(resp, 40));
  CHECK(t.take_event() == CustomTimerBuzzer::Event::Busy);
  CHECK(!t.begin(CustomTimerBuzzer::Beep, 49, 300, 3, 50, f));
  CHECK(!t.begin(CustomTimerBuzzer::Beep, 200, 5001, 3, 50, f));
  CHECK(!t.begin(CustomTimerBuzzer::Beep, 200, 300, 21, 50, f));
  CHECK(t.begin(CustomTimerBuzzer::Beep, 200, 300, 3, 50, f));
  CHECK(f.bytes[5] == 0 && f.bytes[6] == 200 && f.bytes[7] == 1 && f.bytes[8] == 44 &&
        f.bytes[9] == 3);
  uint16_t nonce = be16(f.bytes + 3);
  resp = reply(3, nonce ^ 0x0100, 0);
  CHECK(!t.accept(resp, 51));
  t.tick(3549);
  CHECK(t.pending());
  t.tick(3550);
  CHECK(!t.pending() && !t.available());
  CHECK(t.take_event() == CustomTimerBuzzer::Event::Timeout);
  resp = reply(3, nonce, 0);
  CHECK(!t.accept(resp, 3551));
  CHECK(!t.begin(CustomTimerBuzzer::CancelTimer, 0, 0, 0, 3551, f));
  // Escape/resync encoding preserves all reserved values in payload and nonce.
  t.seed(0xfaf9);
  CHECK(t.begin(CustomTimerBuzzer::Probe, 0, 0, 0, 3551, f));
  Packet p{};
  CHECK(encode_resync(f, p));
  CHECK(p.bytes[0] == 0xFA && p.bytes[1] == 0xFA);
  Parser parser;
  Frame decoded{};
  bool ready = false;
  for (size_t i = 0; i < p.size; ++i)
    if (parser.feed(p.bytes[i], i, decoded) == ParseResult::FrameReady)
      ready = true;
  CHECK(ready && decoded.size == f.size && be16(decoded.bytes + 3) == 0xFAFA);
  t.observe_outgoing(f);
  CHECK(t.pending());  // Its own D7 is allowed.
  Frame stop = desk_protocol::stop();
  t.observe_outgoing(stop);  // Stop is never blocked by a pending request.
  CHECK(!t.pending() && !t.available());
  CHECK(t.take_event() == CustomTimerBuzzer::Event::LinkLost);
  resp = reply(0, be16(f.bytes + 3), 0);
  CHECK(!t.accept(resp, 3552));  // late ACK
  // Accepted/invalid/busy/backend responses each complete and emit one event.
  CHECK(t.begin(CustomTimerBuzzer::Probe, 0, 0, 0, 3600, f));
  resp = reply(0, be16(f.bytes + 3), 0);
  CHECK(t.accept(resp, 3601));
  t.take_event();
  for (uint8_t result = 0; result <= 3; ++result) {
    CHECK(t.begin(CustomTimerBuzzer::CancelTimer, 0, 0, 0, 3700 + result, f));
    resp = reply(2, be16(f.bytes + 3), result);
    CHECK(t.accept(resp, 3700 + result));
    CHECK(!t.pending());
    const auto expected = result == 0   ? CustomTimerBuzzer::Event::Accepted
                          : result == 1 ? CustomTimerBuzzer::Event::Invalid
                          : result == 2 ? CustomTimerBuzzer::Event::Busy
                                        : CustomTimerBuzzer::Event::BackendError;
    CHECK(t.take_event() == expected && t.take_event() == CustomTimerBuzzer::Event::None);
  }
  // A failed read-only probe clears a previously advertised capability.
  for (uint8_t result = 1; result <= 3; ++result) {
    CHECK(t.begin(CustomTimerBuzzer::Probe, 0, 0, 0, 3800, f));
    resp = reply(0, be16(f.bytes + 3), 0);
    CHECK(t.accept(resp, 3801) && t.available());
    t.take_event();
    CHECK(t.begin(CustomTimerBuzzer::Probe, 0, 0, 0, 3802, f));
    resp = reply(0, be16(f.bytes + 3), result);
    CHECK(t.accept(resp, 3803) && !t.available() && !t.pending());
    t.take_event();
  }
  // Even another D7 must match both operation and nonce to remain our request.
  for (uint8_t mismatch = 0; mismatch < 2; ++mismatch) {
    CHECK(t.begin(CustomTimerBuzzer::Probe, 0, 0, 0, 3900, f));
    Frame competing = f;
    if (mismatch == 0)
      competing.bytes[2] = CustomTimerBuzzer::CancelTimer;
    else
      competing.bytes[4] ^= 1;
    t.observe_outgoing(competing);
    CHECK(!t.pending() && !t.available());
    CHECK(t.take_event() == CustomTimerBuzzer::Event::LinkLost);
  }
  // uint32 clock wrap is handled by unsigned elapsed arithmetic.
  CHECK(t.begin(CustomTimerBuzzer::Probe, 0, 0, 0, 0xfffffff0U, f));
  t.tick(0x00000d9bU);
  CHECK(t.pending());
  t.tick(0x00000d9cU);
  CHECK(!t.pending());
}
