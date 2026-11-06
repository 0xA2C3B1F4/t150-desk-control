#include "../components/desk_provisioning/credentials.h"
#include <cstdlib>
#include <iostream>
using namespace esphome::desk_provisioning;
static unsigned checks = 0;
#define CHECK(x)                             \
  do {                                       \
    ++checks;                                \
    if (!(x)) {                              \
      std::cerr << __LINE__ << ": " #x "\n"; \
      std::exit(1);                          \
    }                                        \
  } while (false)
struct Store : Backend {
  Record stored{};
  ReadResult first{ReadResult::MISSING}, after{ReadResult::FOUND};
  bool random_ok{true}, save_ok{true}, alter{false};
  unsigned reads{0}, writes{0}, randoms{0};
  uint8_t byte{17};
  ReadResult read(Record& out) override {
    auto result = reads++ == 0 ? first : after;
    out = stored;
    if (reads > 1 && alter) {
      out[10] ^= 1;
    }
    return result;
  }
  bool save(const Record& r) override {
    ++writes;
    stored = r;
    return save_ok;
  }
  bool random(uint8_t* out, size_t size) override {
    ++randoms;
    std::fill_n(out, size, byte);
    return random_ok;
  }
};
int main() {
  CHECK(checksum(reinterpret_cast<const uint8_t*>("123456789"), 9) == 0xCBF43926u);
  Store fresh;
  Record record{};
  CHECK(initialize(fresh, record) == InitResult::CREATED);
  CHECK(fresh.reads == 2 && fresh.writes == 1 && fresh.randoms == 1 && valid(record));
  const Record original = record;
  Store reboot;
  reboot.stored = record;
  reboot.first = ReadResult::FOUND;
  CHECK(initialize(reboot, record) == InitResult::LOADED && record == original);
  CHECK(reboot.writes == 0 && reboot.randoms == 0);
  // Every bit flip must be detected; corrupt existing records must never be
  // regenerated.
  for (size_t i = 0; i < original.size(); ++i) {
    for (unsigned bit = 0; bit < 8; ++bit) {
      Store bad;
      bad.stored = original;
      bad.stored[i] ^= 1u << bit;
      bad.first = ReadResult::FOUND;
      CHECK(initialize(bad, record) == InitResult::FAILED && bad.writes == 0 && bad.randoms == 0);
    }
  }
  for (auto first : {ReadResult::ERROR}) {
    Store bad;
    bad.first = first;
    CHECK(initialize(bad, record) == InitResult::FAILED && bad.randoms == 0);
  }
  for (uint8_t byte : {uint8_t(0), uint8_t(255)}) {
    Store bad;
    bad.byte = byte;
    CHECK(initialize(bad, record) == InitResult::FAILED && bad.writes == 0);
    for (size_t offset : {size_t(8), size_t(40)}) {
      bad = Store{};
      bad.stored = original;
      std::fill_n(bad.stored.begin() + offset, offset == 8 ? 32 : 16, byte);
      seal(bad.stored);
      bad.first = ReadResult::FOUND;
      CHECK(initialize(bad, record) == InitResult::FAILED && bad.randoms == 0);
    }
  }
  Store rng_fail;
  rng_fail.random_ok = false;
  CHECK(initialize(rng_fail, record) == InitResult::FAILED && rng_fail.writes == 0);
  Store save_fail;
  save_fail.save_ok = false;
  CHECK(initialize(save_fail, record) == InitResult::FAILED && save_fail.reads == 1);
  for (auto after : {ReadResult::ERROR, ReadResult::MISSING}) {
    Store bad;
    bad.after = after;
    CHECK(initialize(bad, record) == InitResult::FAILED);
  }
  Store changed;
  changed.alter = true;
  CHECK(initialize(changed, record) == InitResult::FAILED);
  // Lost-before-write is a new first boot. Persisted-before-readback loads the
  // same bytes.
  Store lost;
  CHECK(initialize(lost, record) == InitResult::CREATED);
  Store persisted;
  persisted.stored = save_fail.stored;
  persisted.first = ReadResult::FOUND;
  CHECK(initialize(persisted, record) == InitResult::LOADED && record == save_fail.stored);
  HoldButton button;
  for (uint32_t t = 0; t < 5000; t += 20) {
    CHECK(!button.update(true, t));
  }
  CHECK(!button.update(false, 5000));
  CHECK(!button.update(false, 5060));
  CHECK(!button.update(true, 5100));
  CHECK(!button.update(true, 5160));
  CHECK(!button.update(true, 7140));
  CHECK(button.update(true, 7160));
  CHECK(!button.update(true, 20000));
  CHECK(!button.update(false, 20100));
  CHECK(!button.update(true, 20120));
  CHECK(!button.update(true, 23000));
  CHECK(!button.update(false, 23100));
  CHECK(!button.update(false, 23160));
  CHECK(!button.update(true, 23200));
  CHECK(!button.update(true, 23260));
  CHECK(button.update(true, 25260));
  HoldButton bounce;
  CHECK(!bounce.update(false, 0));
  CHECK(!bounce.update(false, 60));
  CHECK(!bounce.update(true, 100));
  CHECK(!bounce.update(true, 160));
  CHECK(!bounce.update(false, 1000));
  CHECK(!bounce.update(true, 1020));
  CHECK(bounce.update(true, 2160));
  HoldButton wrap;
  CHECK(!wrap.update(false, 0xFFFFFF00u));
  CHECK(!wrap.update(false, 0xFFFFFF40u));
  CHECK(!wrap.update(true, 0xFFFFFF80u));
  CHECK(!wrap.update(true, 0xFFFFFFC0u));
  CHECK(wrap.update(true, 0x00000790u));
  std::cout << checks << " provisioning assertions passed\n";
}
