#include "motion_trim_i2c.h"

enum {
  PE = 1,
  TXE = 1,
  TXIS = 2,
  RXNE = 4,
  ADDR = 8,
  NACK = 0x10,
  STOPF = 0x20,
  TC = 0x40,
  TCR = 0x80,
  BERR = 0x100,
  ARLO = 0x200,
  OVR = 0x400,
  PECERR = 0x800,
  TIMEOUT = 0x1000,
  ALERT = 0x2000,
  BUSY = 0x8000,
  READ_DIRECTION = 0x400,
  START = 0x2000,
  STOP = 0x4000,
  AUTOEND = 0x2000000,
  STATUS_MASK = 0xBFFF,
  CLEAR_FLAGS = STOPF | NACK | BERR,
  FATAL_FLAGS = ADDR | TCR | ARLO | OVR | PECERR | TIMEOUT | ALERT
};
static const uint32_t CR2_FORBIDDEN = 0xFD009800u;
enum {
  ACQUIRE_CHECK,
  ACQUIRE_DISABLE,
  ACQUIRE_SETUP,
  ACQUIRE_VERIFY,
  BYTE_START,
  READ_ADDRESS,
  READ_DATA,
  WRITE_DATA,
  BYTE_STOP,
  READY_START,
  READY_WAIT,
  READY_INTERVAL,
  CLEANUP_GRACE,
  CLEANUP_WAIT,
  CLEANUP_DISABLE,
  CLEANUP_SETUP,
  CLEANUP_VERIFY,
  CLEANUP_PROGRAM,
  CLEANUP_PROBE,
  CLEANUP_PROBE_WAIT,
  RESTORE_DISABLE,
  RESTORE_SETUP,
  RESTORE_VERIFY,
  CLEANUP_ABORT
};

typedef struct {
  uint32_t cr1;
  uint32_t cr2;
  uint32_t oar1;
  uint32_t oar2;
  uint32_t timing;
  uint32_t timeout;
  uint32_t status;
  bool lines;
} Registers;

static bool duration_valid(uint32_t duration) {
  return duration != 0 && duration < 0x80000000u;
}

static bool add_bound(uint32_t* sum, uint32_t term) {
  if (term >= 0x80000000u || *sum >= 0x80000000u - term) {
    return false;
  }
  *sum += term;
  return true;
}

bool trim_i2c_bounds(const TrimI2cProfile* profile, TrimI2cBounds* bounds) {
  if (!profile || !bounds || profile->address < 8 || profile->address > 0x77 || !profile->timing ||
      (profile->timing & 0x0F000000u) || !duration_valid(profile->transfer_ticks) ||
      !duration_valid(profile->write_ticks) || !duration_valid(profile->poll_ticks) ||
      !duration_valid(profile->stop_ticks) || !duration_valid(profile->max_poll_gap) ||
      profile->max_poll_gap >= 0x08000000u) {
    return false;
  }
  TrimI2cBounds value = {0};
  value.acquire = 4 * profile->max_poll_gap;
  value.read = 8 * profile->max_poll_gap;
  value.write = 16 * profile->max_poll_gap;
  value.cleanup = value.write;
  value.ready = value.read;
  if (!add_bound(&value.read, profile->transfer_ticks) ||
      !add_bound(&value.read, profile->transfer_ticks) ||
      !add_bound(&value.ready, profile->write_ticks) ||
      !add_bound(&value.ready, profile->poll_ticks) ||
      !add_bound(&value.ready, profile->transfer_ticks) ||
      !add_bound(&value.ready, profile->transfer_ticks) ||
      !add_bound(&value.write, profile->write_ticks) ||
      !add_bound(&value.write, profile->poll_ticks) ||
      !add_bound(&value.write, profile->transfer_ticks) ||
      !add_bound(&value.write, profile->transfer_ticks) ||
      !add_bound(&value.write, profile->transfer_ticks) ||
      !add_bound(&value.cleanup, profile->stop_ticks) ||
      !add_bound(&value.cleanup, profile->write_ticks) ||
      !add_bound(&value.cleanup, profile->transfer_ticks)) {
    return false;
  }
  *bounds = value;
  return true;
}

