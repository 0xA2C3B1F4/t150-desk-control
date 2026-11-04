#include "motion_trim_i2c.h"
#include "motion_trim_layout.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

enum {
  TXE = 1,
  TXIS = 2,
  RXNE = 4,
  NACK = 0x10,
  STOPF = 0x20,
  TC = 0x40,
  BERR = 0x100,
  BUSY = 0x8000,
  START = 0x2000,
  STOP = 0x4000,
  AUTOEND = 0x2000000
};

typedef struct {
  uint8_t offset;
  uint32_t value;
} Write;
typedef struct {
  uint32_t registers[11];
  uint32_t now;
  uint32_t tick_on_status;
  uint32_t stuck_clear;
  uint32_t stuck_cr2;
  uint32_t status_after_clear;
  uint32_t complete_on_cr2_read;
  uint32_t advance_on_disable_read;
  unsigned accesses;
  unsigned clocks;
  unsigned rx_reads;
  unsigned count;
  bool lines;
  bool start_clears_on_read;
  bool ignore_disable;
  bool override_after_clear;
  Write writes[512];
} Fake;

typedef struct {
  Fake fake;
  TrimI2cState driver;
} Fixture;

static uint32_t read_fake(void* context, uint8_t offset) {
  Fake* fake = context;
  ++fake->accesses;
  uint32_t result = fake->registers[offset / 4];
  if (offset == TRIM_I2C_CR1 && result == 0) {
    fake->now += fake->advance_on_disable_read;
    fake->advance_on_disable_read = 0;
  }
  if (offset == TRIM_I2C_ISR) {
    fake->now += fake->tick_on_status;
  }
  if (offset == TRIM_I2C_RXDR) {
    ++fake->rx_reads;
    fake->registers[TRIM_I2C_ISR / 4] &= ~RXNE;
  }
  if (offset == TRIM_I2C_CR2 && fake->start_clears_on_read) {
    fake->registers[TRIM_I2C_CR2 / 4] &= ~START;
  }
  if (offset == TRIM_I2C_CR2 && fake->complete_on_cr2_read) {
    fake->registers[TRIM_I2C_CR2 / 4] &= ~(START | STOP);
    fake->registers[TRIM_I2C_ISR / 4] = fake->complete_on_cr2_read;
    fake->complete_on_cr2_read = 0;
  }
  return result;
}

/* PE reset and write-one-to-clear are modeled; transfer events are scripted.
 * No test relies on an automatic STOP after NACK. Missing STOP is adversarial.
 * This port does not model EEPROM power loss, pin waveforms or clock timing. */
static void write_fake(void* context, uint8_t offset, uint32_t value) {
  Fake* fake = context;
  ++fake->accesses;
  assert(fake->count < sizeof(fake->writes) / sizeof(fake->writes[0]));
  fake->writes[fake->count++] = (Write){offset, value};
  if (offset == TRIM_I2C_ICR) {
    fake->registers[TRIM_I2C_ISR / 4] &= ~(value & ~fake->stuck_clear);
    if (fake->override_after_clear) {
      fake->registers[TRIM_I2C_ISR / 4] = fake->status_after_clear;
    }
    return;
  }
  if (offset == TRIM_I2C_CR1 && value == 0 && fake->ignore_disable) {
    return;
  }
  fake->registers[offset / 4] = value;
  if (offset == TRIM_I2C_CR1 && value == 0) {
    fake->registers[TRIM_I2C_ISR / 4] = TXE;
    fake->registers[TRIM_I2C_CR2 / 4] &= ~(START | STOP | 0x8000u);
    fake->registers[TRIM_I2C_CR2 / 4] |= fake->stuck_cr2;
  }
  if (offset == TRIM_I2C_TXDR) {
    fake->registers[TRIM_I2C_ISR / 4] &= ~(TXE | TXIS);
  }
  if (offset == TRIM_I2C_CR2 && (value & START)) {
    fake->registers[TRIM_I2C_ISR / 4] &= ~TC;
    fake->registers[TRIM_I2C_ISR / 4] |= BUSY;
  }
}

static bool lines_fake(void* context) {
  Fake* fake = context;
  ++fake->accesses;
  return fake->lines;
}

static uint32_t clock_fake(void* context) {
  Fake* fake = context;
  ++fake->clocks;
  return fake->now;
}

static TrimI2cProfile profile(void) {
  /* Synthetic ticks and timing; deliberately not a qualified device profile. */
  return (TrimI2cProfile){.timing = 0x10420F13,
                          .transfer_ticks = 20,
                          .write_ticks = 7,
                          .poll_ticks = 3,
                          .stop_ticks = 5,
                          .max_poll_gap = 1,
                          .address = 0x50};
}

static void init(Fixture* fixture, uint32_t now) {
  memset(fixture, 0, sizeof(*fixture));
  Fake* fake = &fixture->fake;
  fake->now = now;
  fake->lines = true;
  fake->registers[TRIM_I2C_CR1 / 4] = 1;
  fake->registers[TRIM_I2C_CR2 / 4] = AUTOEND | (2u << 16) | 0xA0;
  fake->registers[TRIM_I2C_OAR1 / 4] = 0x8000;
  fake->registers[TRIM_I2C_TIMINGR / 4] = 0x00210507;
  fake->registers[TRIM_I2C_ISR / 4] = TXE;
  TrimI2cPort port = {read_fake, write_fake, lines_fake, clock_fake, fake};
  TrimI2cProfile part = profile();
  assert(trim_i2c_init(&fixture->driver, &port, &part));
  assert(fake->accesses == 0 && fake->clocks == 0);
}

