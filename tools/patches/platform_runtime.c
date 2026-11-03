/* FP01 is restricted to the documented C8 board and exact R7 firmware.
 * The register pair rejects the documented F030x4/x6 profile. It does not
 * authenticate an arbitrary STM32 or replace physical SRAM acceptance. */
#include "platform_runtime.h"
#include "tb01_core.h"
#define WORD(a) (*(volatile uint32_t*)(a))
#define BYTE(a) (*(volatile uint8_t*)(a))
typedef struct {
  uint32_t magic, layout, ram_abi, modules_ready, fault, reserved[3];
} PlatformHeader;
#define HEADER ((volatile PlatformHeader*)PLATFORM_HEADER_ADDRESS)
_Static_assert(sizeof(PlatformHeader) == PLATFORM_HEADER_SIZE, "header slot");
_Static_assert(sizeof(tb_state) <= PLATFORM_BUZZER_SIZE, "buzzer slot");
_Static_assert(sizeof(tb_state) == 12, "preserve the 32-bit TB01 deadline");
_Static_assert(PLATFORM_HEADER_ADDRESS % 4 == 0, "header alignment");
_Static_assert(PLATFORM_BUZZER_ADDRESS % 4 == 0, "buzzer alignment");
_Static_assert(PLATFORM_GUARD_ADDRESS % 4 == 0, "guard alignment");
static uint32_t platform_lock(void) {
  uint32_t saved;
  __asm volatile("mrs %0, PRIMASK\ncpsid i" : "=r"(saved)::"memory");
  return saved;
}
static void platform_unlock(uint32_t saved) {
  __asm volatile("msr PRIMASK, %0" ::"r"(saved) : "memory");
}
bool platform_profile_matches(void) {
  /* RM0360 Rev 5, 7.4.7: DBGMCUEN resets off; inactive registers read zero.
   * Match ST's __HAL_RCC_DBGMCU_CLK_ENABLE readback delay, then restore only
   * the DBG clock bit. Native ownership of all other APB2 clocks is retained.
   * RM0360 26.4.1 requires a 32-bit IDCODE read; ST CMSIS/LL uses a 32-bit
   * FLASHSIZE_BASE read. The installation profile remains a separate gate. */
  uint32_t saved = platform_lock();
  uint32_t previous = WORD(0x40021018u);
  if (!(previous & 0x00400000u)) {
    WORD(0x40021018u) = previous | 0x00400000u;
    (void)WORD(0x40021018u);
  }
  uint32_t identity = WORD(0x40015800u);
  uint32_t flash_size = WORD(0x1ffff7ccu);
  if (!(previous & 0x00400000u))
    WORD(0x40021018u) = WORD(0x40021018u) & ~0x00400000u;
  platform_unlock(saved);
  return (identity & 0xfffu) == 0x440u && (flash_size & 0xffffu) == 64u;
}
static void platform_release_locked(void) {
  /* The check and output release form one atomic native-ownership decision.
   * No corrupted state chooses the output or address. */
  if (!BYTE(0x200008a8u) && !BYTE(0x2000014du))
    WORD(0x48001428u) = 0x80u;
}
static void platform_fault(void) {
  uint32_t saved = platform_lock();
  HEADER->modules_ready = 0;
  HEADER->fault = 1;
  platform_release_locked();
  platform_unlock(saved);
}
bool platform_initialized(void) {
  /* This gate precedes even the first header read on every hook. */
  if (!platform_profile_matches())
    return false;
  if (HEADER->magic != PLATFORM_VALID_MAGIC || HEADER->layout != PLATFORM_LAYOUT_VERSION ||
      HEADER->ram_abi != PLATFORM_RAM_ABI || HEADER->fault) {
    platform_fault();
    return false;
  }
  for (uint32_t a = PLATFORM_GUARD_ADDRESS; a < PLATFORM_RAM_END; a += 4) {
    if (WORD(a) != PLATFORM_GUARD_WORD) {
      platform_fault();
      return false;
    }
  }
  return true;
}
bool platform_ready(void) {
  if (!platform_initialized())
    return false;
  uint32_t ready = HEADER->modules_ready;
  if (ready == 1u)
    return true;
  if (ready) {
    platform_fault();
  } else {
    /* Zero is also the valid pre-init state. Release any own pulse without
     * preventing the later module init from publishing readiness. */
    uint32_t saved = platform_lock();
    if (((volatile tb_state*)PLATFORM_BUZZER_ADDRESS)->phase)
      platform_release_locked();
    platform_unlock(saved);
  }
  return false;
}
void platform_modules_ready(void) {
  if (platform_initialized())
    HEADER->modules_ready = 1;
}
void platform_bootstrap(void) {
  if (!platform_profile_matches())
    return;
  /* The reset/pre-remap hook runs with PRIMASK set. Never inspect old RAM:
   * word writes initialize parity for the complete reservation first. */
  for (uint32_t a = PLATFORM_RAM_START; a < PLATFORM_RAM_END; a += 4)
    WORD(a) = 0;
  HEADER->layout = PLATFORM_LAYOUT_VERSION;
  HEADER->ram_abi = PLATFORM_RAM_ABI;
  for (uint32_t a = PLATFORM_GUARD_ADDRESS; a < PLATFORM_RAM_END; a += 4)
    WORD(a) = PLATFORM_GUARD_WORD;
  /* The module-ready flag remains clear until the original TB01 init hook. */
  HEADER->magic = PLATFORM_VALID_MAGIC;
}