bool trim_i2c_init(TrimI2cState* state, const TrimI2cPort* port, const TrimI2cProfile* profile) {
  TrimI2cBounds bounds;
  if (!state || !port || !port->read || !port->write || !port->lines_high || !port->now ||
      !trim_i2c_bounds(profile, &bounds)) {
    return false;
  }
  *state = (TrimI2cState){.port = *port, .profile = *profile, .bounds = bounds};
  return true;
}

static uint32_t read_register(TrimI2cState* state, uint8_t offset) {
  return state->port.read(state->port.context, offset);
}

static void write_register(TrimI2cState* state, uint8_t offset, uint32_t value) {
  state->port.write(state->port.context, offset, value);
}

static uint32_t now_ticks(TrimI2cState* state) {
  return state->port.now(state->port.context);
}

static bool begin(TrimI2cState* state, uint8_t action, uint8_t phase, uint32_t bound,
                  uint32_t deadline) {
  uint32_t now = now_ticks(state);
  uint32_t limit = deadline - now;
  if (!duration_valid(limit) || limit <= bound) {
    return false;
  }
  state->started = now;
  state->limit = limit;
  state->phase_started = now;
  state->phase = phase;
  state->action = action;
  state->result = TRIM_I2C_PENDING;
  state->fault = TRIM_I2C_NO_FAULT;
  state->berr_seen = 0;
  state->probe_nack = 0;
  state->released = 0;
  return true;
}

bool trim_i2c_acquire(TrimI2cState* state, uint32_t deadline) {
  if (!state ||
      !(state->result == TRIM_I2C_IDLE ||
        (state->action == TRIM_I2C_CLEANUP && state->result == TRIM_I2C_OK && state->released))) {
    return false;
  }
  if (!begin(state, TRIM_I2C_ACQUIRE, ACQUIRE_CHECK, state->bounds.acquire, deadline)) {
    return false;
  }
  state->have_saved = 0;
  state->operation_berr = 0;
  state->operation_action = TRIM_I2C_NONE;
  state->value = 0;
  return true;
}

static bool begin_byte(TrimI2cState* state, uint8_t action, uint8_t address, uint8_t value,
                       uint32_t deadline) {
  if (!state || state->result != TRIM_I2C_OK || !state->owned || state->possible_programming) {
    return false;
  }
  uint32_t bound = action == TRIM_I2C_READ ? state->bounds.read : state->bounds.write;
  if (!begin(state, action, BYTE_START, bound, deadline)) {
    return false;
  }
  state->address = address;
  state->value = value;
  state->operation_berr = 0;
  state->operation_action = action;
  return true;
}

bool trim_i2c_read(TrimI2cState* state, uint8_t address, uint32_t deadline) {
  return begin_byte(state, TRIM_I2C_READ, address, 0, deadline);
}

bool trim_i2c_write(TrimI2cState* state, uint8_t address, uint8_t value, uint32_t deadline) {
  return begin_byte(state, TRIM_I2C_WRITE, address, value, deadline);
}

bool trim_i2c_cleanup(TrimI2cState* state, uint32_t deadline) {
  if (!state || !state->have_saved || state->result == TRIM_I2C_PENDING ||
      state->result == TRIM_I2C_UNRELEASED || state->released) {
    return false;
  }
  uint8_t read_abort = state->action == TRIM_I2C_READ;
  if (!begin(state, TRIM_I2C_CLEANUP, CLEANUP_GRACE, state->bounds.cleanup, deadline)) {
    return false;
  }
  state->read_abort = read_abort;
  return true;
}

bool trim_i2c_closed(const TrimI2cState* state) {
  return state->result != TRIM_I2C_PENDING;
}

static void finish(TrimI2cState* state, uint8_t result, uint8_t fault) {
  state->result = result;
  state->fault = fault;
  if (state->action == TRIM_I2C_READ || state->action == TRIM_I2C_WRITE) {
    state->operation_berr = state->berr_seen;
  }
}