static void poll(Fixture* fixture) {
  unsigned before = fixture->fake.accesses;
  trim_i2c_poll(&fixture->driver);
  assert(fixture->fake.accesses - before <= 24);
}

static void event(Fixture* fixture, uint32_t flags) {
  fixture->fake.registers[TRIM_I2C_CR2 / 4] &= ~(START | STOP);
  fixture->fake.registers[TRIM_I2C_ISR / 4] = flags;
}

static unsigned writes_to(const Fake* fake, uint8_t offset) {
  unsigned count = 0;
  for (unsigned i = 0; i < fake->count; ++i) {
    count += fake->writes[i].offset == offset;
  }
  return count;
}

static unsigned starts(const Fake* fake) {
  unsigned count = 0;
  for (unsigned i = 0; i < fake->count; ++i) {
    count += fake->writes[i].offset == TRIM_I2C_CR2 && (fake->writes[i].value & START) != 0;
  }
  return count;
}

static void acquire(Fixture* fixture) {
  assert(trim_i2c_acquire(&fixture->driver, fixture->fake.now + 1000));
  for (unsigned i = 0; i < 4; ++i) {
    poll(fixture);
  }
  assert(fixture->driver.result == TRIM_I2C_OK && fixture->driver.owned);
  assert(!fixture->driver.released && fixture->fake.count == 7);
  assert(fixture->fake.writes[0].offset == TRIM_I2C_CR1 && fixture->fake.writes[0].value == 0);
  assert(fixture->fake.writes[6].offset == TRIM_I2C_CR1 && fixture->fake.writes[6].value == 1);
}

static void begin_read(Fixture* fixture) {
  assert(trim_i2c_read(&fixture->driver, 0x62, fixture->fake.now + 1000));
  poll(fixture);
  assert(fixture->driver.result == TRIM_I2C_PENDING);
  assert(fixture->fake.registers[TRIM_I2C_TXDR / 4] == 0x62);
  assert(fixture->fake.registers[TRIM_I2C_CR2 / 4] == (0xA0 | START | (1u << 16)));
}

static void begin_write(Fixture* fixture) {
  assert(trim_i2c_write(&fixture->driver, 0x63, 0xA5, fixture->fake.now + 1000));
  poll(fixture);
  assert(fixture->fake.registers[TRIM_I2C_CR2 / 4] == (0xA0 | START | AUTOEND | (2u << 16)));
  event(fixture, TXE | TXIS | BUSY);
  poll(fixture);
  assert(fixture->driver.possible_programming);
  assert(fixture->fake.registers[TRIM_I2C_TXDR / 4] == 0xA5);
}

static void read_success(Fixture* fixture, bool berr) {
  begin_read(fixture);
  event(fixture, TXE | TC | BUSY | (berr ? BERR : 0));
  poll(fixture);
  assert(fixture->fake.registers[TRIM_I2C_CR2 / 4] ==
         (0xA0 | START | AUTOEND | 0x400 | (1u << 16)));
  fixture->fake.registers[TRIM_I2C_RXDR / 4] = 0x71;
  event(fixture, TXE | RXNE | STOPF);
  poll(fixture);
  assert(fixture->driver.result == TRIM_I2C_PENDING);
  poll(fixture);
  assert(fixture->driver.result == TRIM_I2C_OK);
  assert(fixture->driver.value == 0x71 && fixture->fake.rx_reads == 1);
  assert(fixture->driver.operation_berr == berr);
  assert(fixture->fake.registers[TRIM_I2C_ISR / 4] == TXE);
}

/* Drive only the certification probe's external completion. Other transitions
 * must follow from the driver's register writes and explicit time progression. */
static void cleanup_success(Fixture* fixture) {
  TrimI2cState* driver = &fixture->driver;
  uint8_t previous_berr = driver->operation_berr;
  unsigned initial_starts = starts(&fixture->fake);
  unsigned initial_payloads = writes_to(&fixture->fake, TRIM_I2C_TXDR);
  assert(trim_i2c_cleanup(driver, fixture->fake.now + 1000));
  for (unsigned i = 0; i < 80 && !trim_i2c_closed(driver); ++i) {
    if (starts(&fixture->fake) > initial_starts) {
      assert(fixture->fake.registers[TRIM_I2C_CR2 / 4] == (0xA0 | START | AUTOEND));
      event(fixture, TXE | STOPF);
      initial_starts = UINT_MAX;
    }
    poll(fixture);
    ++fixture->fake.now;
  }
  assert(driver->result == TRIM_I2C_OK && driver->released && !driver->owned);
  assert(driver->operation_berr == previous_berr);
  assert(fixture->fake.registers[TRIM_I2C_CR2 / 4] == (AUTOEND | (2u << 16) | 0xA0));
  assert(fixture->fake.registers[TRIM_I2C_OAR1 / 4] == 0x8000);
  assert(fixture->fake.registers[TRIM_I2C_TIMINGR / 4] == 0x00210507);
  assert(fixture->fake.registers[TRIM_I2C_ISR / 4] == TXE);
  assert(writes_to(&fixture->fake, TRIM_I2C_TXDR) == initial_payloads);
}

