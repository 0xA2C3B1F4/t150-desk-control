#include "buzzer_pwm.h"
#include "buzzer_profile.h"

#define REG(address) (*(volatile uint32_t*)(address))
#define TIM(offset) REG(0x40014000u + (offset))
#define DMA(offset) REG(0x40020058u + (offset))
#define RCC REG(0x40021018u)
#define PF_BSRR REG(0x48001418u)
#define TIMER_CLOCK (1u << 16)
#define OWN_TAG 0x5057u

/* Compare turns PF7 off, update turns it on. The circular source stays in flash. */
static const uint32_t pattern[2] = {0x00800000u, 0x00000080u};

bool buzzer_pwm_owned(void) {
  return (RCC & TIMER_CLOCK) && TIM(0x38) == OWN_TAG;
}

bool buzzer_pwm_available(void) {
  return !(RCC & TIMER_CLOCK) && !(DMA(0) & 1u);
}

bool buzzer_pwm_running(void) {
  return buzzer_pwm_owned() && (DMA(0) & 1u) && !(REG(0x40020000u) & 0x80000u);
}

static void stop_engine(void) {
  TIM(0x0c) = 0;
  TIM(0) = 0;
  DMA(0) &= ~1u;
  __asm volatile("dsb" ::: "memory");
  (void)DMA(0);
  REG(0x40020004u) = 0x000f0000u;
}

void buzzer_pwm_pause(void) {
  if (buzzer_pwm_owned()) {
    stop_engine();
  }
  PF_BSRR = 0x00800000u;
}

void buzzer_pwm_release(void) {
  if (buzzer_pwm_owned()) {
    stop_engine();
    TIM(0x38) = 0;
    RCC &= ~TIMER_CLOCK;
  }
}

void buzzer_pwm_compare(uint16_t compare) {
  if (buzzer_pwm_owned() && compare >= BUZZER_PERIOD / 20 && compare <= BUZZER_PERIOD * 95 / 100) {
    TIM(0x34) = compare;
  }
}

bool buzzer_pwm_resume(bool direct, uint16_t compare) {
  if (direct) {
    if (!buzzer_pwm_available()) {
      return false;
    }
    PF_BSRR = 0x80;
    return true;
  }
  if (!buzzer_pwm_owned() || compare < BUZZER_PERIOD / 20 || compare > BUZZER_PERIOD * 95 / 100) {
    return false;
  }
  stop_engine();
  TIM(0x34) = compare;
  TIM(0x24) = 0;
  TIM(0x14) = 1;
  TIM(0x10) = 0;
  DMA(0x08) = 0x48001418u;
  DMA(0x0c) = (uint32_t)(uintptr_t)pattern;
  DMA(0x04) = 2;
  DMA(0) = 0x0ab0u;
  DMA(0) = 0x0ab1u;
  PF_BSRR = 0x80;
  TIM(0x0c) = 0x0300u;
  TIM(0) = 1;
  return true;
}

bool buzzer_pwm_begin(uint8_t volume, uint16_t compare) {
  if (!volume || volume > 100 || volume % 5 || !buzzer_pwm_available() ||
      (REG(0x48001400u) & 0xc000u) != 0x4000u) {
    return false;
  }
  if (volume == 100) {
    return buzzer_pwm_resume(true, 0);
  }
  /* HSI/2 x12, PLL selected, no HCLK/PCLK division. DMA and GPIO are native clocks. */
  if ((REG(0x40021004u) & 0x003d07fcu) != 0x00280008u ||
      (REG(0x40021014u) & 0x00400001u) != 0x00400001u || compare < BUZZER_PERIOD / 20 ||
      compare > BUZZER_PERIOD * 95 / 100) {
    return false;
  }
  RCC |= TIMER_CLOCK;
  (void)RCC;
  TIM(0) = 0;
  TIM(0x04) = 0;
  TIM(0x08) = 0;
  TIM(0x0c) = 0;
  TIM(0x10) = 0;
  TIM(0x18) = 8;
  TIM(0x20) = 0;
  TIM(0x30) = 0;
  TIM(0x44) = 0;
  TIM(0x28) = 0;
  TIM(0x2c) = BUZZER_PERIOD - 1;
  TIM(0x34) = compare;
  TIM(0x38) = OWN_TAG;
  return buzzer_pwm_resume(false, compare);
}