static void fail(TrimI2cState* state, uint8_t fault) {
  if (state->action == TRIM_I2C_CLEANUP) {
    state->fault = fault;
    state->phase = CLEANUP_ABORT;
  } else {
    finish(state, TRIM_I2C_FAILED, fault);
  }
}

static bool on_time(TrimI2cState* state, uint32_t now) {
  if (now - state->started >= state->limit) {
    finish(state, state->action == TRIM_I2C_CLEANUP ? TRIM_I2C_UNRELEASED : TRIM_I2C_FAILED,
           TRIM_I2C_DEADLINE);
    return false;
  }
  return true;
}

static uint32_t snapshot(TrimI2cState* state, Registers* registers) {
  registers->cr1 = read_register(state, TRIM_I2C_CR1);
  /* Completion must precede the control read: an interrupt between these
   * accesses can let hardware clear START and finish the whole transfer. */
  registers->status = read_register(state, TRIM_I2C_ISR) & STATUS_MASK;
  registers->cr2 = read_register(state, TRIM_I2C_CR2);
  registers->oar1 = read_register(state, TRIM_I2C_OAR1);
  registers->oar2 = read_register(state, TRIM_I2C_OAR2);
  registers->timing = read_register(state, TRIM_I2C_TIMINGR);
  registers->timeout = read_register(state, TRIM_I2C_TIMEOUTR);
  registers->lines = state->port.lines_high(state->port.context);
  return now_ticks(state);
}

static bool idle(const Registers* registers) {
  return registers->status == TXE && !(registers->cr2 & (START | STOP)) && registers->lines;
}

static bool owned_configuration(const TrimI2cState* state, const Registers* registers) {
  return registers->cr1 == PE && registers->oar1 == 0 && registers->oar2 == 0 &&
         registers->timing == state->profile.timing && registers->timeout == 0 &&
         !(registers->cr2 & CR2_FORBIDDEN);
}

static bool disable(TrimI2cState* state) {
  write_register(state, TRIM_I2C_CR1, 0);
  uint32_t cr1 = read_register(state, TRIM_I2C_CR1);
  uint32_t cr2 = read_register(state, TRIM_I2C_CR2);
  uint32_t now = now_ticks(state);
  if (!on_time(state, now)) {
    return false;
  }
  if (cr1 != 0 || (cr2 & (START | STOP))) {
    fail(state, TRIM_I2C_RESET_FAILED);
    return false;
  }
  state->released_at = now;
  return true;
}

static void setup_owned(TrimI2cState* state) {
  write_register(state, TRIM_I2C_OAR1, 0);
  write_register(state, TRIM_I2C_OAR2, 0);
  write_register(state, TRIM_I2C_CR2, 0);
  write_register(state, TRIM_I2C_TIMINGR, state->profile.timing);
  write_register(state, TRIM_I2C_TIMEOUTR, 0);
  write_register(state, TRIM_I2C_CR1, PE);
}

static bool clear_status(TrimI2cState* state, uint32_t expected) {
  write_register(state, TRIM_I2C_ICR, CLEAR_FLAGS);
  uint32_t status = read_register(state, TRIM_I2C_ISR) & STATUS_MASK;
  if (!on_time(state, now_ticks(state))) {
    return false;
  }
  if (status != expected) {
    fail(state, TRIM_I2C_FLAG_CLEAR);
    return false;
  }
  return true;
}

static void start(TrimI2cState* state, uint32_t control, uint8_t next_phase, bool preload,
                  uint32_t expected) {
  if (!clear_status(state, expected)) {
    return;
  }
  uint32_t now = now_ticks(state);
  if (!on_time(state, now)) {
    return;
  }
  if (state->phase == READ_ADDRESS && now - state->phase_started >= state->profile.transfer_ticks) {
    fail(state, TRIM_I2C_TRANSFER_TIMEOUT);
    return;
  }
  if (state->phase == READY_START && now - state->ready_started >= state->bounds.ready) {
    fail(state, TRIM_I2C_READY_TIMEOUT);
    return;
  }
  if (preload) {
    write_register(state, TRIM_I2C_TXDR, state->address);
  }
  write_register(state, TRIM_I2C_CR2, ((uint32_t)state->profile.address << 1) | START | control);
  state->phase_started = now_ticks(state);
  state->phase = next_phase;
  state->probe_nack = 0;
}

