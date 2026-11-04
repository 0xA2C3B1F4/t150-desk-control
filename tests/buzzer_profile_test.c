#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "buzzer_profile.h"

static void packet(uint8_t* bytes, uint8_t operation) {
  const uint8_t flat[21] = {0xd7, 3, operation, 0xfa, 0xfd, 0x4e, 0x20, 50, 0, 200, 1,
                            44,   3, 0,         0,    0,    0,    0,    0,  0, 2};
  memcpy(bytes, flat, sizeof(flat));
}

int main(void) {
  uint8_t bytes[22];
  BuzzerProfile profile = {0};
  uint16_t minutes = 99;
  packet(bytes, BUZZER_PLAY);
  assert(buzzer_profile_decode(bytes, 19, &profile, &minutes));
  assert(profile.volume == 50 && !profile.soften && minutes == 0);
  assert(buzzer_profile_duration(&profile) == 1200);
  for (unsigned size = 0; size <= 22; ++size) {
    if (size != 19) {
      assert(!buzzer_profile_decode(bytes, size, &profile, &minutes));
    }
  }
  for (unsigned volume = 0; volume <= 255; ++volume) {
    bytes[7] = (uint8_t)volume;
    assert(buzzer_profile_decode(bytes, 19, &profile, &minutes) == (volume >= 1 && volume <= 100));
  }
  packet(bytes, BUZZER_TIMER);
  bytes[7] = 0;
  assert(buzzer_profile_decode(bytes, 21, &profile, &minutes) && minutes == 2);
  assert(!profile.volume);
  for (unsigned duration = 0; duration <= 721; ++duration) {
    bytes[19] = (uint8_t)(duration >> 8);
    bytes[20] = (uint8_t)duration;
    assert(buzzer_profile_decode(bytes, 21, &profile, &minutes) ==
           (duration >= 2 && duration <= 720));
  }
  packet(bytes, BUZZER_PLAY);
  bytes[8] = bytes[10] = 0;
  bytes[9] = bytes[11] = 5;
  assert(buzzer_profile_decode(bytes, 19, &profile, &minutes));
  assert(buzzer_profile_duration(&profile) == 25);
  bytes[9] = 4;
  BuzzerProfile saved = profile;
  minutes = 55;
  assert(!buzzer_profile_decode(bytes, 19, &profile, &minutes));
  assert(memcmp(&saved, &profile, sizeof(saved)) == 0 && minutes == 55);
  packet(bytes, BUZZER_PLAY);
  const uint8_t soft[6] = {2, 5, 20, 20, 80, 100};
  memcpy(bytes + 13, soft, sizeof(soft));
  assert(buzzer_profile_decode(bytes, 19, &profile, &minutes) && profile.soften);
  for (unsigned i = 13; i < 19; ++i) {
    bytes[i] ^= 1;
    assert(!buzzer_profile_decode(bytes, 19, &profile, &minutes));
    bytes[i] ^= 1;
  }
  for (unsigned volume = 0; volume <= 100; ++volume) {
    for (unsigned on_ms = 5; on_ms <= 2000; ++on_ms) {
      for (unsigned elapsed = 0; elapsed <= on_ms; ++elapsed) {
        const unsigned peak = 24 * volume;
        const unsigned level = buzzer_profile_compare(volume, true, on_ms, elapsed);
        assert(level <= peak);
        assert(level >= (volume >= 5 ? 120u : peak));
        if (volume == 100 || on_ms < 15 || volume <= 5) {
          assert(level == peak);
        }
      }
    }
  }
  assert(buzzer_profile_compare(50, true, 2000, 0) == 120);
  assert(buzzer_profile_compare(50, true, 2000, 80) == 1200);
  assert(buzzer_profile_compare(50, true, 2000, 1900) == 1200);
  assert(buzzer_profile_compare(50, true, 2000, 2000) == 120);
  return 0;
}