static void assert_closed(Fixture* fixture) {
  unsigned accesses = fixture->fake.accesses;
  unsigned clocks = fixture->fake.clocks;
  uint8_t result = fixture->driver.result;
  fixture->fake.now += 1000000;
  for (unsigned i = 0; i < 3; ++i) {
    poll(fixture);
  }
  assert(fixture->fake.accesses == accesses && fixture->fake.clocks == clocks);
  assert(fixture->driver.result == result);
  if (result == TRIM_I2C_UNRELEASED) {
    assert(!trim_i2c_acquire(&fixture->driver, fixture->fake.now + 1000));
    assert(!trim_i2c_read(&fixture->driver, 0, fixture->fake.now + 1000));
    assert(!trim_i2c_write(&fixture->driver, 0, 0, fixture->fake.now + 1000));
    assert(!trim_i2c_cleanup(&fixture->driver, fixture->fake.now + 1000));
    assert(fixture->fake.accesses == accesses && fixture->fake.clocks == clocks);
  }
}

static void test_bounds(void) {
  TrimI2cProfile part = profile();
  TrimI2cBounds bounds;
  assert(trim_i2c_bounds(&part, &bounds));
  assert(bounds.acquire == 4 && bounds.read == 48 && bounds.write == 86);
  assert(bounds.cleanup == 48 && bounds.ready == 58);
  uint32_t* fields[] = {&part.transfer_ticks, &part.write_ticks, &part.poll_ticks, &part.stop_ticks,
                        &part.max_poll_gap};
  for (unsigned i = 0; i < 5; ++i) {
    uint32_t saved = *fields[i];
    *fields[i] = 0;
    assert(!trim_i2c_bounds(&part, &bounds));
    *fields[i] = 0x80000000u;
    assert(!trim_i2c_bounds(&part, &bounds));
    *fields[i] = 0x7FFFFFFFu;
    assert(!trim_i2c_bounds(&part, &bounds));
    *fields[i] = saved;
  }
  part.address = 0;
  assert(!trim_i2c_bounds(&part, &bounds));
  part = profile();
  part.timing |= 0x01000000;
  assert(!trim_i2c_bounds(&part, &bounds));

  Fixture fixture;
  init(&fixture, 0xFFFFFFF0u);
  assert(!trim_i2c_acquire(&fixture.driver, fixture.fake.now));
  assert(!trim_i2c_acquire(&fixture.driver, fixture.fake.now - 1));
  assert(!trim_i2c_acquire(&fixture.driver, fixture.fake.now + 0x80000000u));
  assert(!trim_i2c_acquire(&fixture.driver, fixture.fake.now + bounds.acquire));
  acquire(&fixture);
  unsigned writes = fixture.fake.count;
  assert(!trim_i2c_write(&fixture.driver, 0, 0, fixture.fake.now + bounds.write));
  assert(fixture.fake.count == writes);
  read_success(&fixture, false);
  cleanup_success(&fixture);
}

static void test_success_and_reuse(void) {
  Fixture fixture;
  init(&fixture, 0);
  acquire(&fixture);
  read_success(&fixture, false);
  begin_write(&fixture);
  event(&fixture, TXE | STOPF);
  poll(&fixture);
  assert(fixture.driver.result == TRIM_I2C_PENDING && fixture.driver.possible_programming);
  poll(&fixture);
  assert(fixture.fake.registers[TRIM_I2C_CR2 / 4] == (0xA0 | START | AUTOEND));
  event(&fixture, TXE | NACK | STOPF);
  poll(&fixture);
  assert(fixture.driver.result == TRIM_I2C_PENDING && fixture.driver.possible_programming);
  unsigned starts_before = starts(&fixture.fake);
  poll(&fixture);
  assert(starts(&fixture.fake) == starts_before);
  fixture.fake.now += 3;
  poll(&fixture);
  poll(&fixture);
  event(&fixture, TXE | STOPF);
  poll(&fixture);
  assert(fixture.driver.result == TRIM_I2C_OK && !fixture.driver.possible_programming);
  assert(writes_to(&fixture.fake, TRIM_I2C_TXDR) == 3); /* Read address, write address, payload. */
  cleanup_success(&fixture);
  assert(trim_i2c_acquire(&fixture.driver, fixture.fake.now + 1000));
  for (unsigned i = 0; i < 4; ++i) {
    poll(&fixture);
  }
  assert(fixture.driver.result == TRIM_I2C_OK);
  assert_closed(&fixture);
}