static bool transfer_status(TrimI2cState* state, Registers* registers, uint32_t now,
                            bool allow_nack) {
  if (now - state->phase_started >= state->profile.transfer_ticks) {
    fail(state, TRIM_I2C_TRANSFER_TIMEOUT);
    return false;
  }
  if (registers->status & BERR) {
    state->berr_seen = 1;
    write_register(state, TRIM_I2C_ICR, BERR);
    registers->status &= ~BERR;
  }
  if (registers->status & FATAL_FLAGS) {
    fail(state, TRIM_I2C_BUS_FAULT);
    return false;
  }
  if (!allow_nack && (registers->status & NACK)) {
    fail(state, TRIM_I2C_NACK);
    return false;
  }
  return true;
}

static bool stopped(TrimI2cState* state, const Registers* registers, bool allow_nack) {
  if (!(registers->status & STOPF) || (registers->status & BUSY)) {
    return false;
  }
  uint32_t expected = TXE | STOPF;
  if (allow_nack) {
    expected |= registers->status & NACK;
  }
  if (registers->status != expected || (registers->cr2 & (START | STOP))) {
    fail(state, TRIM_I2C_SEQUENCE);
    return false;
  }
  if (!registers->lines) {
    fail(state, TRIM_I2C_NOT_IDLE);
    return false;
  }
  if (!clear_status(state, TXE)) {
    return false;
  }
  uint32_t now = now_ticks(state);
  if (!on_time(state, now)) {
    return false;
  }
  if (now - state->phase_started >= state->profile.transfer_ticks) {
    fail(state, TRIM_I2C_TRANSFER_TIMEOUT);
    return false;
  }
  return true;
}

static void byte_poll(TrimI2cState* state, Registers* registers, uint32_t now) {
  if (!owned_configuration(state, registers)) {
    fail(state, TRIM_I2C_CONFIG);
    return;
  }
  if (state->phase >= READY_START && now - state->ready_started >= state->bounds.ready) {
    fail(state, TRIM_I2C_READY_TIMEOUT);
    return;
  }
  if (state->phase != BYTE_START && state->phase != READY_START && state->phase != READY_INTERVAL &&
      !transfer_status(state, registers, now, state->phase == READY_WAIT)) {
    return;
  }
  switch (state->phase) {
    case BYTE_START:
      if (!idle(registers)) {
        fail(state, TRIM_I2C_NOT_IDLE);
      } else if (state->action == TRIM_I2C_READ) {
        start(state, 1u << 16, READ_ADDRESS, true, TXE);
      } else {
        start(state, (2u << 16) | AUTOEND, WRITE_DATA, true, TXE);
      }
      break;
    case READ_ADDRESS:
      if (registers->status & (STOPF | RXNE)) {
        fail(state, TRIM_I2C_SEQUENCE);
      } else if (registers->status & TC) {
        if (registers->status != (TXE | TC | BUSY) || (registers->cr2 & (START | STOP))) {
          fail(state, TRIM_I2C_SEQUENCE);
        } else {
          start(state, (1u << 16) | READ_DIRECTION | AUTOEND, READ_DATA, false, TXE | TC | BUSY);
        }
      }
      break;
    case READ_DATA:
      if (registers->status & (TC | TXIS)) {
        fail(state, TRIM_I2C_SEQUENCE);
      } else if (registers->status & RXNE) {
        state->value = (uint8_t)read_register(state, TRIM_I2C_RXDR);
        if (on_time(state, now_ticks(state))) {
          state->phase = BYTE_STOP;
        }
      } else if (registers->status & STOPF) {
        fail(state, TRIM_I2C_SEQUENCE);
      }
      break;
    case WRITE_DATA:
      if (registers->status & (STOPF | RXNE | TC)) {
        fail(state, TRIM_I2C_SEQUENCE);
      } else if (registers->status & TXIS) {
        state->possible_programming = 1;
        write_register(state, TRIM_I2C_TXDR, state->value);
        state->phase = BYTE_STOP;
      }
      break;
    case BYTE_STOP:
      if (stopped(state, registers, false)) {
        if (state->action == TRIM_I2C_READ) {
          finish(state, TRIM_I2C_OK, TRIM_I2C_NO_FAULT);
        } else {
          state->ready_started = now_ticks(state);
          state->phase = READY_START;
        }
      }
      break;
    case READY_START:
      if (!idle(registers)) {
        fail(state, TRIM_I2C_NOT_IDLE);
      } else {
        start(state, AUTOEND, READY_WAIT, false, TXE);
      }
      break;
    case READY_WAIT:
      state->probe_nack |= (registers->status & NACK) != 0;
      if (stopped(state, registers, true)) {
        if (state->probe_nack) {
          state->interval_started = now_ticks(state);
          state->phase = READY_INTERVAL;
        } else {
          state->possible_programming = 0;
          finish(state, TRIM_I2C_OK, TRIM_I2C_NO_FAULT);
        }
      }
      break;
    case READY_INTERVAL:
      if (!idle(registers)) {
        fail(state, TRIM_I2C_NOT_IDLE);
      } else if (now - state->interval_started >= state->profile.poll_ticks) {
        state->phase = READY_START;
      }
      break;
    default:
      fail(state, TRIM_I2C_SEQUENCE);
      break;
  }
}

