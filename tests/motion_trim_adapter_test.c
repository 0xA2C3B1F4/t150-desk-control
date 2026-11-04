#include "motion_trim_adapter.h"
#include "motion_trim_layout.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { TXE = 1, TXIS = 2, RXNE = 4, NACK = 0x10, STOPF = 0x20, TC = 0x40, BERR = 0x100 };
enum { BUSY = 0x8000, START = 0x2000, STOP = 0x4000, READ = 0x400, AUTOEND = 0x2000000 };
enum { RECORD_ADDRESS = 0x80 };

static const int8_t old_values[TRIM_COUNT] = {-2, -1, 1, 2};
static const int8_t new_values[TRIM_COUNT] = {3, -1, 1, 2};
static const uint8_t native_bytes[TRIM_NATIVE_BYTES] = {1, 2, 3, 4, 5, 6};
static const uint8_t native_addresses[TRIM_NATIVE_BYTES] = {0x40, 0x41, 0x4a, 0x4b, 0x2a, 0x2b};

typedef struct {
  TrimOwnerState owner;
  TrimStorageState record;
  TrimI2cState driver;
  TrimAdapterState adapter;
  uint32_t registers[11];
  uint32_t now;
  uint32_t programming_until;
  uint32_t mask;
  uint32_t accesses;
  uint32_t capture_at;
  uint32_t reads;
  uint32_t corrupt_read;
  uint32_t berr_read;
  uint32_t fail_start;
  uint32_t starts;
  uint32_t writes;
  uint32_t record_writes;
  uint32_t event_at;
  uint32_t event_flags;
  uint32_t late_after_reads;
  uint32_t step_ticks;
  uint32_t completion_delay;
  uint32_t delayed_token;
  uint32_t delay_count;
  uint8_t eeprom[256];
  uint8_t capture_bytes[TRIM_NATIVE_BYTES];
  uint8_t preloaded;
  uint8_t cursor;
  uint8_t late_action;
  uint8_t delayed_action;
  bool programming;
  bool hold_transfers;
  bool fail_cleanup;
  bool inject_berr;
  bool slow_events;
  bool event_pending;
} Rig;

static uint32_t enter_lock(void* context) {
  Rig* rig = context;
  uint32_t previous = rig->mask;
  rig->mask = 1;
  return previous;
}

static void leave_lock(void* context, uint32_t previous) {
  Rig* rig = context;
  assert(rig->mask == 1);
  rig->mask = previous;
}

static void access_port(Rig* rig) {
  assert(rig->mask == 0);
  ++rig->accesses;
  if (rig->capture_at && rig->accesses == rig->capture_at) {
    rig->capture_at = 0;
    assert(trim_adapter_capture_native(&rig->adapter, rig->capture_bytes) == TRIM_CAPTURE_QUEUED);
    assert(rig->mask == 0);
  }
}

static uint32_t read_port(void* context, uint8_t offset) {
  Rig* rig = context;
  access_port(rig);
  uint32_t value = rig->registers[offset / 4];
  if (offset == TRIM_I2C_RXDR) {
    ++rig->reads;
    value = rig->eeprom[rig->cursor];
    if (rig->reads == rig->corrupt_read) {
      value ^= 1;
    }
    rig->registers[TRIM_I2C_ISR / 4] &= ~RXNE;
  }
  return value;
}

/* A deterministic memory-backed peripheral, not an EEPROM electrical model.
 * Events are immediate at START/TXDR; foreground service advances by one tick.
 * Programming delays ACK, and PE reset does not end that memory busy interval. */