static void test_admission(void) {
  const uint32_t flags[] = {0,          TXE | RXNE, TXE | STOPF, TXE | NACK,
                            TXE | BUSY, TXE | TC,   TXE | BERR,  TXE | TXIS};
  for (unsigned i = 0; i < sizeof(flags) / sizeof(flags[0]); ++i) {
    Fixture fixture;
    init(&fixture, 0);
    fixture.fake.registers[TRIM_I2C_ISR / 4] = flags[i];
    assert(trim_i2c_acquire(&fixture.driver, 1000));
    poll(&fixture);
    assert(fixture.driver.result == TRIM_I2C_FAILED && fixture.fake.count == 0);
    assert(fixture.fake.rx_reads == 0);
  }
  const uint32_t cr2_flags[] = {START, STOP, 0x800, 0x1000, 0x8000, 0x1000000, 0x4000000};
  for (unsigned i = 0; i < sizeof(cr2_flags) / sizeof(cr2_flags[0]); ++i) {
    Fixture fixture;
    init(&fixture, 0);
    fixture.fake.registers[TRIM_I2C_CR2 / 4] |= cr2_flags[i];
    assert(trim_i2c_acquire(&fixture.driver, 1000));
    poll(&fixture);
    assert(fixture.driver.result == TRIM_I2C_FAILED && fixture.fake.count == 0);
  }
  for (unsigned bit = 1; bit < 24; ++bit) {
    Fixture fixture;
    init(&fixture, 0);
    fixture.fake.registers[TRIM_I2C_CR1 / 4] |= 1u << bit;
    assert(trim_i2c_acquire(&fixture.driver, 1000));
    poll(&fixture);
    assert(fixture.driver.result == TRIM_I2C_FAILED && fixture.fake.count == 0);
  }
}

static void test_read_errors(void) {
  const uint32_t errors[] = {NACK, 8, 0x80, 0x200, 0x400, 0x800, 0x1000, 0x2000};
  for (unsigned stage = 0; stage < 3; ++stage) {
    for (unsigned i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
      Fixture fixture;
      init(&fixture, 0);
      acquire(&fixture);
      begin_read(&fixture);
      if (stage >= 1) {
        event(&fixture, TXE | TC | BUSY);
        poll(&fixture);
      }
      if (stage >= 2) {
        fixture.fake.registers[TRIM_I2C_RXDR / 4] = 0x71;
        event(&fixture, TXE | RXNE | BUSY);
        poll(&fixture);
      }
      event(&fixture, TXE | STOPF | errors[i]);
      poll(&fixture);
      assert(fixture.driver.result == TRIM_I2C_FAILED);
      assert(fixture.fake.rx_reads == (stage >= 2));
      cleanup_success(&fixture);
    }
  }
  Fixture fixture;
  init(&fixture, 0);
  acquire(&fixture);
  read_success(&fixture, true);
  assert(fixture.driver.operation_berr);
  cleanup_success(&fixture);
}

static void test_write_failures(void) {
  for (unsigned stage = 0; stage < 4; ++stage) {
    Fixture fixture;
    init(&fixture, 0);
    acquire(&fixture);
    assert(trim_i2c_write(&fixture.driver, 0x63, 0xA5, 1000));
    poll(&fixture);
    if (stage >= 1) {
      event(&fixture, TXE | TXIS | BUSY);
      poll(&fixture);
    }
    if (stage >= 2) {
      event(&fixture, TXE | STOPF);
      poll(&fixture);
    }
    if (stage >= 3) {
      poll(&fixture);
    }
    event(&fixture, TXE | NACK | STOPF);
    poll(&fixture);
    if (stage == 3) {
      /* Ready NACK remains pending, then the absolute ready bound expires. */
      assert(fixture.driver.result == TRIM_I2C_PENDING);
      fixture.fake.now = fixture.driver.ready_started + fixture.driver.bounds.ready;
      poll(&fixture);
    }
    assert(fixture.driver.result == TRIM_I2C_FAILED);
    assert(fixture.driver.possible_programming == (stage != 0));
    unsigned tx_writes = writes_to(&fixture.fake, TRIM_I2C_TXDR);
    assert(tx_writes == (stage == 0 ? 1u : 2u));
    cleanup_success(&fixture);
  }
}

static void test_deadlines(void) {
  for (unsigned wrap = 0; wrap < 2; ++wrap) {
    for (unsigned equality = 0; equality < 2; ++equality) {
      Fixture fixture;
      init(&fixture, wrap ? UINT32_MAX - 10 : 0);
      acquire(&fixture);
      begin_read(&fixture);
      event(&fixture, TXE | TC | BUSY);
      fixture.fake.now += 19 + equality;
      poll(&fixture);
      assert(fixture.driver.result == (equality ? TRIM_I2C_FAILED : TRIM_I2C_PENDING));
      if (equality) {
        assert(fixture.driver.fault == TRIM_I2C_TRANSFER_TIMEOUT);
        cleanup_success(&fixture);
      }
    }
  }
  Fixture fixture;
  init(&fixture, 0);
  acquire(&fixture);
  begin_write(&fixture);
  event(&fixture, TXE | STOPF);
  fixture.fake.now = 999;
  fixture.fake.tick_on_status = 1;
  poll(&fixture);
  assert(fixture.driver.result == TRIM_I2C_FAILED && fixture.driver.fault == TRIM_I2C_DEADLINE);
  fixture.fake.tick_on_status = 0;
  cleanup_success(&fixture);
  assert_closed(&fixture);
}

