#ifndef DESK_BUZZER_PROFILE_H
#define DESK_BUZZER_PROFILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
  BUZZER_FREQUENCY_HZ = 20000,
  BUZZER_PERIOD = 2400,
  BUZZER_PLAY = 9,
  BUZZER_TIMER = 10,
  BUZZER_PLAY_SIZE = 19,
  BUZZER_TIMER_SIZE = 21
};

typedef struct {
  _Alignas(4) uint16_t on_ms;
  uint16_t off_ms;
  uint8_t count, volume, soften, reserved;
} BuzzerProfile;

bool buzzer_profile_valid(const BuzzerProfile* profile, bool allow_silent);
/* Failed decoding leaves the destination and minutes untouched. */
bool buzzer_profile_decode(const uint8_t* body, size_t size, BuzzerProfile* profile,
                           uint16_t* minutes);
uint32_t buzzer_profile_duration(const BuzzerProfile* profile);
uint16_t buzzer_profile_compare(uint8_t volume, bool soften, uint16_t on_ms, uint16_t elapsed_ms);

#endif