static void write_port(void* context, uint8_t offset, uint32_t value) {
  Rig* rig = context;
  access_port(rig);
  if (offset == TRIM_I2C_ICR) {
    rig->registers[TRIM_I2C_ISR / 4] &= ~value;
    return;
  }
  rig->registers[offset / 4] = value;
  if (offset == TRIM_I2C_CR1 && value == 0) {
    rig->event_pending = false;
    rig->registers[TRIM_I2C_ISR / 4] = TXE;
    rig->registers[TRIM_I2C_CR2 / 4] &= ~(START | STOP);
  }
  if (offset == TRIM_I2C_TXDR) {
    if (rig->registers[TRIM_I2C_ISR / 4] & TXIS) {
      rig->eeprom[rig->cursor] = (uint8_t)value;
      ++rig->writes;
      if (rig->cursor >= RECORD_ADDRESS && rig->cursor < RECORD_ADDRESS + TRIM_RECORD_SIZE) {
        ++rig->record_writes;
      }
      rig->programming = true;
      rig->programming_until = rig->now + rig->driver.profile.write_ticks;
      rig->registers[TRIM_I2C_ISR / 4] = TXE | STOPF;
    } else {
      rig->preloaded = (uint8_t)value;
    }
  }
  if (offset != TRIM_I2C_CR2 || !(value & START)) {
    return;
  }
  ++rig->starts;
  rig->registers[TRIM_I2C_CR2 / 4] &= ~START;
  if (rig->hold_transfers) {
    rig->registers[TRIM_I2C_ISR / 4] = TXE | BUSY;
    return;
  }
  unsigned count = (value >> 16) & 0xff;
  if ((rig->fail_start && rig->starts == rig->fail_start) ||
      (rig->fail_cleanup && rig->driver.action == TRIM_I2C_CLEANUP)) {
    rig->registers[TRIM_I2C_ISR / 4] = TXE | NACK | STOPF;
  } else if (count == 0) {
    bool busy = rig->programming && (int32_t)(rig->now - rig->programming_until) < 0;
    rig->registers[TRIM_I2C_ISR / 4] = TXE | STOPF | (busy ? NACK : 0);
  } else if (value & READ) {
    rig->registers[TRIM_I2C_ISR / 4] = TXE | RXNE | STOPF;
    if (rig->inject_berr || rig->reads + 1 == rig->berr_read) {
      rig->inject_berr = false;
      rig->registers[TRIM_I2C_ISR / 4] |= BERR;
    }
  } else {
    rig->cursor = rig->preloaded;
    rig->registers[TRIM_I2C_ISR / 4] = TXE | BUSY | (count == 1 ? TC : TXIS);
  }
  if (rig->slow_events) {
    rig->event_flags = rig->registers[TRIM_I2C_ISR / 4];
    /* TC/probe STOP may arrive at T-1. RXNE/TXIS need an additional foreground
     * STOP observation, so deliver them one tick earlier. */
    uint32_t gap = rig->driver.profile.max_poll_gap;
    uint32_t delay = rig->driver.profile.transfer_ticks - 1;
    if (count != 0 && !(count == 1 && !(value & READ))) {
      delay -= gap;
    }
    rig->event_at = rig->now + delay / gap * gap;
    rig->event_pending = true;
    rig->registers[TRIM_I2C_ISR / 4] = TXE | BUSY;
  }
}

static bool lines_port(void* context) {
  Rig* rig = context;
  access_port(rig);
  return true;
}

static uint32_t now_port(void* context) {
  Rig* rig = context;
  bool delivering = rig->adapter.phase == TRIM_ADAPTER_ACQUIRE ||
                    rig->adapter.phase == TRIM_ADAPTER_BYTE ||
                    rig->adapter.phase == TRIM_ADAPTER_CLEANUP;
  if (rig->completion_delay && delivering && rig->driver.result == TRIM_I2C_OK &&
      rig->owner.outstanding &&
      (rig->delayed_token != rig->owner.operation.token ||
       rig->delayed_action != rig->driver.action)) {
    rig->now += rig->completion_delay;
    rig->delayed_token = rig->owner.operation.token;
    rig->delayed_action = rig->driver.action;
    ++rig->delay_count;
  }
  if (rig->late_action && rig->driver.action == rig->late_action &&
      rig->driver.result == TRIM_I2C_OK && rig->owner.outstanding &&
      rig->reads == rig->late_after_reads) {
    rig->now = rig->owner.issued_at + rig->owner.issued_limit + 1;
    rig->late_action = 0;
  }
  return rig->now;
}

static TrimI2cProfile profile(void) {
  /* Synthetic timing only. No physical device or tWR is qualified here. */
  return (TrimI2cProfile){.timing = 0x10420f13,
                          .transfer_ticks = 3,
                          .write_ticks = 5,
                          .poll_ticks = 1,
                          .stop_ticks = 1,
                          .max_poll_gap = 1,
                          .address = 0x50};
}