static void test_stale_and_reset(void) {
  Fixture fixture;
  init(&fixture, 0);
  acquire(&fixture);
  begin_write(&fixture);
  event(&fixture, TXE | STOPF);
  fixture.fake.stuck_clear = STOPF;
  poll(&fixture);
  assert(fixture.driver.result == TRIM_I2C_FAILED && fixture.driver.possible_programming);
  assert(starts(&fixture.fake) == 1);
  fixture.fake.stuck_clear = 0;
  cleanup_success(&fixture);

  for (unsigned i = 0; i < 2; ++i) {
    init(&fixture, 0);
    assert(trim_i2c_acquire(&fixture.driver, 1000));
    poll(&fixture);
    fixture.fake.stuck_cr2 = i ? STOP : START;
    poll(&fixture);
    assert(fixture.driver.result == TRIM_I2C_FAILED);
    assert(writes_to(&fixture.fake, TRIM_I2C_CR1) == 1);
    assert(fixture.fake.registers[TRIM_I2C_CR1 / 4] == 0);
  }
  init(&fixture, 0);
  acquire(&fixture);
  begin_read(&fixture);
  event(&fixture, NACK | STOPF); /* The preloaded address remains occupied. */
  poll(&fixture);
  assert(fixture.driver.result == TRIM_I2C_FAILED);
  unsigned before = fixture.fake.count;
  assert(!trim_i2c_write(&fixture.driver, 0x70, 0xBB, 1000));
  assert(fixture.fake.count == before);
  cleanup_success(&fixture);
}

static void test_cleanup_failures(void) {
  for (unsigned kind = 0; kind < 5; ++kind) {
    Fixture fixture;
    init(&fixture, 0);
    acquire(&fixture);
    begin_write(&fixture);
    fixture.fake.now = 20;
    poll(&fixture);
    assert(fixture.driver.result == TRIM_I2C_FAILED);
    unsigned count = starts(&fixture.fake);
    assert(trim_i2c_cleanup(&fixture.driver, 1020));
    for (unsigned i = 0; i < 80 && !trim_i2c_closed(&fixture.driver); ++i) {
      if (kind == 0) {
        fixture.fake.lines = false;
      }
      if (starts(&fixture.fake) > count) {
        if (kind == 1) {
          event(&fixture, TXE | STOPF | NACK);
        } else if (kind == 2) {
          event(&fixture, TXE | STOPF | BERR);
        } else if (kind == 3) {
          event(&fixture, TXE | STOPF);
          fixture.fake.ignore_disable = true;
        } else if (kind == 4) {
          fixture.fake.now = 1020; /* No post-deadline disable is allowed. */
        }
        count = UINT_MAX;
      }
      poll(&fixture);
      ++fixture.fake.now;
    }
    assert(fixture.driver.result == TRIM_I2C_UNRELEASED && !fixture.driver.released);
    assert_closed(&fixture);
  }
}

static void test_stop_policy(void) {
  for (unsigned kind = 0; kind < 3; ++kind) {
    Fixture fixture;
    init(&fixture, 0);
    acquire(&fixture);
    begin_read(&fixture);
    fixture.fake.now = 20;
    if (kind == 0) {
      event(&fixture, TXE | TC | BUSY);
    } else if (kind == 1) {
      event(&fixture, TXE | TXIS | BUSY);
    } else {
      fixture.fake.start_clears_on_read = true;
    }
    poll(&fixture);
    assert(fixture.driver.result == TRIM_I2C_FAILED);
    unsigned before = fixture.fake.count;
    assert(trim_i2c_cleanup(&fixture.driver, 1020));
    poll(&fixture);
    if (kind == 0) {
      assert(fixture.fake.count == before + 1);
      Write last = fixture.fake.writes[before];
      assert(last.offset == TRIM_I2C_CR2 && (last.value & STOP) && !(last.value & START));
      fixture.fake.now += 5;
      poll(&fixture); /* Grace expiry advances to reset. */
    } else {
      assert(fixture.fake.count == before);
    }
    poll(&fixture);
    assert(fixture.fake.registers[TRIM_I2C_CR1 / 4] == 0);
  }
  Fixture fixture;
  init(&fixture, 0);
  acquire(&fixture);
  assert(trim_i2c_write(&fixture.driver, 0x63, 0xA5, 1000));
  poll(&fixture);
  event(&fixture, TXE | TXIS | BUSY);
  fixture.fake.now = 20;
  poll(&fixture);
  assert(fixture.driver.result == TRIM_I2C_FAILED && !fixture.driver.possible_programming);
  unsigned before = fixture.fake.count;
  assert(trim_i2c_cleanup(&fixture.driver, 1020));
  poll(&fixture);
  assert(fixture.fake.count == before); /* AUTOEND TXIS stall also gets no software STOP. */
  poll(&fixture);
  assert(fixture.fake.registers[TRIM_I2C_CR1 / 4] == 0);

  init(&fixture, 0);
  acquire(&fixture);
  begin_read(&fixture);
  fixture.fake.now = 20;
  poll(&fixture);
  fixture.fake.registers[TRIM_I2C_ISR / 4] = TXE | TC | BUSY;
  fixture.fake.registers[TRIM_I2C_CR2 / 4] |= START;
  fixture.fake.start_clears_on_read = true;
  before = fixture.fake.count;
  assert(trim_i2c_cleanup(&fixture.driver, 1020));
  poll(&fixture);
  assert(fixture.fake.count ==
         before); /* Sampled START must not be replayed after hardware clears it. */
}

