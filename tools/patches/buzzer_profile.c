#include "buzzer_profile.h"

static uint16_t be16(const uint8_t* bytes) {
  return (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
}

bool buzzer_profile_valid(const BuzzerProfile* profile, bool allow_silent) {
  return profile->on_ms >= 5 && profile->on_ms <= 2000 && profile->off_ms >= 5 &&
         profile->off_ms <= 5000 && profile->count >= 1 && profile->count <= 20 &&
         profile->volume <= 100 && profile->volume % 5 == 0 &&
         (allow_silent || profile->volume != 0) && profile->soften <= 1 && !profile->reserved;
}

bool buzzer_profile_decode(const uint8_t* body, size_t size, BuzzerProfile* profile,
                           uint16_t* minutes) {
  if (size != BUZZER_PLAY_SIZE && size != BUZZER_TIMER_SIZE) {
    return false;
  }
  const bool timer = body[2] == BUZZER_TIMER;
  if (body[0] != 0xd7 || body[1] != 3 ||
      (timer ? size != BUZZER_TIMER_SIZE : body[2] != BUZZER_PLAY || size != BUZZER_PLAY_SIZE) ||
      be16(body + 5) != BUZZER_FREQUENCY_HZ) {
    return false;
  }
  const uint8_t soften = body[13] == 2;
  static const uint8_t envelope[6] = {2, 5, 20, 20, 80, 100};
  for (size_t i = 0; i < sizeof(envelope); ++i) {
    if (body[13 + i] != (soften ? envelope[i] : 0)) {
      return false;
    }
  }
  const BuzzerProfile decoded = {be16(body + 8), be16(body + 10), body[12], body[7], soften, 0};
  const uint16_t duration = timer ? be16(body + 19) : 0;
  if (!buzzer_profile_valid(&decoded, timer) || (timer && (duration < 2 || duration > 720))) {
    return false;
  }
  *profile = decoded;
  *minutes = duration;
  return true;
}

uint32_t buzzer_profile_duration(const BuzzerProfile* profile) {
  if (!buzzer_profile_valid(profile, true)) {
    return 0;
  }
  return (uint32_t)profile->count * profile->on_ms +
         (uint32_t)(profile->count - 1) * profile->off_ms;
}

static uint16_t ramp_ms(uint16_t duration, uint16_t cap) {
  uint16_t ramp = duration / 5;
  if (ramp > cap) {
    ramp = cap;
  }
  return ramp >= 3 ? ramp : 0;
}

uint16_t buzzer_profile_compare(uint8_t volume, bool soften, uint16_t on_ms, uint16_t elapsed_ms) {
  if (volume > 100 || volume % 5 != 0) {
    return 0;
  }
  const uint16_t peak = (BUZZER_PERIOD * volume) / 100;
  const uint16_t floor = BUZZER_PERIOD / 20;
  if (!soften || volume == 100 || peak <= floor) {
    return peak;
  }
  const uint16_t attack = ramp_ms(on_ms, 80);
  const uint16_t release = ramp_ms(on_ms, 100);
  uint32_t level = 256;
  if (attack && elapsed_ms < attack) {
    level = (uint32_t)elapsed_ms * 256 / attack;
  } else if (release) {
    const uint16_t remaining = elapsed_ms < on_ms ? on_ms - elapsed_ms : 0;
    if (remaining < release) {
      level = (uint32_t)remaining * 256 / release;
    }
  }
  /* Same Q8 smoothstep as the reviewed PWM prototype. */
  level = (level * level * (768 - 2 * level) + 32768) >> 16;
  return (uint16_t)(floor + (((peak - floor) * level + 128) >> 8));
}