static void init_scaled(Rig* rig, uint32_t now, uint32_t scale, uint32_t delivery) {
  memset(rig, 0, sizeof(*rig));
  memcpy(rig->capture_bytes, native_bytes, TRIM_NATIVE_BYTES);
  rig->now = now;
  rig->step_ticks = scale;
  rig->registers[TRIM_I2C_CR1 / 4] = 1;
  rig->registers[TRIM_I2C_CR2 / 4] = AUTOEND | (2u << 16) | 0xa0;
  rig->registers[TRIM_I2C_OAR1 / 4] = 0x8000;
  rig->registers[TRIM_I2C_TIMINGR / 4] = 0x00210507;
  rig->registers[TRIM_I2C_ISR / 4] = TXE;
  TrimI2cPort port = {read_port, write_port, lines_port, now_port, rig};
  TrimAdapterLock lock = {enter_lock, leave_lock, rig};
  TrimI2cProfile part = profile();
  part.transfer_ticks *= scale;
  part.write_ticks *= scale;
  part.poll_ticks *= scale;
  part.stop_ticks *= scale;
  part.max_poll_gap *= scale;
  /* Scaling every duration changes only synthetic clock units, not the wall
   * time contract. No longer physical ownership window is proposed here. */
  assert(trim_adapter_init(&rig->adapter, &rig->owner, &rig->record, &rig->driver, &port, &part,
                           &lock, RECORD_ADDRESS, 1500 * scale, delivery));
  assert(rig->accesses == 0);
}

static void init(Rig* rig, uint32_t now) {
  init_scaled(rig, now, 1, 2);
}

static TrimAdapterView view(Rig* rig) {
  TrimAdapterView value;
  trim_adapter_view(&rig->adapter, &value);
  return value;
}

static void step(Rig* rig) {
  rig->now += rig->step_ticks;
  if (rig->event_pending && (int32_t)(rig->now - rig->event_at) >= 0) {
    rig->registers[TRIM_I2C_ISR / 4] = rig->event_flags;
    rig->event_pending = false;
  }
  uint32_t before = rig->accesses;
  trim_adapter_poll(&rig->adapter);
  assert(rig->mask == 0);
  assert(rig->accesses - before <= 24);
}

static TrimAdapterView finish(Rig* rig) {
  for (unsigned i = 0; i < 2000; ++i) {
    TrimAdapterView value = view(rig);
    if (!value.busy && value.closed) {
      uint32_t before = rig->accesses;
      trim_adapter_poll(&rig->adapter);
      assert(rig->accesses == before);
      return value;
    }
    step(rig);
  }
  assert(false && "adapter did not terminate");
  return view(rig);
}

static void boot(Rig* rig) {
  assert(trim_adapter_begin_boot(&rig->adapter));
  TrimAdapterView value = finish(rig);
  assert(value.result == TRIM_OWNER_OK && value.handoff && value.boot_coherent);
}

/* A cold restart resets controller and peripheral state, retaining EEPROM.
 * This models a new session, never recovery of an active or unreleased bus. */
static void cold_boot(Rig* rig) {
  uint8_t eeprom[sizeof rig->eeprom];
  memcpy(eeprom, rig->eeprom, sizeof eeprom);
  init(rig, 0);
  memcpy(rig->eeprom, eeprom, sizeof eeprom);
  boot(rig);
}

static void boot_old(Rig* rig, uint32_t now) {
  init(rig, now);
  assert(trim_record_encode(old_values, rig->eeprom + RECORD_ADDRESS));
  boot(rig);
  assert(view(rig).storage == TRIM_VERIFIED);
  assert(memcmp(view(rig).active, old_values, TRIM_COUNT) == 0);
}

static void test_admission(void) {
  TrimI2cProfile part = profile();
  TrimAdapterBudget budget;
  assert(trim_adapter_budget(&part, 1500, 2, &budget));
  assert(budget.set_required < 1500 && budget.boot_required < budget.set_required);
  TrimAdapterBudget before = budget;
  assert(!trim_adapter_budget(&part, before.set_required, 2, &budget));
  assert(memcmp(&budget, &before, sizeof budget) == 0);
  assert(trim_adapter_budget(&part, before.set_required + 1, 2, &budget));
  part.max_poll_gap = 5;
  assert(!trim_adapter_budget(&part, 1500, 2, &budget));
  part = profile();
  assert(!trim_adapter_budget(&part, UINT32_MAX, 2, &budget));
  assert(!trim_adapter_budget(&part, 1500, UINT32_MAX, &budget));
  assert(!trim_adapter_budget(&part, 1500, 0, &budget));
  part.transfer_ticks = 0x20000000;
  assert(!trim_adapter_budget(&part, 0x7fffffff, 2, &budget));

  for (unsigned address = 0; address < 256; ++address) {
    Rig rig;
    init(&rig, 0);
    TrimAdapterState unchanged = rig.adapter;
    TrimI2cState driver = rig.driver;
    part = profile();
    unsigned end = address + TRIM_RECORD_SIZE;
    bool valid = end <= 256 && !(address <= 0x61 && end > 0x60);
    for (unsigned i = 0; i < TRIM_NATIVE_BYTES; ++i) {
      valid = valid && !(address <= native_addresses[i] && end > native_addresses[i]);
    }
    assert(trim_adapter_init(&rig.adapter, &rig.owner, &rig.record, &rig.driver, &driver.port,
                             &part, &unchanged.lock, (uint8_t)address, 1500, 2) == valid);
    if (!valid) {
      assert(memcmp(&rig.adapter, &unchanged, sizeof unchanged) == 0);
      assert(memcmp(&rig.driver, &driver, sizeof driver) == 0);
    }
  }
}