/* Each setup stops at a different externally observable transfer boundary. */
static void boundary(Fixture* fixture, unsigned stage) {
  init(fixture, 0);
  acquire(fixture);
  if (stage < 4) {
    assert(trim_i2c_read(&fixture->driver, 0x62, 1000));
    if (stage >= 1) {
      poll(fixture);
    }
    if (stage >= 2) {
      event(fixture, TXE | TC | BUSY);
      poll(fixture);
    }
    if (stage >= 3) {
      event(fixture, TXE | RXNE | BUSY);
      poll(fixture);
    }
  } else {
    assert(trim_i2c_write(&fixture->driver, 0x63, 0xA5, 1000));
    poll(fixture);
    if (stage >= 5) {
      event(fixture, TXE | TXIS | BUSY);
      poll(fixture);
    }
    if (stage >= 6) {
      event(fixture, TXE | STOPF);
      poll(fixture);
    }
    if (stage >= 7) {
      poll(fixture);
    }
    if (stage >= 8) {
      event(fixture, TXE | NACK | STOPF);
      poll(fixture);
    }
  }
  assert(fixture->driver.result == TRIM_I2C_PENDING);
}

static void test_transfer_matrix(void) {
  const uint32_t faults[] = {8, 0x80, 0x200, 0x400, 0x800, 0x1000, 0x2000};
  for (unsigned stage = 0; stage < 9; ++stage) {
    for (unsigned i = 0; i < sizeof(faults) / sizeof(faults[0]); ++i) {
      Fixture fixture;
      boundary(&fixture, stage);
      unsigned tx_count = writes_to(&fixture.fake, TRIM_I2C_TXDR);
      event(&fixture, TXE | STOPF | faults[i]);
      poll(&fixture);
      assert(fixture.driver.result == TRIM_I2C_FAILED);
      assert(writes_to(&fixture.fake, TRIM_I2C_TXDR) == tx_count);
      cleanup_success(&fixture);
      assert_closed(&fixture);
    }
  }
  const unsigned waits[] = {1, 2, 3, 4, 5, 7};
  for (unsigned i = 0; i < sizeof(waits) / sizeof(waits[0]); ++i) {
    Fixture fixture;
    boundary(&fixture, waits[i]);
    event(&fixture, TXE | BUSY | (waits[i] == 7 ? NACK : 0));
    fixture.fake.now = fixture.driver.phase_started + 19;
    poll(&fixture);
    assert(fixture.driver.result == TRIM_I2C_PENDING);
    ++fixture.fake.now;
    poll(&fixture);
    assert(fixture.driver.result == TRIM_I2C_FAILED);
    assert(fixture.driver.fault == TRIM_I2C_TRANSFER_TIMEOUT);
    if (waits[i] == 7) {
      assert(fixture.driver.possible_programming);
      unsigned starts_before = starts(&fixture.fake);
      poll(&fixture);
      assert(starts(&fixture.fake) == starts_before);
    }
    cleanup_success(&fixture);
  }
  Fixture fixture;
  boundary(&fixture, 1);
  /* START and an occupied TXDR can persist while the address is on the wire. */
  poll(&fixture);
  assert(fixture.driver.result == TRIM_I2C_PENDING);
  fixture.fake.now = 20;
  poll(&fixture);
  assert(fixture.driver.result == TRIM_I2C_FAILED);
  cleanup_success(&fixture);
}

static void test_post_observation_deadlines(void) {
  Fixture fixture;
  boundary(&fixture, 3);
  event(&fixture, TXE | STOPF);
  fixture.fake.now = 18;
  fixture.fake.tick_on_status = 1;
  poll(&fixture); /* STOP is timely, but its clear/readback reaches the phase limit. */
  assert(fixture.driver.result == TRIM_I2C_FAILED);
  assert(fixture.driver.fault == TRIM_I2C_TRANSFER_TIMEOUT);

  boundary(&fixture, 1);
  event(&fixture, TXE | TC | BUSY);
  fixture.fake.now = 18;
  fixture.fake.tick_on_status = 1;
  unsigned start_count = starts(&fixture.fake);
  poll(&fixture);
  assert(fixture.driver.result == TRIM_I2C_FAILED);
  assert(starts(&fixture.fake) == start_count);
}

