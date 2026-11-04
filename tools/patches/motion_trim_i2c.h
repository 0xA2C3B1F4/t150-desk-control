#ifndef DESK_MOTION_TRIM_I2C_H
#define DESK_MOTION_TRIM_I2C_H

#include <stdbool.h>
#include <stdint.h>

enum {
  TRIM_I2C_CR1 = 0x00,
  TRIM_I2C_CR2 = 0x04,
  TRIM_I2C_OAR1 = 0x08,
  TRIM_I2C_OAR2 = 0x0C,
  TRIM_I2C_TIMINGR = 0x10,
  TRIM_I2C_TIMEOUTR = 0x14,
  TRIM_I2C_ISR = 0x18,
  TRIM_I2C_ICR = 0x1C,
  TRIM_I2C_RXDR = 0x24,
  TRIM_I2C_TXDR = 0x28
};
enum { TRIM_I2C_NONE, TRIM_I2C_ACQUIRE, TRIM_I2C_READ, TRIM_I2C_WRITE, TRIM_I2C_CLEANUP };
enum { TRIM_I2C_IDLE, TRIM_I2C_PENDING, TRIM_I2C_OK, TRIM_I2C_FAILED, TRIM_I2C_UNRELEASED };
enum {
  TRIM_I2C_NO_FAULT,
  TRIM_I2C_CONFIG,
  TRIM_I2C_NOT_IDLE,
  TRIM_I2C_DEADLINE,
  TRIM_I2C_TRANSFER_TIMEOUT,
  TRIM_I2C_READY_TIMEOUT,
  TRIM_I2C_NACK,
  TRIM_I2C_BUS_FAULT,
  TRIM_I2C_SEQUENCE,
  TRIM_I2C_FLAG_CLEAR,
  TRIM_I2C_RESET_FAILED,
  TRIM_I2C_PROBE_BERR
};

/* All callbacks are bounded and synchronous. now uses a qualified monotonic
 * clock in the same units as the profile. It must run while I2C is disabled.
 * The caller excludes native I2C users and serializes all driver calls. */
typedef struct {
  uint32_t (*read)(void* context, uint8_t offset);
  void (*write)(void* context, uint8_t offset, uint32_t value);
  bool (*lines_high)(void* context);
  uint32_t (*now)(void* context);
  void* context;
} TrimI2cPort;

typedef struct {
  uint32_t timing;
  /* Include foreground service and master stretching, not just wire time. */
  uint32_t transfer_ticks;
  uint32_t write_ticks;
  uint32_t poll_ticks;
  uint32_t stop_ticks;
  uint32_t max_poll_gap;
  uint8_t address;
} TrimI2cProfile;

typedef struct {
  uint32_t acquire;
  uint32_t read;
  uint32_t write;
  uint32_t cleanup;
  uint32_t ready;
} TrimI2cBounds;

/* Caller-owned, read-only between calls. No RAM address is allocated here.
 * operation_action and operation_berr retain byte context across cleanup. A read
 * with operation_berr is unsuitable as persistence proof. result describes
 * the current action; released is independent of successful persistence.
 * owned only records verified driver configuration. Its absence never grants
 * native access after a failed acquisition; only released certifies handoff. */
typedef struct {
  TrimI2cPort port;
  TrimI2cProfile profile;
  TrimI2cBounds bounds;
  uint32_t saved[5];
  uint32_t started;
  uint32_t limit;
  uint32_t phase_started;
  uint32_t ready_started;
  uint32_t interval_started;
  uint32_t released_at;
  uint8_t phase;
  uint8_t action;
  uint8_t result;
  uint8_t fault;
  uint8_t operation_berr;
  uint8_t operation_action;
  uint8_t owned;
  uint8_t released;
  uint8_t have_saved;
  uint8_t possible_programming;
  uint8_t berr_seen;
  uint8_t probe_nack;
  uint8_t read_abort;
  uint8_t address;
  uint8_t value;
} TrimI2cState;

/* Bounds exclude the final tick needed by the exclusive action deadline.
 * Structural validation cannot qualify electrical timing, tWR or the part's
 * one-byte word addressing. Those are caller prerequisites, not defaults. */
bool trim_i2c_bounds(const TrimI2cProfile* profile, TrimI2cBounds* bounds);

/* Only at boot or after independent recovery with old lifetime closed. This
 * function does not recover a peripheral or override native ownership. */
bool trim_i2c_init(TrimI2cState* state, const TrimI2cPort* port, const TrimI2cProfile* profile);
/* A rejected preflight that saved no configuration wrote no registers and may
 * be retried explicitly. Any later acquire failure requires cleanup. */
bool trim_i2c_acquire(TrimI2cState* state, uint32_t deadline);
bool trim_i2c_read(TrimI2cState* state, uint8_t address, uint32_t deadline);
bool trim_i2c_write(TrimI2cState* state, uint8_t address, uint8_t value, uint32_t deadline);
bool trim_i2c_cleanup(TrimI2cState* state, uint32_t deadline);

/* One bounded transition, no waits or interrupt masks. Sample status before
 * the clock. Closed actions perform no I/O. Unreleased rejects every future
 * action without I/O; recovery is deliberately outside this interface.
 * Call often enough to meet max_poll_gap. A missed action deadline fails;
 * it never extends the owner deadline. BERR is cleared and recorded during
 * active transfers; an idle BERR rejects the next transfer instead. */
void trim_i2c_poll(TrimI2cState* state);
bool trim_i2c_closed(const TrimI2cState* state);

/* Real STM32F030 I2C1/PB8/PB9 accessors. They supply no clock, configure no
 * GPIO/RCC and acquire no ownership. Install them in a port only after the
 * firmware adapter validates those conditions and provides a bounded now. */
uint32_t trim_i2c_mmio_read(void* context, uint8_t offset);
void trim_i2c_mmio_write(void* context, uint8_t offset, uint32_t value);
bool trim_i2c_mmio_lines_high(void* context);

#endif