static void test_success_and_publication(void) {
  Rig rig;
  boot_old(&rig, 0);
  uint32_t reads = rig.reads;
  uint32_t writes = rig.writes;
  assert(trim_adapter_begin_set(&rig.adapter, new_values));
  assert(!trim_adapter_begin_set(&rig.adapter, old_values));
  while (view(&rig).busy) {
    assert(memcmp(view(&rig).active, old_values, TRIM_COUNT) == 0);
    assert(!view(&rig).handoff);
    step(&rig);
  }
  TrimAdapterView value = finish(&rig);
  assert(value.result == TRIM_OWNER_OK && value.storage == TRIM_VERIFIED && value.handoff);
  assert(memcmp(value.active, new_values, TRIM_COUNT) == 0);
  assert(rig.reads - reads == TRIM_STORAGE_MAX_READS);
  assert(rig.writes - writes == TRIM_STORAGE_MAX_WRITES);
  uint32_t elapsed = rig.now - rig.owner.started;
  assert(elapsed < rig.adapter.budget.set_required);
  assert(elapsed < 1500);
  reads = rig.reads;
  writes = rig.writes;
  assert(trim_adapter_begin_set(&rig.adapter, new_values));
  assert(finish(&rig).result == TRIM_OWNER_OK);
  assert(rig.reads - reads == TRIM_STORAGE_BOOT_READS && rig.writes == writes);

  boot_old(&rig, UINT32_MAX - 100);
  rig.slow_events = true;
  assert(trim_adapter_begin_set(&rig.adapter, new_values));
  assert(finish(&rig).result == TRIM_OWNER_OK);
  assert(rig.now - rig.owner.started < rig.adapter.budget.set_required);
  assert(rig.record_writes == TRIM_STORAGE_MAX_WRITES);
}

static void test_boot_and_repair(void) {
  for (unsigned invalid = 0; invalid < 16; ++invalid) {
    Rig rig;
    init(&rig, 0);
    assert(trim_record_encode(old_values, rig.eeprom + RECORD_ADDRESS));
    int8_t expected[TRIM_COUNT];
    memcpy(expected, old_values, sizeof expected);
    for (unsigned slot = 0; slot < TRIM_COUNT; ++slot) {
      if (invalid & (1u << slot)) {
        rig.eeprom[RECORD_ADDRESS + slot] = slot % 2 ? 0xff : 0;
        expected[slot] = 0;
      }
    }
    assert(!trim_adapter_begin_set(&rig.adapter, expected));
    boot(&rig);
    assert(view(&rig).storage == (invalid ? TRIM_DEFAULT : TRIM_VERIFIED));
    assert(memcmp(view(&rig).active, expected, TRIM_COUNT) == 0);
    assert(rig.writes == 0 && rig.reads == TRIM_STORAGE_BOOT_READS);
    uint8_t saved[TRIM_RECORD_SIZE];
    memcpy(saved, rig.eeprom + RECORD_ADDRESS, sizeof saved);
    assert(trim_adapter_begin_set(&rig.adapter, expected));
    assert(finish(&rig).result == TRIM_OWNER_OK);
    assert(rig.writes == 0);
    expected[0] = 3;
    assert(trim_adapter_begin_set(&rig.adapter, expected));
    assert(finish(&rig).result == TRIM_OWNER_OK);
    assert(rig.record_writes == 1);
    assert(memcmp(saved + 1, rig.eeprom + RECORD_ADDRESS + 1, TRIM_RECORD_SIZE - 1) == 0);
    uint8_t status = invalid & ~1u ? TRIM_DEFAULT : TRIM_VERIFIED;
    assert(view(&rig).storage == status);
    cold_boot(&rig);
    assert(view(&rig).storage == status);
    assert(memcmp(view(&rig).active, expected, TRIM_COUNT) == 0);
  }
}