static void test_probe_and_restoration_boundaries(void) {
  Fixture fixture;
  boundary(&fixture, 5);
  fixture.fake.now = 20;
  poll(&fixture);
  assert(trim_i2c_cleanup(&fixture.driver, 1020));
  unsigned initial_starts = starts(&fixture.fake);
  unsigned initial_writes = fixture.fake.count;
  uint32_t release_stamp = 0;
  bool saw_disable = false;
  for (unsigned i = 0; i < 80 && starts(&fixture.fake) == initial_starts; ++i) {
    poll(&fixture);
    for (unsigned j = initial_writes; j < fixture.fake.count; ++j) {
      if (fixture.fake.writes[j].offset == TRIM_I2C_CR1 && fixture.fake.writes[j].value == 0) {
        release_stamp = fixture.fake.now;
        saw_disable = true;
      }
    }
    initial_writes = fixture.fake.count;
    if (starts(&fixture.fake) != initial_starts) {
      assert(saw_disable && fixture.fake.now - release_stamp >= 7);
      break;
    }
    ++fixture.fake.now;
  }
  assert(starts(&fixture.fake) == initial_starts + 1);
  event(&fixture, TXE | NACK | BUSY); /* No automatic STOP is supplied. */
  poll(&fixture);
  assert(fixture.driver.result == TRIM_I2C_PENDING);
  poll(&fixture); /* Reserved disable after failed certification. */
  assert(fixture.driver.result == TRIM_I2C_UNRELEASED);
  assert(fixture.driver.possible_programming && !fixture.driver.released);
  assert(fixture.fake.registers[TRIM_I2C_CR1 / 4] == 0);

  for (unsigned field = 0; field < 6; ++field) {
    init(&fixture, 0);
    acquire(&fixture);
    assert(trim_i2c_cleanup(&fixture.driver, 1000));
    unsigned start_count = starts(&fixture.fake);
    bool restored = false;
    for (unsigned i = 0; i < 40 && !trim_i2c_closed(&fixture.driver); ++i) {
      if (starts(&fixture.fake) > start_count) {
        event(&fixture, TXE | STOPF);
        start_count = UINT_MAX;
      }
      if (fixture.fake.registers[TRIM_I2C_OAR1 / 4] == 0x8000 &&
          fixture.fake.registers[TRIM_I2C_TIMINGR / 4] == 0x00210507 && i > 4) {
        const uint8_t offsets[] = {TRIM_I2C_CR1,  TRIM_I2C_CR2,     TRIM_I2C_OAR1,
                                   TRIM_I2C_OAR2, TRIM_I2C_TIMINGR, TRIM_I2C_TIMEOUTR};
        fixture.fake.registers[offsets[field] / 4] ^= 1;
        restored = true;
      }
      poll(&fixture);
      ++fixture.fake.now;
    }
    assert(restored && fixture.driver.result == TRIM_I2C_UNRELEASED);
    assert_closed(&fixture);
  }
}

static void test_exported_budgets(void) {
  Fixture fixture;
  init(&fixture, 0);
  acquire(&fixture);
  assert(trim_i2c_write(&fixture.driver, 0x63, 0xA5, fixture.driver.bounds.write + 1));
  poll(&fixture);
  fixture.fake.now = 18;
  event(&fixture, TXE | TXIS | BUSY);
  poll(&fixture);
  fixture.fake.now = 19;
  event(&fixture, TXE | STOPF);
  poll(&fixture);
  fixture.fake.now = 20;
  poll(&fixture);
  fixture.fake.now = 39;
  event(&fixture, TXE | NACK | STOPF);
  poll(&fixture);
  for (unsigned tick = 40; tick <= 43; ++tick) {
    fixture.fake.now = tick;
    poll(&fixture);
  }
  assert(fixture.fake.registers[TRIM_I2C_CR2 / 4] & START);
  fixture.fake.now = 62;
  event(&fixture, TXE | STOPF);
  poll(&fixture);
  assert(fixture.driver.result == TRIM_I2C_OK);

  init(&fixture, UINT32_MAX - 10);
  acquire(&fixture);
  begin_write(&fixture);
  fixture.fake.now += 20;
  poll(&fixture);
  uint32_t entered = fixture.fake.now;
  uint32_t deadline = entered + fixture.driver.bounds.cleanup + 1;
  unsigned initial_starts = starts(&fixture.fake);
  unsigned initial_tx = writes_to(&fixture.fake, TRIM_I2C_TXDR);
  bool probe_started = false;
  uint32_t probe_at = 0;
  assert(trim_i2c_cleanup(&fixture.driver, deadline));
  for (unsigned i = 0; i <= fixture.driver.bounds.cleanup && !trim_i2c_closed(&fixture.driver);
       ++i) {
    if (probe_started && fixture.fake.now - probe_at == 19) {
      event(&fixture, TXE | STOPF);
    }
    poll(&fixture);
    if (!probe_started && starts(&fixture.fake) > initial_starts) {
      probe_started = true;
      probe_at = fixture.fake.now;
    }
    ++fixture.fake.now;
  }
  assert(probe_started && fixture.driver.result == TRIM_I2C_OK && fixture.driver.released);
  assert(fixture.fake.now - entered <= fixture.driver.bounds.cleanup);
  assert(writes_to(&fixture.fake, TRIM_I2C_TXDR) == initial_tx);
}

static void test_register_observation_order(void) {
  Fixture fixture;
  boundary(&fixture, 1);
  /* Hardware finishes between two separate register observations. The old
   * START read must not contradict completion observed later in that poll. */
  fixture.fake.complete_on_cr2_read = TXE | TC | BUSY;
  poll(&fixture);
  assert(fixture.driver.result == TRIM_I2C_PENDING);
  poll(&fixture);
  assert(starts(&fixture.fake) == 2);
  event(&fixture, TXE | RXNE | STOPF);
  poll(&fixture);
  poll(&fixture);
  assert(fixture.driver.result == TRIM_I2C_OK);

  boundary(&fixture, 7);
  fixture.fake.complete_on_cr2_read = TXE | STOPF;
  poll(&fixture);
  assert(fixture.driver.result == TRIM_I2C_PENDING);
  poll(&fixture);
  assert(fixture.driver.result == TRIM_I2C_OK && !fixture.driver.possible_programming);

  init(&fixture, 0);
  acquire(&fixture);
  unsigned before = starts(&fixture.fake);
  assert(trim_i2c_cleanup(&fixture.driver, 1000));
  for (unsigned i = 0; i < 20 && starts(&fixture.fake) == before; ++i) {
    poll(&fixture);
    ++fixture.fake.now;
  }
  assert(starts(&fixture.fake) == before + 1);
  fixture.fake.complete_on_cr2_read = TXE | STOPF;
  poll(&fixture);
  assert(fixture.driver.result == TRIM_I2C_PENDING);
  for (unsigned i = 0; i < 6 && !trim_i2c_closed(&fixture.driver); ++i) {
    poll(&fixture);
    ++fixture.fake.now;
  }
  assert(fixture.driver.result == TRIM_I2C_OK && fixture.driver.released);
}

