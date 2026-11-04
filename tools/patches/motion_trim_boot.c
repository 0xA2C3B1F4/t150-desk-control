#include "motion_trim_controller.h"
#include "platform_runtime.h"

void trim_controller_boot(void) {
#ifdef TRIM_BOOT_TIMING
  static const TrimI2cProfile profile = {
      TRIM_BOOT_TIMING,     TRIM_BOOT_TRANSFER_TICKS, TRIM_BOOT_WRITE_TICKS, TRIM_BOOT_POLL_TICKS,
      TRIM_BOOT_STOP_TICKS, TRIM_BOOT_MAX_POLL_GAP,   TRIM_BOOT_ADDRESS};
  (void)trim_controller_start(&profile, TRIM_BOOT_RECORD_ADDRESS, TRIM_BOOT_DELIVERY_TICKS);
#endif
}

static uint32_t enter(void* context) {
  (void)context;
  uint32_t previous;
  __asm volatile("mrs %0, PRIMASK\ncpsid i" : "=r"(previous)::"memory");
  return previous;
}

static void leave(void* context, uint32_t previous) {
  (void)context;
  __asm volatile("msr PRIMASK, %0" ::"r"(previous) : "memory");
}

static uint32_t now(void* context) {
  (void)context;
  return *(volatile uint32_t*)0x2000075cu;
}

bool trim_controller_start(const TrimI2cProfile* profile, uint8_t record_address,
                           uint32_t delivery) {
  uint32_t previous;
  __asm volatile("mrs %0, PRIMASK" : "=r"(previous));
  if (previous || !platform_ready() || trim_controller_started()) {
    return false;
  }
  /* No default profile: the builder must supply qualified electrical and
   * scheduling bounds. Initialization itself performs no peripheral access. */
  const TrimI2cPort port = {trim_i2c_mmio_read, trim_i2c_mmio_write, trim_i2c_mmio_lines_high, now,
                            0};
  const TrimAdapterLock lock = {enter, leave, 0};
  TrimAdapterState* adapter = (TrimAdapterState*)PLATFORM_TRIM_ADAPTER_ADDRESS;
  if (!trim_adapter_init(adapter, (TrimOwnerState*)PLATFORM_TRIM_OWNER_ADDRESS,
                         (TrimStorageState*)PLATFORM_TRIM_RECORD_ADDRESS,
                         (TrimI2cState*)PLATFORM_TRIM_I2C_ADDRESS, &port, profile, &lock,
                         record_address, TRIM_STORAGE_WINDOW, delivery)) {
    return false;
  }
  /* Publish the owner and capture bridge together before the first I2C poll.
   * A mandatory native save cannot pass between these two publications. */
  previous = enter(0);
  bool started = trim_adapter_begin_boot(adapter) && trim_controller_activate();
  leave(0, previous);
  return started;
}
