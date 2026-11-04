#include "buzzer_layout.h"
#include "buzzer_pwm.h"
#include "platform_runtime.h"
#include "motion_trim_controller.h"
#define ALARM ((BuzzerAlarm*)BUZZER_ALARM_ADDRESS)
#define VOLUME ((BuzzerVolume*)PLATFORM_VOLUME_ADDRESS)
#define TB01_STATE_ADDRESS PLATFORM_BUZZER_ADDRESS
#define TB01_READY() platform_ready()
#define TB01_ALARM_BLOCKED() (B(0x8a8) && ALARM->state != BUZZER_ALARM_PLAYING)
#define TB01_RTC_HELPERS_ONLY
#include "tb01.c"

static bool owned_alarm(void) {
  return ALARM->state == BUZZER_ALARM_PLAYING || ALARM->state == BUZZER_ALARM_CONSUMED;
}

static void finish_sound(void) {
  const bool alarm = owned_alarm();
  if (alarm) {
    ALARM->state = BUZZER_ALARM_CONSUMED;
  }
  buzzer_pwm_release();
  if (VOLUME->output || alarm) {
    PF_OFF = 0x80;
  }
  VOLUME->output = BUZZER_OUTPUT_NONE;
  tb_init(STATE);
  if (alarm) {
    B(0x14d) = 0;
    B(0x8a8) = 0;
    W(0x8b4) = 0;
    PB_OFF = 4;
    ALARM->state = BUZZER_ALARM_NONE;
  }
}

void buzzer_platform_release(void) {
  /* Called with IRQs masked, even after a failed header/guard check. */
  const bool hardware_owned = buzzer_pwm_owned();
  const bool alarm = B(0x8a8) && (owned_alarm() || hardware_owned);
  buzzer_pwm_release();
  if (hardware_owned || alarm || (!B(0x8a8) && !B(0x14d))) {
    PF_OFF = 0x80;
  }
  if (alarm) {
    B(0x14d) = 0;
    B(0x8a8) = 0;
    W(0x8b4) = 0;
    PB_OFF = 4;
  }
  ALARM->state = BUZZER_ALARM_NONE;
  VOLUME->output = BUZZER_OUTPUT_NONE;
  tb_init(STATE);
}

static bool sound_ready(void* context) {
  if (!ready(context) || trim_controller_blocks_native()) {
    return false;
  }
  if (owned_alarm() && !B(0x8a8)) {
    return false;
  }
  if (STATE->phase && VOLUME->output == BUZZER_OUTPUT_PWM) {
    return buzzer_pwm_owned() && (STATE->phase != 1 || buzzer_pwm_running());
  }
  return !STATE->phase || VOLUME->output == BUZZER_OUTPUT_DIRECT;
}

static bool legacy_start(void* context) {
  (void)context;
  if (!buzzer_pwm_begin(100, 0)) {
    return false;
  }
  VOLUME->volume = 100;
  VOLUME->soften = 0;
  VOLUME->output = BUZZER_OUTPUT_DIRECT;
  return true;
}

static void sound_off(void* context) {
  (void)context;
  buzzer_pwm_pause();
}

static void sound_on(void* context) {
  (void)context;
  const uint16_t compare = buzzer_profile_compare(VOLUME->volume, VOLUME->soften, STATE->on_ms, 0);
  if (!buzzer_pwm_resume(VOLUME->output == BUZZER_OUTPUT_DIRECT, compare)) {
    /* tb_tick completes the phase update; the next readiness check aborts it. */
    VOLUME->output = BUZZER_OUTPUT_NONE;
    buzzer_pwm_pause();
  }
}

static void sound_release(void* context) {
  (void)context;
  finish_sound();
}

static bool cancel_timer(void* context) {
  if (owned_alarm()) {
    uint32_t saved = lock();
    finish_sound();
    unlock(saved);
    return true;
  }
  if (!timer_cancel(context)) {
    return false;
  }
  uint32_t saved = lock();
  ALARM->state = BUZZER_ALARM_NONE;
  unlock(saved);
  return true;
}

static const tb_platform sound_platform = {sound_ready, timer_set, cancel_timer, legacy_start,
                                           sound_off,   sound_on,  sound_release};