static void cleanup_poll(TrimI2cState* state, Registers* registers, uint32_t now) {
  switch (state->phase) {
    case CLEANUP_GRACE:
      state->phase_started = now;
      state->phase = CLEANUP_DISABLE;
      if (state->read_abort && (registers->status & TC) && !(registers->cr2 & (START | AUTOEND))) {
        write_register(state, TRIM_I2C_CR2, (registers->cr2 & ~START) | STOP);
        state->phase = CLEANUP_WAIT;
      } else if ((registers->cr2 & AUTOEND) && (registers->status & BUSY) &&
                 !(registers->status & TXIS) && !(registers->cr2 & START)) {
        state->phase = CLEANUP_WAIT;
      }
      break;
    case CLEANUP_WAIT:
      if (((registers->status & STOPF) && !(registers->status & BUSY)) ||
          now - state->phase_started >= state->profile.stop_ticks) {
        state->phase = CLEANUP_DISABLE;
      }
      break;
    case CLEANUP_DISABLE:
      if (disable(state)) {
        state->phase = CLEANUP_SETUP;
      }
      break;
    case CLEANUP_SETUP:
      if (registers->cr1 != 0 || (registers->cr2 & (START | STOP))) {
        fail(state, TRIM_I2C_RESET_FAILED);
      } else {
        setup_owned(state);
        state->phase = CLEANUP_VERIFY;
      }
      break;
    case CLEANUP_VERIFY:
      if (!owned_configuration(state, registers) || !idle(registers)) {
        fail(state, TRIM_I2C_CONFIG);
      } else {
        state->phase = state->possible_programming ? CLEANUP_PROGRAM : CLEANUP_PROBE;
      }
      break;
    case CLEANUP_PROGRAM:
      if (now - state->released_at >= state->profile.write_ticks) {
        state->phase = CLEANUP_PROBE;
      }
      break;
    case CLEANUP_PROBE:
      if (!owned_configuration(state, registers) || !idle(registers)) {
        fail(state, TRIM_I2C_NOT_IDLE);
      } else {
        state->berr_seen = 0;
        start(state, AUTOEND, CLEANUP_PROBE_WAIT, false, TXE);
      }
      break;
    case CLEANUP_PROBE_WAIT:
      if (!owned_configuration(state, registers)) {
        fail(state, TRIM_I2C_CONFIG);
      } else if (transfer_status(state, registers, now, false) &&
                 stopped(state, registers, false)) {
        if (state->berr_seen) {
          fail(state, TRIM_I2C_PROBE_BERR);
        } else {
          state->possible_programming = 0;
          state->phase = RESTORE_DISABLE;
        }
      }
      break;
    case RESTORE_DISABLE:
      if (disable(state)) {
        state->phase = RESTORE_SETUP;
      }
      break;
    case RESTORE_SETUP:
      if (registers->cr1 != 0 || (registers->cr2 & (START | STOP))) {
        fail(state, TRIM_I2C_RESET_FAILED);
      } else {
        write_register(state, TRIM_I2C_CR2, state->saved[0]);
        write_register(state, TRIM_I2C_OAR1, state->saved[1]);
        write_register(state, TRIM_I2C_OAR2, state->saved[2]);
        write_register(state, TRIM_I2C_TIMINGR, state->saved[3]);
        write_register(state, TRIM_I2C_TIMEOUTR, state->saved[4]);
        write_register(state, TRIM_I2C_CR1, PE);
        state->phase = RESTORE_VERIFY;
      }
      break;
    case RESTORE_VERIFY:
      if (registers->cr1 != PE || registers->cr2 != state->saved[0] ||
          registers->oar1 != state->saved[1] || registers->oar2 != state->saved[2] ||
          registers->timing != state->saved[3] || registers->timeout != state->saved[4] ||
          !idle(registers)) {
        fail(state, TRIM_I2C_CONFIG);
      } else {
        state->owned = 0;
        state->released = 1;
        finish(state, TRIM_I2C_OK, TRIM_I2C_NO_FAULT);
      }
      break;
    case CLEANUP_ABORT:
      write_register(state, TRIM_I2C_CR1, 0);
      finish(state, TRIM_I2C_UNRELEASED, state->fault);
      break;
    default:
      fail(state, TRIM_I2C_SEQUENCE);
      break;
  }
}