static void test_service_gap_and_delivery(void) {
  for (unsigned latency = 0; latency < 2; ++latency) {
    Rig rig;
    init_scaled(&rig, UINT32_MAX - 50, 2, latency ? 2 : 4);
    assert(trim_record_encode(old_values, rig.eeprom + RECORD_ADDRESS));
    boot(&rig);
    rig.slow_events = true;
    if (latency) {
      /* A one-tick step plus one tick of completion handling still respects
       * the configured two-tick maximum between driver polls. */
      rig.step_ticks = 1;
      rig.completion_delay = 1;
    }
    assert(trim_adapter_begin_set(&rig.adapter, new_values));
    TrimAdapterView value = finish(&rig);
    assert(value.result == TRIM_OWNER_OK && value.storage == TRIM_VERIFIED);
    assert(rig.record_writes == TRIM_STORAGE_MAX_WRITES);
    assert(rig.now - rig.owner.started < rig.adapter.budget.set_required);
    if (latency) {
      assert(rig.delay_count >= TRIM_STORAGE_MAX_OPERATIONS);
    }
  }
}

static void test_boot_failures(void) {
  for (unsigned kind = 0; kind < 5; ++kind) {
    Rig rig;
    init(&rig, 0);
    assert(trim_record_encode(old_values, rig.eeprom + RECORD_ADDRESS));
    if (kind == 0) {
      rig.corrupt_read = 7;
    }
    if (kind == 1) {
      rig.fail_start = 1;
    }
    if (kind == 2) {
      rig.inject_berr = true;
    }
    if (kind == 3) {
      rig.fail_cleanup = true;
    }
    if (kind == 4) {
      rig.registers[TRIM_I2C_CR1 / 4] = 0;
    }
    assert(trim_adapter_begin_boot(&rig.adapter));
    TrimAdapterView value = finish(&rig);
    assert(value.result != TRIM_OWNER_OK && !value.boot_coherent);
    assert(value.storage == TRIM_FAULT && rig.writes == 0);
    assert(!trim_adapter_begin_set(&rig.adapter, new_values));
    assert(!trim_adapter_begin_boot(&rig.adapter));
    if (kind >= 3) {
      assert(value.result == TRIM_OWNER_UNRELEASED && !value.handoff);
      assert(!trim_adapter_begin_set(&rig.adapter, new_values));
    } else {
      assert(value.handoff);
      rig.corrupt_read = 0;
      rig.fail_start = 0;
      cold_boot(&rig);
    }
  }
  for (unsigned read = 1; read <= TRIM_STORAGE_BOOT_READS; ++read) {
    Rig rig;
    init(&rig, 0);
    rig.berr_read = read;
    assert(trim_adapter_begin_boot(&rig.adapter));
    TrimAdapterView value = finish(&rig);
    assert(value.result == TRIM_OWNER_STORAGE_ERROR && value.storage == TRIM_FAULT);
    assert(!value.boot_coherent && value.handoff && rig.writes == 0);
  }
  for (unsigned interrupt = 0; interrupt < 2; ++interrupt) {
    Rig rig;
    init(&rig, 0);
    assert(trim_record_encode(old_values, rig.eeprom + RECORD_ADDRESS));
    assert(trim_adapter_begin_boot(&rig.adapter));
    while (!rig.owner.trim_verified) {
      step(&rig);
    }
    assert(!view(&rig).boot_coherent);
    if (interrupt == 0) {
      assert(trim_adapter_capture_native(&rig.adapter, native_bytes) == TRIM_CAPTURE_QUEUED);
    } else {
      rig.now = rig.owner.started + rig.owner.limits.window;
    }
    TrimAdapterView value = finish(&rig);
    assert(value.result != TRIM_OWNER_OK && !value.boot_coherent);
    const int8_t zeros[TRIM_COUNT] = {0};
    assert(value.storage == TRIM_FAULT && memcmp(value.active, zeros, TRIM_COUNT) == 0);
  }
}

static void test_failed_repair(void) {
  Rig rig;
  init(&rig, 0);
  assert(trim_record_encode(old_values, rig.eeprom + RECORD_ADDRESS));
  rig.eeprom[RECORD_ADDRESS] ^= 1;
  boot(&rig);
  assert(view(&rig).storage == TRIM_DEFAULT);
  assert(trim_adapter_begin_set(&rig.adapter, new_values));
  while (!rig.record_writes) {
    step(&rig);
  }
  rig.corrupt_read = rig.reads + 1;
  TrimAdapterView value = finish(&rig);
  assert(value.result == TRIM_OWNER_STORAGE_ERROR && value.storage == TRIM_UNCERTAIN);
  assert(value.handoff && rig.record.write_issued && !value.boot_coherent);
  assert(!trim_adapter_begin_set(&rig.adapter, new_values));
  rig.corrupt_read = 0;
  cold_boot(&rig);
  assert(view(&rig).storage == TRIM_VERIFIED && view(&rig).boot_coherent);
  assert(trim_adapter_begin_set(&rig.adapter, new_values));
  assert(finish(&rig).result == TRIM_OWNER_OK);
  assert(view(&rig).storage == TRIM_VERIFIED);
}