static bool start_profile(const BuzzerProfile* profile, uint32_t now) {
  if (!buzzer_profile_valid(profile, false) || !sound_ready(0)) {
    return false;
  }
  uint16_t compare = buzzer_profile_compare(profile->volume, profile->soften, profile->on_ms, 0);
  if (!buzzer_pwm_begin(profile->volume, compare)) {
    return false;
  }
  VOLUME->volume = profile->volume;
  VOLUME->soften = profile->soften;
  VOLUME->output = profile->volume == 100 ? BUZZER_OUTPUT_DIRECT : BUZZER_OUTPUT_PWM;
  STATE->on_ms = profile->on_ms;
  STATE->pause_ms = profile->off_ms;
  STATE->left = profile->count;
  STATE->deadline = now + profile->on_ms;
  STATE->phase = 1;
  return true;
}

void tb01_abort(void) {
  uint32_t saved = lock();
  if (platform_ready()) {
    finish_sound();
  }
  unlock(saved);
}

void buzzer_timer_replaced(void) {
  uint32_t saved = lock();
  if (platform_ready()) {
    if (owned_alarm()) {
      finish_sound();
    }
    ALARM->state = BUZZER_ALARM_NONE;
  }
  unlock(saved);
}

bool tb01_native_priority(uint32_t mode, uint32_t count) {
  uint32_t saved = lock();
  if (!platform_profile_matches()) {
    unlock(saved);
    return false;
  }
  const bool was_owned = owned_alarm() || buzzer_pwm_owned();
  bool handled = false;
  if (!platform_ready()) {
    handled = mode == 0xff && was_owned;
  } else if (mode == 0xff) {
    if (ALARM->state == BUZZER_ALARM_ARMED && B(0x8a8) && ALARM->rtc_alarm == RTC_ALARM) {
      finish_sound();
      ALARM->state = BUZZER_ALARM_PLAYING;
      if (!ALARM->profile.volume || !start_profile(&ALARM->profile, W(0x75c))) {
        finish_sound();
      }
      handled = true;
    } else if (owned_alarm()) {
      handled = true;
    } else {
      ALARM->state = BUZZER_ALARM_NONE;
      finish_sound();
    }
  } else if (mode == 0xaa && B(0x14e) < count) {
    finish_sound();
  }
  unlock(saved);
  return handled;
}

void tb01_init(void) {
  uint32_t saved = lock();
  if (platform_initialized()) {
    tb_init(STATE);
    VOLUME->output = BUZZER_OUTPUT_NONE;
    ALARM->state = BUZZER_ALARM_NONE;
    platform_modules_ready();
  }
  unlock(saved);
  FN(0x080094a1u, void (*)(void))();
}

void tb01_watchdog(void) {
  FN(0x080094c1u, void (*)(void))();
  uint32_t saved = lock();
  if (platform_ready()) {
    if (ALARM->state == BUZZER_ALARM_ARMED && !(RTC_CR & 0x1100u) && !B(0x8a8) &&
        !(RTC_ISR & 0x100u) && !(EXTI_PR & (1u << 17))) {
      ALARM->state = BUZZER_ALARM_NONE;
    }
    const uint32_t now = W(0x75c);
    tb_tick(STATE, &sound_platform, 0, now);
    if (STATE->phase == 1 && VOLUME->output == BUZZER_OUTPUT_PWM) {
      const uint32_t remaining = STATE->deadline - now;
      if (remaining <= STATE->on_ms) {
        buzzer_pwm_compare(buzzer_profile_compare(VOLUME->volume, VOLUME->soften, STATE->on_ms,
                                                  STATE->on_ms - remaining));
      }
    }
  }
  unlock(saved);
}

void tb01_sleep(void) {
  tb01_abort();
  FN(0x08007569u, void (*)(void))();
}

static uint8_t status_bits(void) {
  return (STATE->phase ? 1 : 0) | ((RTC_CR & 0x1100u) ? 4 : 0) |
         ((B(0x8a8) || (RTC_ISR & 0x100u) || (EXTI_PR & (1u << 17))) ? 8 : 0) |
         (!sound_ready(0) ? 16 : 0) | (B(0x18c) ? 32 : 0);
}

