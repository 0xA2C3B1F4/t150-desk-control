#include "desk_protocol/child_lock.h"
#include <cstdlib>
#include <iostream>
using namespace desk_protocol;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while (false)

static Frame reply(const Frame &request, bool locked = true, uint8_t status = 0,
                   bool gate = true, bool fault = false, uint16_t sequence = 1) {
  Frame f;
  f.size = 15; f.bytes[0] = 0xD4; f.bytes[1] = 1; f.bytes[2] = request.bytes[2]; f.bytes[3] = status;
  f.bytes[4] = locked; f.bytes[5] = gate; f.bytes[6] = fault;
  std::memcpy(f.bytes + 7, "CL01", 4);
  f.bytes[11] = request.bytes[3]; f.bytes[12] = request.bytes[4];
  f.bytes[13] = static_cast<uint8_t>(sequence >> 8); f.bytes[14] = static_cast<uint8_t>(sequence);
  return f;
}
static Frame discover(ChildLock &lock, uint32_t now = 0, uint16_t sequence = 1,
                      bool fault = false) {
  Frame f;
  CHECK(lock.probe(now)); CHECK(lock.take_frame(f)); CHECK(f.size == 5);
  CHECK(f.bytes[0] == 0xD3 && f.bytes[1] == 1 && f.bytes[2] == 0);
  lock.accept(reply(f, true, 0, true, fault, sequence), now + 1);
  CHECK(lock.supported() && !lock.busy());
  CHECK(lock.state_known() == !fault);
  CHECK(lock.take_completed() && !lock.take_completed());
  return f;
}
int main() {
  Frame f;
  { ChildLock lock; CHECK(!lock.set(true, 0)); CHECK(!lock.take_frame(f));
    lock.seed(0xFFFF); discover(lock); CHECK(lock.next_nonce() == 0);
    CHECK(lock.set(false, 2)); CHECK(!lock.state_known() && !lock.set(true, 2));
    CHECK(lock.take_frame(f)); CHECK(f.bytes[2] == 2 && be16(f.bytes + 3) == 0);
    lock.accept(reply(f, false, 0, true, false, 2), 3);
    CHECK(lock.state_known() && !lock.reply().locked && lock.reply().physical_gate);
    CHECK(lock.set(true, 4)); CHECK(lock.take_frame(f));
    lock.accept(reply(f, true, 0, true, false, 3), 5);
    CHECK(lock.state_known() && lock.reply().locked && !lock.take_frame(f)); }
  // SET is legal after a cached GET reports a storage fault, even to the same
  // effective locked state. Only the successful SET proves persistence again.
  { ChildLock lock; discover(lock, 0, 1, true);
    CHECK(lock.reply().locked && lock.reply().storage_fault && !lock.state_known());
    CHECK(lock.set(true, 2)); CHECK(lock.take_frame(f));
    lock.accept(reply(f, true, 3, true, true, 2), 3);
    CHECK(lock.supported() && !lock.state_known() && !lock.busy() && lock.error());
    lock.tick(900, true); CHECK(!lock.take_frame(f));
    CHECK(lock.set(true, 901)); CHECK(lock.take_frame(f));
    lock.accept(reply(f, true, 0, true, false, 3), 902); CHECK(lock.state_known()); }
  { ChildLock lock; discover(lock); CHECK(lock.set(false, 2)); CHECK(lock.take_frame(f));
    lock.accept(reply(f, true, 2, true, false, 2), 3);
    CHECK(lock.state_known() && lock.reply().locked && lock.error()); CHECK(!lock.take_frame(f)); }
  // Magic, version, bools, status and successful SET semantics are strict.
  for (unsigned offset : {1U, 3U, 4U, 5U, 6U, 7U, 10U}) {
    ChildLock lock; CHECK(lock.probe(0)); CHECK(lock.take_frame(f));
    Frame bad = reply(f); bad.bytes[offset] = 0xFF;
    lock.accept(bad, 1); CHECK(!lock.supported() && !lock.state_known() && !lock.busy()); }
  for (unsigned offset : {2U, 11U, 12U}) {
    ChildLock lock; CHECK(lock.probe(0)); CHECK(lock.take_frame(f));
    Frame stale = reply(f); stale.bytes[offset] ^= 1;
    lock.accept(stale, 1); CHECK(lock.busy() && !lock.supported());
    lock.tick(800, true); CHECK(!lock.busy() && !lock.supported() && !lock.take_frame(f)); }
  { ChildLock lock; CHECK(lock.probe(0)); CHECK(lock.take_frame(f));
    Frame malformed = reply(f); --malformed.size; lock.accept(malformed, 1);
    CHECK(lock.busy() && !lock.supported()); lock.tick(800, true); CHECK(!lock.busy()); }
  { ChildLock lock; discover(lock); CHECK(lock.set(false, 2)); CHECK(lock.take_frame(f));
    lock.accept(reply(f, true, 0, true, false, 2), 3); CHECK(!lock.supported() && !lock.state_known()); }
  { ChildLock lock; discover(lock); CHECK(lock.set(true, 2)); CHECK(lock.take_frame(f));
    lock.accept(reply(f, true, 0, true, true, 2), 3); CHECK(!lock.supported() && !lock.state_known()); }
  { ChildLock lock; discover(lock); CHECK(lock.probe(2)); CHECK(lock.take_frame(f));
    lock.accept(reply(f, true, 0, true, false, 1), 3); CHECK(!lock.supported() && !lock.state_known()); }
  { ChildLock lock; discover(lock, 0, 0xFFFF); CHECK(lock.probe(2)); CHECK(lock.take_frame(f));
    lock.accept(reply(f, false, 0, false, false, 0), 3); CHECK(lock.state_known()); }
  // An unbound generic DT02 error never establishes CL01 support.
  { ChildLock lock; CHECK(lock.probe(0)); CHECK(lock.take_frame(f)); Frame unsupported;
    unsupported.size = 4; unsupported.bytes[0] = 0xEE; unsupported.bytes[1] = 3;
    lock.accept(unsupported, 1); lock.tick(800, true); CHECK(!lock.supported() && !lock.state_known()); }
  { ChildLock lock; discover(lock); CHECK(lock.set(false, 2)); CHECK(lock.take_frame(f));
    lock.accept(reply(f, false, 0, false, false, 2), 802);
    CHECK(!lock.supported() && !lock.state_known() && !lock.take_frame(f)); }
  { ChildLock lock; CHECK(lock.probe(0xFFFFFF00U)); CHECK(lock.take_frame(f));
    lock.tick(0x220, true); CHECK(!lock.busy() && !lock.supported()); }
  { ChildLock lock; discover(lock); CHECK(lock.set(false, 2)); CHECK(lock.take_frame(f));
    auto delayed = reply(f, false, 0, false, false, 2);
    lock.disconnect(); lock.accept(delayed, 3); CHECK(!lock.state_known() && !lock.supported());
    discover(lock, 4); lock.tick(6, false); CHECK(!lock.state_known() && !lock.supported()); }
  // A reply before the request leaves the queue cannot bind to that request.
  { ChildLock lock; CHECK(lock.probe(0)); Frame guessed; guessed.size = 5;
    lock.accept(reply(guessed), 1); CHECK(lock.busy() && !lock.supported()); }
  // Fresh shared telemetry, rather than the previous D4, binds a reply after
  // a long idle interval spanning more than half the controller's counter.
  { ChildLock lock; discover(lock); lock.observe_sequence(0x9000);
    CHECK(lock.probe(2)); CHECK(lock.take_frame(f));
    lock.accept(reply(f, true, 0, true, false, 0x9001), 3); CHECK(lock.state_known()); }
  { ChildLock lock; discover(lock); lock.observe_sequence(0x9000);
    CHECK(lock.probe(2)); CHECK(lock.take_frame(f));
    lock.accept(reply(f, true, 0, true, false, 0x8FFF), 3); CHECK(!lock.state_known()); }
  // Timeouts produce one failed SET result; a later fresh GET restores support
  // before another explicit SET. Neither the timeout nor GET queues a SET.
  { ChildLock lock; discover(lock); CHECK(lock.set(false, 2)); CHECK(lock.take_frame(f));
    const Frame old = f; lock.tick(802, true); CHECK(lock.take_failure()); CHECK(!lock.take_failure());
    CHECK(!lock.set(false, 803) && !lock.take_frame(f));
    CHECK(lock.probe(1602)); CHECK(lock.take_frame(f)); CHECK(f.bytes[2] == 0);
    CHECK(be16(f.bytes + 3) != be16(old.bytes + 3));
    lock.accept(reply(old, false, 0, false, false, 2), 1603); CHECK(!lock.supported());
    lock.accept(reply(f, false, 0, false, false, 3), 1604); CHECK(lock.supported());
    CHECK(lock.set(false, 1605)); CHECK(lock.take_frame(f)); CHECK(f.bytes[2] == 2); }
  { ChildLock lock; discover(lock); CHECK(lock.set(false, 2)); CHECK(lock.take_frame(f));
    lock.disconnect("Interrupted"); CHECK(lock.take_failure() && !lock.take_failure()); }
  { ChildLock lock; discover(lock); CHECK(lock.set(false, 2)); CHECK(lock.take_frame(f));
    lock.disconnect("Stop", false); CHECK(!lock.take_failure()); }
  std::cout << "CL01 binding, discovery, storage uncertainty, single attempts, loss and wrap passed\n";
}