static void test_repeated_native_capture(void) {
  for (unsigned failure = 0; failure < 2; ++failure) {
    Rig rig;
    boot_old(&rig, 0);
    assert(trim_adapter_begin_set(&rig.adapter, new_values));
    assert(trim_adapter_capture_native(&rig.adapter, native_bytes) == TRIM_CAPTURE_QUEUED);
    while (!rig.writes) {
      step(&rig);
    }
    uint8_t next[TRIM_NATIVE_BYTES] = {7, 8, 9, 10, 11, 12};
    if (failure) {
      rig.corrupt_read = rig.reads + 1;
    } else {
      assert(trim_adapter_capture_native(&rig.adapter, next) == TRIM_CAPTURE_QUEUED);
      ++next[1];
      assert(trim_adapter_capture_native(&rig.adapter, next) == TRIM_CAPTURE_QUEUED);
    }
    TrimAdapterView value = finish(&rig);
    assert(value.result == TRIM_OWNER_STORAGE_ERROR && value.handoff);
    assert(rig.record_writes == 0);
    uint8_t pending[TRIM_NATIVE_BYTES];
    if (failure) {
      assert(trim_adapter_pending_native(&rig.adapter, pending));
      assert(memcmp(pending, native_bytes, TRIM_NATIVE_BYTES) == 0);
      assert(!trim_adapter_begin_set(&rig.adapter, new_values));
      assert(trim_adapter_capture_native(&rig.adapter, next) == TRIM_CAPTURE_PASSTHROUGH);
      assert(trim_adapter_pending_native(&rig.adapter, pending));
      assert(memcmp(pending, next, TRIM_NATIVE_BYTES) == 0);
      ++next[0];
      assert(trim_adapter_capture_native(&rig.adapter, next) == TRIM_CAPTURE_PASSTHROUGH);
      assert(trim_adapter_pending_native(&rig.adapter, pending));
      assert(memcmp(pending, next, TRIM_NATIVE_BYTES) == 0);
      assert(!trim_adapter_begin_set(&rig.adapter, new_values));
    } else {
      assert(rig.owner.generation == 2);
      assert(!trim_adapter_pending_native(&rig.adapter, pending));
      for (unsigned i = 0; i < TRIM_NATIVE_BYTES; ++i) {
        assert(rig.eeprom[native_addresses[i]] == next[i]);
      }
    }
  }
}

static void test_berr_on_set_and_native_reads(void) {
  for (unsigned native = 0; native < 2; ++native) {
    unsigned count = native ? TRIM_NATIVE_BYTES : TRIM_STORAGE_MAX_READS;
    for (unsigned read = 1; read <= count; ++read) {
      Rig rig;
      boot_old(&rig, 0);
      rig.berr_read = rig.reads + read;
      assert(trim_adapter_begin_set(&rig.adapter, new_values));
      if (native) {
        assert(trim_adapter_capture_native(&rig.adapter, native_bytes) == TRIM_CAPTURE_QUEUED);
      }
      TrimAdapterView value = finish(&rig);
      assert(value.result == TRIM_OWNER_STORAGE_ERROR && value.handoff);
      assert(value.storage == TRIM_UNCERTAIN && !value.boot_coherent);
      assert(memcmp(value.active, old_values, TRIM_COUNT) == 0);
      assert(rig.record.write_issued == (!native && read > TRIM_STORAGE_BOOT_READS));
      if (native) {
        uint8_t pending[TRIM_NATIVE_BYTES];
        assert(trim_adapter_pending_native(&rig.adapter, pending));
        assert(memcmp(pending, native_bytes, TRIM_NATIVE_BYTES) == 0);
        assert(!trim_adapter_begin_set(&rig.adapter, new_values));
      }
    }
  }
}

static void check_native(Rig* rig) {
  for (unsigned i = 0; i < TRIM_NATIVE_BYTES; ++i) {
    assert(rig->eeprom[native_addresses[i]] == native_bytes[i]);
  }
  uint8_t pending[TRIM_NATIVE_BYTES];
  assert(!trim_adapter_pending_native(&rig->adapter, pending));
}