static uint8_t profile_request(const uint8_t* request, uint8_t size) {
  const uint8_t operation = request[2];
  if (size == 5 && operation == 2) {
    uint8_t cancel = 2;
    uint32_t saved = lock();
    const bool allowed = owned_alarm() || (ready(&cancel) && !trim_controller_blocks_native());
    unlock(saved);
    if (!allowed) {
      return TB_BUSY;
    }
    return cancel_timer(0) ? TB_OK : TB_BACKEND;
  }
  uint32_t saved = lock();
  if (size == 5 && (operation == 0 || operation == 4)) {
    if (operation == 4) {
      finish_sound();
    }
    unlock(saved);
    return TB_OK;
  }
  BuzzerProfile profile;
  uint16_t minutes;
  if (!buzzer_profile_decode(request, size, &profile, &minutes)) {
    unlock(saved);
    return TB_BAD;
  }
  if (STATE->phase || !sound_ready(0) || (operation == BUZZER_TIMER && (RTC_CR & 0x1100u))) {
    unlock(saved);
    return TB_BUSY;
  }
  if (operation == BUZZER_PLAY) {
    const bool started = start_profile(&profile, W(0x75c));
    unlock(saved);
    return started ? TB_OK : TB_BACKEND;
  }
  unlock(saved);
  /* Native RTC waits must run with IRQs enabled. The setter hook clears old ownership. */
  if (!timer_set(0, minutes)) {
    return TB_BACKEND;
  }
  saved = lock();
  const bool armed = platform_ready() && (RTC_CR & 0x1100u) == 0x1100u && !B(0x8a8);
  if (armed) {
    ALARM->profile = profile;
    ALARM->rtc_alarm = RTC_ALARM;
    ALARM->state = BUZZER_ALARM_ARMED;
  }
  unlock(saved);
  if (!armed) {
    (void)timer_cancel(0);
    saved = lock();
    /* This just-created alarm must not fall through to the native sound. */
    ALARM->state = BUZZER_ALARM_CONSUMED;
    finish_sound();
    unlock(saved);
  }
  return armed ? TB_OK : TB_BACKEND;
}

__attribute__((noinline)) int buzzer_extension_route(void) {
  const volatile uint8_t* received = (const volatile uint8_t*)W(0xe8);
  if (received[0] != 0xd7) {
    return 0;
  }
  const uint8_t size = B(0x124);
  if (size < 6 || size > BUZZER_TIMER_SIZE + 1) {
    return 1;
  }
  uint8_t request[BUZZER_TIMER_SIZE], reply[8];
  for (uint8_t i = 0; i < size - 1; ++i) {
    request[i] = received[i];
  }
  if (request[1] == 1 && size > 11) {
    return 1;
  }
  if (request[1] != 1 && request[1] != 3) {
    return 1;
  }
  if ((uint8_t)(B(0xf4) - B(0xf5) - 1) < 20) {
    return 1;
  }
  uint32_t mask;
  __asm volatile("mrs %0, PRIMASK" : "=r"(mask));
  if (mask) {
    return 1;
  }
  const bool enabled = platform_ready();
  const uint8_t length = request[1] == 3 ? 8 : 6;
  for (uint8_t i = 0; i < 5; ++i) {
    reply[i] = request[i];
  }
  reply[0] = 0xd8;
  reply[5] = TB_BACKEND;
  reply[6] = 0;
  reply[7] = 0;
  if (enabled) {
    if (request[1] == 3) {
      reply[5] = profile_request(request, size - 1);
      uint32_t saved = lock();
      reply[6] = 0xf3;
      reply[7] = status_bits();
      unlock(saved);
    } else if (request[2] == 1 || request[2] == 2) {
      tb_request(STATE, &sound_platform, &request[2], request, size - 1, W(0x75c), reply);
    } else {
      uint32_t saved = lock();
      tb_request(STATE, &sound_platform, 0, request, size - 1, W(0x75c), reply);
      unlock(saved);
    }
  }
  volatile uint8_t* tx = (volatile uint8_t*)W(0xe0);
  tx[0] = 0xfa;
  for (uint8_t i = 0; i < length; ++i) {
    tx[i + 1] = reply[i];
  }
  B(0xe4) = length;
  FN(0x08003611u, void (*)(uint32_t, uint32_t))(0x200000f0u, 0x200000e0u);
  H(0x120)++;
  return 1;
}

int tb01_route(void) {
  int original = FN(0x080097e1u, int (*)(void))();
  return original ? original : buzzer_extension_route();
}
