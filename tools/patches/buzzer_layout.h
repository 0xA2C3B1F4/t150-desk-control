#ifndef DESK_BUZZER_LAYOUT_H
#define DESK_BUZZER_LAYOUT_H

#include "buzzer_profile.h"
#include "platform_layout.h"

enum { BUZZER_ALARM_NONE, BUZZER_ALARM_ARMED, BUZZER_ALARM_PLAYING, BUZZER_ALARM_CONSUMED };
enum { BUZZER_OUTPUT_NONE, BUZZER_OUTPUT_DIRECT, BUZZER_OUTPUT_PWM };

typedef struct {
  BuzzerProfile profile;
  uint32_t rtc_alarm;
  uint8_t state, reserved[3];
} BuzzerAlarm;

typedef struct {
  uint8_t volume, soften, output, reserved;
} BuzzerVolume;

/* ABI 1 reserved this space for future modules; no existing slot moves. */
#define BUZZER_ALARM_ADDRESS PLATFORM_ABI_RESERVE_ADDRESS
#define BUZZER_ALARM_SIZE 16u
_Static_assert(sizeof(BuzzerProfile) == 8, "profile layout");
_Static_assert(sizeof(BuzzerAlarm) == BUZZER_ALARM_SIZE, "alarm layout");
_Static_assert(BUZZER_ALARM_SIZE <= PLATFORM_ABI_RESERVE_SIZE, "alarm reservation");
_Static_assert(BUZZER_ALARM_ADDRESS % 4 == 0, "alarm alignment");
_Static_assert(sizeof(BuzzerVolume) <= PLATFORM_VOLUME_SIZE, "volume reservation");

#endif