static void test_boot_and_drain_captures(void) {
  for (unsigned drain = 0; drain < 2; ++drain) {
    Rig baseline;
    init(&baseline, 0);
    assert(trim_adapter_begin_boot(&baseline.adapter));
    if (drain) {
      assert(trim_adapter_capture_native(&baseline.adapter, native_bytes) == TRIM_CAPTURE_QUEUED);
    }
    finish(&baseline);
    uint32_t accesses = baseline.accesses;
    for (uint32_t offset = 1; offset <= accesses; ++offset) {
      Rig rig;
      init(&rig, 0);
      assert(trim_adapter_begin_boot(&rig.adapter));
      if (drain) {
        assert(trim_adapter_capture_native(&rig.adapter, native_bytes) == TRIM_CAPTURE_QUEUED);
        ++rig.capture_bytes[0];
      }
      rig.capture_at = offset;
      TrimAdapterView value = finish(&rig);
      assert(value.result == TRIM_OWNER_STORAGE_ERROR && value.handoff);
      assert(value.storage == TRIM_FAULT && !value.boot_coherent && !rig.record.write_issued);
      assert(rig.record_writes == 0);
      for (unsigned i = 0; i < TRIM_NATIVE_BYTES; ++i) {
        assert(rig.eeprom[native_addresses[i]] == rig.capture_bytes[i]);
      }
      uint8_t pending[TRIM_NATIVE_BYTES];
      assert(!trim_adapter_pending_native(&rig.adapter, pending));
    }
    printf("%s capture boundaries: %u\n", drain ? "Native drain" : "Boot", accesses);
  }
}

static void test_native_preemption(void) {
  Rig acquisition;
  boot_old(&acquisition, 0);
  uint32_t initial_reads = acquisition.reads;
  assert(trim_adapter_begin_set(&acquisition.adapter, new_values));
  acquisition.capture_at = acquisition.accesses + 1;
  assert(finish(&acquisition).result == TRIM_OWNER_STORAGE_ERROR);
  assert(acquisition.reads - initial_reads == TRIM_NATIVE_BYTES);
  assert(acquisition.record_writes == 0);

  Rig baseline;
  boot_old(&baseline, 0);
  assert(trim_adapter_begin_set(&baseline.adapter, new_values));
  uint32_t begin = baseline.accesses;
  assert(finish(&baseline).result == TRIM_OWNER_OK);
  uint32_t accesses = baseline.accesses - begin;
  for (uint32_t offset = 0; offset <= accesses; ++offset) {
    Rig rig;
    boot_old(&rig, 0);
    assert(trim_adapter_begin_set(&rig.adapter, new_values));
    if (offset == 0) {
      rig.mask = 1;
      assert(trim_adapter_capture_native(&rig.adapter, native_bytes) == TRIM_CAPTURE_QUEUED);
      assert(rig.mask == 1);
      rig.mask = 0;
    } else {
      rig.capture_at = rig.accesses + offset;
    }
    TrimAdapterView value = finish(&rig);
    assert(value.result == TRIM_OWNER_STORAGE_ERROR && rig.owner.reason == TRIM_OWNER_PREEMPTED);
    assert(value.handoff && value.closed && !value.boot_coherent);
    assert(value.storage == TRIM_UNCERTAIN);
    if (rig.record_writes) {
      assert(rig.record.write_issued);
    }
    assert(memcmp(value.active, old_values, TRIM_COUNT) == 0);
    check_native(&rig);
    cold_boot(&rig);
  }
  printf("Native capture boundaries: %u\n", accesses + 1);
}