void trim_i2c_poll(TrimI2cState* state) {
  if (trim_i2c_closed(state)) {
    return;
  }
  Registers registers;
  uint32_t now = snapshot(state, &registers);
  if (!on_time(state, now)) {
    return;
  }
  if (state->action == TRIM_I2C_CLEANUP) {
    cleanup_poll(state, &registers, now);
    return;
  }
  if (state->action != TRIM_I2C_ACQUIRE) {
    byte_poll(state, &registers, now);
    return;
  }
  switch (state->phase) {
    case ACQUIRE_CHECK:
      if (registers.cr1 != PE || (registers.cr2 & CR2_FORBIDDEN) || registers.timeout != 0) {
        fail(state, TRIM_I2C_CONFIG);
      } else if (!idle(&registers)) {
        fail(state, TRIM_I2C_NOT_IDLE);
      } else {
        state->saved[0] = registers.cr2;
        state->saved[1] = registers.oar1;
        state->saved[2] = registers.oar2;
        state->saved[3] = registers.timing;
        state->saved[4] = registers.timeout;
        state->have_saved = 1;
        state->phase = ACQUIRE_DISABLE;
      }
      break;
    case ACQUIRE_DISABLE:
      if (disable(state)) {
        state->phase = ACQUIRE_SETUP;
      }
      break;
    case ACQUIRE_SETUP:
      if (registers.cr1 != 0 || (registers.cr2 & (START | STOP))) {
        fail(state, TRIM_I2C_RESET_FAILED);
      } else {
        setup_owned(state);
        state->phase = ACQUIRE_VERIFY;
      }
      break;
    case ACQUIRE_VERIFY:
      if (!owned_configuration(state, &registers) || !idle(&registers)) {
        fail(state, TRIM_I2C_CONFIG);
      } else {
        state->owned = 1;
        finish(state, TRIM_I2C_OK, TRIM_I2C_NO_FAULT);
      }
      break;
    default:
      fail(state, TRIM_I2C_SEQUENCE);
      break;
  }
}

uint32_t trim_i2c_mmio_read(void* context, uint8_t offset) {
  (void)context;
  return *(volatile uint32_t*)(uintptr_t)(0x40005400u + offset);
}

void trim_i2c_mmio_write(void* context, uint8_t offset, uint32_t value) {
  (void)context;
  *(volatile uint32_t*)(uintptr_t)(0x40005400u + offset) = value;
}

bool trim_i2c_mmio_lines_high(void* context) {
  (void)context;
  return (*(volatile uint32_t*)(uintptr_t)0x48000410u & 0x300u) == 0x300u;
}
