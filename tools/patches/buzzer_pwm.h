#ifndef DESK_BUZZER_PWM_H
#define DESK_BUZZER_PWM_H
#include <stdbool.h>
#include <stdint.h>

bool buzzer_pwm_owned(void);
bool buzzer_pwm_available(void);
bool buzzer_pwm_running(void);
bool buzzer_pwm_begin(uint8_t volume, uint16_t compare);
/* Resume never converts a lost PWM claim into full-volume GPIO output. */
bool buzzer_pwm_resume(bool direct, uint16_t compare);
void buzzer_pwm_pause(void);
void buzzer_pwm_release(void);
void buzzer_pwm_compare(uint16_t compare);
void buzzer_platform_release(void);
#endif