static void test_deadlines_and_retention(void) {
  Rig rig;
  boot_old(&rig, 0);
  rig.fail_cleanup = true;
  assert(trim_adapter_begin_set(&rig.adapter, new_values));
  TrimAdapterView failed_cleanup = finish(&rig);
  assert(failed_cleanup.result == TRIM_OWNER_UNRELEASED && !failed_cleanup.handoff);
  assert(failed_cleanup.storage == TRIM_UNCERTAIN && rig.record.write_issued);
  assert(memcmp(failed_cleanup.active, old_values, TRIM_COUNT) == 0);
  assert(trim_adapter_capture_native(&rig.adapter, native_bytes) == TRIM_CAPTURE_RETAINED);
  assert(!trim_adapter_begin_set(&rig.adapter, new_values));

  boot_old(&rig, 0);
  rig.hold_transfers = true;
  assert(trim_adapter_begin_set(&rig.adapter, new_values));
  while (rig.owner.phase != TRIM_OWNER_CLEANUP) {
    step(&rig);
  }
  rig.hold_transfers = false;
  assert(finish(&rig).result == TRIM_OWNER_STORAGE_ERROR);
  assert(view(&rig).handoff && rig.record_writes == 0);

  for (unsigned delay = 0; delay < 2; ++delay) {
    boot_old(&rig, 0);
    assert(trim_adapter_begin_set(&rig.adapter, new_values));
    while (rig.adapter.phase != TRIM_ADAPTER_BYTE) {
      step(&rig);
    }
    rig.now = rig.adapter.driver_deadline + delay;
    trim_adapter_poll(&rig.adapter);
    assert(rig.driver.fault == TRIM_I2C_DEADLINE);
    assert(finish(&rig).result == TRIM_OWNER_STORAGE_ERROR);
    assert(view(&rig).handoff);
  }

  for (unsigned action = 0; action < 2; ++action) {
    boot_old(&rig, 0);
    rig.late_action = action == 0 ? TRIM_I2C_READ : TRIM_I2C_CLEANUP;
    rig.late_after_reads = rig.reads + TRIM_STORAGE_MAX_READS;
    assert(trim_adapter_begin_set(&rig.adapter, new_values));
    TrimAdapterView late = finish(&rig);
    assert(late.result == TRIM_OWNER_UNRELEASED && !late.handoff && late.closed);
    assert(late.storage == TRIM_UNCERTAIN);
    assert(memcmp(late.active, old_values, TRIM_COUNT) == 0);
    if (action == 1) {
      assert(rig.driver.released);
      assert(rig.record.status == TRIM_VERIFIED);
    }
  }

  for (unsigned equality = 0; equality < 2; ++equality) {
    boot_old(&rig, 0);
    assert(trim_adapter_begin_set(&rig.adapter, new_values));
    while (!(rig.driver.action == TRIM_I2C_CLEANUP && rig.driver.result == TRIM_I2C_PENDING &&
             rig.registers[TRIM_I2C_TIMINGR / 4] == rig.driver.saved[3])) {
      step(&rig);
    }
    assert(!rig.driver.released);
    /* Adversarial late observation, not a claim about healthy maximum service
     * spacing: restoration is written, but its final certificate is pending. */
    rig.now = rig.adapter.driver_deadline - 1 + equality;
    rig.completion_delay = 1;
    trim_adapter_poll(&rig.adapter);
    TrimAdapterView boundary = finish(&rig);
    if (equality) {
      assert(boundary.result == TRIM_OWNER_UNRELEASED && !boundary.handoff);
      assert(memcmp(boundary.active, old_values, TRIM_COUNT) == 0);
    } else {
      assert(boundary.result == TRIM_OWNER_OK && boundary.handoff);
      assert(memcmp(boundary.active, new_values, TRIM_COUNT) == 0);
    }
  }

  boot_old(&rig, 0);
  assert(trim_adapter_begin_set(&rig.adapter, new_values));
  while (rig.adapter.phase != TRIM_ADAPTER_BYTE) {
    step(&rig);
  }
  rig.now = rig.owner.started + rig.owner.limits.window;
  trim_adapter_poll(&rig.adapter);
  TrimAdapterView value = finish(&rig);
  assert(value.result == TRIM_OWNER_UNRELEASED && !value.handoff && value.closed);
  assert(memcmp(value.active, old_values, TRIM_COUNT) == 0);
  assert(!trim_adapter_begin_set(&rig.adapter, new_values));
  assert(trim_adapter_capture_native(&rig.adapter, native_bytes) == TRIM_CAPTURE_RETAINED);
  uint8_t latest[TRIM_NATIVE_BYTES] = {6, 5, 4, 3, 2, 1};
  assert(trim_adapter_capture_native(&rig.adapter, latest) == TRIM_CAPTURE_RETAINED);
  uint8_t retained[TRIM_NATIVE_BYTES];
  assert(trim_adapter_pending_native(&rig.adapter, retained));
  assert(memcmp(retained, latest, TRIM_NATIVE_BYTES) == 0);
  uint32_t accesses = rig.accesses;
  for (unsigned i = 0; i < 20; ++i) {
    step(&rig);
  }
  assert(rig.accesses == accesses);
}

int main(void) {
  test_admission();
  test_success_and_publication();
  test_boot_and_repair();
  test_service_gap_and_delivery();
  test_boot_failures();
  test_failed_repair();
  test_repeated_native_capture();
  test_berr_on_set_and_native_reads();
  test_boot_and_drain_captures();
  test_native_preemption();
  test_deadlines_and_retention();
  puts("Storage adapter checks passed");
  return 0;
}