static void test_review_followups(void) {
  Fixture fixture;
  init(&fixture, 0);
  fixture.fake.registers[TRIM_I2C_ISR / 4] = TXE | STOPF;
  assert(trim_i2c_acquire(&fixture.driver, 1000));
  poll(&fixture);
  assert(fixture.driver.result == TRIM_I2C_FAILED && !fixture.driver.have_saved);
  assert(fixture.fake.count == 0);
  event(&fixture, TXE);
  assert(!trim_i2c_acquire(&fixture.driver, 1000));
  assert(fixture.fake.count == 0);
  init(&fixture, 0);
  acquire(&fixture);
  read_success(&fixture, true);
  assert(fixture.driver.operation_action == TRIM_I2C_READ);
  assert(trim_i2c_write(&fixture.driver, 0x63, 0xA5, 1000));
  assert(fixture.driver.result == TRIM_I2C_PENDING);
  assert(fixture.driver.operation_action == TRIM_I2C_WRITE);
  assert(!fixture.driver.fault && !fixture.driver.operation_berr);

  boundary(&fixture, 5);
  fixture.fake.now = 20;
  poll(&fixture);
  assert(trim_i2c_cleanup(&fixture.driver, 1020));
  poll(&fixture); /* The incomplete AUTOEND write is allowed to finish naturally. */
  unsigned before = fixture.fake.count;
  ++fixture.fake.now;
  event(&fixture, TXE | STOPF);
  poll(&fixture);
  assert(fixture.fake.count == before);
  poll(&fixture);
  assert(fixture.fake.registers[TRIM_I2C_CR1 / 4] == 0);
  assert(fixture.fake.now == 21); /* No need to consume the full five-tick grace. */

  boundary(&fixture, 7);
  const uint32_t nack_times[] = {0, 22, 44};
  for (unsigned i = 0; i < 3; ++i) {
    fixture.fake.now = nack_times[i];
    event(&fixture, TXE | NACK | STOPF);
    poll(&fixture);
    fixture.fake.now += 3;
    poll(&fixture);
    poll(&fixture);
  }
  fixture.fake.now = fixture.driver.ready_started + fixture.driver.bounds.ready - 1;
  event(&fixture, TXE | STOPF);
  poll(&fixture);
  assert(fixture.driver.result == TRIM_I2C_OK && !fixture.driver.possible_programming);
}

static void test_cleanup_write_wait(void) {
  Fixture fixture;
  init(&fixture, 0);
  TrimI2cProfile part = profile();
  part.write_ticks = 50;
  TrimI2cPort port = fixture.driver.port;
  assert(trim_i2c_init(&fixture.driver, &port, &part));
  acquire(&fixture);
  begin_write(&fixture);
  fixture.fake.now = 20;
  poll(&fixture);
  unsigned initial_starts = starts(&fixture.fake);
  assert(trim_i2c_cleanup(&fixture.driver, 1020));
  fixture.fake.advance_on_disable_read = 5;
  bool released = false;
  uint32_t release_stamp = 0;
  for (unsigned i = 0; i < 20 && !released; ++i) {
    poll(&fixture);
    if (fixture.fake.registers[TRIM_I2C_CR1 / 4] == 0) {
      released = true;
      release_stamp = fixture.fake.now;
      assert(fixture.driver.released_at == release_stamp);
      assert(fixture.fake.advance_on_disable_read == 0);
    }
    ++fixture.fake.now;
  }
  assert(released);
  while (fixture.fake.now - release_stamp < 50) {
    poll(&fixture);
    assert(starts(&fixture.fake) == initial_starts);
    ++fixture.fake.now;
  }
  assert(fixture.fake.now - release_stamp == 50);
  poll(&fixture);
  poll(&fixture);
  assert(starts(&fixture.fake) == initial_starts + 1);
}

int main(void) {
  test_bounds();
  test_success_and_reuse();
  test_admission();
  test_read_errors();
  test_write_failures();
  test_deadlines();
  test_stale_and_reset();
  test_cleanup_failures();
  test_stop_policy();
  test_transfer_matrix();
  test_post_observation_deadlines();
  test_probe_and_restoration_boundaries();
  test_exported_budgets();
  test_register_observation_order();
  test_review_followups();
  test_cleanup_write_wait();
  printf("I2C register tests passed; state %zu bytes\n", sizeof(TrimI2cState));
  return 0;
}
