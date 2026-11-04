#include "motion_trim_storage.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  uint8_t bytes[TRIM_RECORD_SIZE];
  TrimStorageOperation log[TRIM_STORAGE_MAX_OPERATIONS];
  unsigned count;
  unsigned writes;
} Eeprom;

static const int8_t previous[TRIM_COUNT] = {-3, -1, 1, 2};
static const int8_t requested[TRIM_COUNT] = {3, -1, 1, 2};
static const int8_t zeros[TRIM_COUNT] = {0};

static bool same_operation(const TrimStorageOperation* first, const TrimStorageOperation* second) {
  return first->token == second->token && first->kind == second->kind &&
         first->offset == second->offset && first->value == second->value;
}

static void assert_state_equal(const TrimStorageState* first, const TrimStorageState* second) {
  assert(first->token == second->token);
  assert(memcmp(first->active, second->active, sizeof first->active) == 0);
  assert(memcmp(first->desired, second->desired, sizeof first->desired) == 0);
  assert(memcmp(first->reads, second->reads, sizeof first->reads) == 0);
  assert(first->mode == second->mode);
  assert(first->phase == second->phase);
  assert(first->index == second->index);
  assert(first->outstanding == second->outstanding);
  assert(first->write_issued == second->write_issued);
  assert(first->status == second->status);
  assert(first->selected == second->selected);
}

static void assert_tuple(const int8_t actual[TRIM_COUNT], const int8_t expected[TRIM_COUNT]) {
  assert(memcmp(actual, expected, TRIM_COUNT) == 0);
}

static void reset_log(Eeprom* eeprom) {
  eeprom->count = 0;
  eeprom->writes = 0;
}

static TrimStorageOperation request(TrimStorageState* state, Eeprom* eeprom) {
  TrimStorageOperation operation, repeated;
  assert(trim_storage_next(state, &operation));
  TrimStorageState unchanged = *state;
  assert(trim_storage_next(state, &repeated));
  assert(same_operation(&operation, &repeated));
  assert_state_equal(state, &unchanged);
  assert(operation.offset < TRIM_RECORD_SIZE);
  assert(operation.token != 0);
  assert(operation.kind == TRIM_STORAGE_READ || operation.kind == TRIM_STORAGE_WRITE);
  assert(eeprom->count < TRIM_STORAGE_MAX_OPERATIONS);
  eeprom->log[eeprom->count++] = operation;
  if (operation.kind == TRIM_STORAGE_WRITE) {
    ++eeprom->writes;
  }
  return operation;
}

static void complete(TrimStorageState* state, Eeprom* eeprom, TrimStorageOperation operation,
                     bool succeeded, bool apply_write, uint8_t read_value) {
  if (succeeded && apply_write && operation.kind == TRIM_STORAGE_WRITE) {
    eeprom->bytes[operation.offset] = operation.value;
  }
  assert(trim_storage_complete(state, &operation, succeeded, read_value));
}

static void step(TrimStorageState* state, Eeprom* eeprom) {
  TrimStorageOperation operation = request(state, eeprom);
  complete(state, eeprom, operation, true, true, eeprom->bytes[operation.offset]);
}

static void finish(TrimStorageState* state, Eeprom* eeprom, const int8_t old[TRIM_COUNT]) {
  while (state->mode != TRIM_STORAGE_TERMINAL) {
    assert_tuple(state->active, old);
    step(state, eeprom);
  }
}

static void assert_terminal(TrimStorageState* state) {
  TrimStorageState unchanged = *state;
  TrimStorageOperation operation = {23, TRIM_STORAGE_WRITE, 4, 9};
  TrimStorageOperation original = operation;
  assert(!trim_storage_next(state, &operation));
  assert(same_operation(&operation, &original));
  assert(!trim_storage_complete(state, &operation, true, 0));
  assert(!trim_storage_cancel(state));
  assert_state_equal(state, &unchanged);
}

static void boot(TrimStorageState* state, Eeprom* eeprom, uint8_t status,
                 const int8_t values[TRIM_COUNT]) {
  trim_storage_init(state);
  reset_log(eeprom);
  assert(trim_storage_begin_boot(state));
  finish(state, eeprom, zeros);
  assert(state->status == status);
  assert_tuple(state->active, values);
  assert(eeprom->count == TRIM_STORAGE_BOOT_READS);
  assert(eeprom->writes == 0);
  assert(state->write_issued == 0);
  assert_terminal(state);
}

static void prepare_set(TrimStorageState* state, Eeprom* eeprom) {
  assert(trim_record_encode(previous, eeprom->bytes));
  boot(state, eeprom, TRIM_VERIFIED, previous);
  reset_log(eeprom);
  assert(trim_storage_begin_set(state, requested));
}

/* Verify the public byte sequence without depending on private phase numbers. */
static void assert_trace(const Eeprom* eeprom, unsigned selected, uint8_t codeword) {
  bool changed = selected < TRIM_COUNT;
  assert(eeprom->count == (changed ? 11u : 8u));
  assert(eeprom->writes == (changed ? 1u : 0u));
  for (unsigned i = 0; i < eeprom->count; ++i) {
    const TrimStorageOperation* operation = &eeprom->log[i];
    assert(operation->kind == (i == 8 ? TRIM_STORAGE_WRITE : TRIM_STORAGE_READ));
    assert(operation->offset == (i < 8 ? i % 4 : selected));
    assert(operation->value == (i == 8 ? codeword : 0));
    if (i > 0) {
      assert(operation->token != eeprom->log[i - 1].token);
    }
  }
}

static void boot_records(void) {
  TrimStorageState state;
  Eeprom eeprom = {0};
  for (unsigned invalid = 0; invalid < 16; ++invalid) {
    int8_t expected[TRIM_COUNT];
    memcpy(expected, previous, sizeof expected);
    assert(trim_record_encode(previous, eeprom.bytes));
    for (unsigned slot = 0; slot < TRIM_COUNT; ++slot) {
      if (invalid & (1u << slot)) {
        eeprom.bytes[slot] = slot % 2 ? 0xff : 0;
        expected[slot] = 0;
      }
    }
    uint8_t saved[TRIM_RECORD_SIZE];
    memcpy(saved, eeprom.bytes, sizeof saved);
    boot(&state, &eeprom, invalid ? TRIM_DEFAULT : TRIM_VERIFIED, expected);
    assert(memcmp(saved, eeprom.bytes, sizeof saved) == 0);
    reset_log(&eeprom);
    assert(trim_storage_begin_set(&state, expected));
    finish(&state, &eeprom, expected);
    assert(state.status == (invalid ? TRIM_DEFAULT : TRIM_VERIFIED));
    assert_trace(&eeprom, TRIM_COUNT, 0);
    assert(memcmp(saved, eeprom.bytes, sizeof saved) == 0);
  }
  for (unsigned failure = 0; failure < 2; ++failure) {
    for (unsigned position = 0; position < TRIM_STORAGE_BOOT_READS; ++position) {
      assert(trim_record_encode(previous, eeprom.bytes));
      trim_storage_init(&state);
      reset_log(&eeprom);
      assert(trim_storage_begin_boot(&state));
      while (state.mode != TRIM_STORAGE_TERMINAL) {
        TrimStorageOperation operation = request(&state, &eeprom);
        bool inject = eeprom.count - 1 == position;
        complete(&state, &eeprom, operation, !(inject && failure), false,
                 eeprom.bytes[operation.offset] ^ (inject ? 1 : 0));
      }
      assert(state.status == TRIM_FAULT && eeprom.writes == 0);
      assert_tuple(state.active, zeros);
      assert_terminal(&state);
    }
  }
}

static void all_tuples(void) {
  unsigned tuples = 0;
  for (int a = -3; a <= 3; ++a) {
    for (int b = -3; b <= 3; ++b) {
      for (int c = -3; c <= 3; ++c) {
        for (int d = -3; d <= 3; ++d) {
          TrimStorageState state;
          Eeprom eeprom = {0};
          int8_t target[TRIM_COUNT] = {(int8_t)a, (int8_t)b, (int8_t)c, (int8_t)d};
          int8_t current[TRIM_COUNT];
          memcpy(current, previous, sizeof current);
          assert(trim_record_encode(current, eeprom.bytes));
          boot(&state, &eeprom, TRIM_VERIFIED, current);
          for (unsigned slot = 0; slot < TRIM_COUNT; ++slot) {
            int8_t desired[TRIM_COUNT];
            uint8_t saved[TRIM_RECORD_SIZE];
            memcpy(desired, current, sizeof desired);
            memcpy(saved, eeprom.bytes, sizeof saved);
            desired[slot] = target[slot];
            reset_log(&eeprom);
            assert(trim_storage_begin_set(&state, desired));
            finish(&state, &eeprom, current);
            assert(state.status == TRIM_VERIFIED);
            assert_tuple(state.active, desired);
            assert_trace(&eeprom, current[slot] == desired[slot] ? TRIM_COUNT : slot,
                         eeprom.bytes[slot]);
            for (unsigned other = 0; other < TRIM_COUNT; ++other) {
              if (other != slot) {
                assert(eeprom.bytes[other] == saved[other]);
              }
            }
            memcpy(current, desired, sizeof current);
          }
          boot(&state, &eeprom, TRIM_VERIFIED, target);
          ++tuples;
        }
      }
    }
  }
  assert(tuples == 2401);
}

static void failures_and_cancellation(void) {
  for (unsigned cut = 0; cut < TRIM_STORAGE_MAX_OPERATIONS; ++cut) {
    for (unsigned kind = 0; kind < 3; ++kind) {
      TrimStorageState state;
      Eeprom eeprom = {0};
      prepare_set(&state, &eeprom);
      for (unsigned i = 0; i < cut; ++i) {
        step(&state, &eeprom);
      }
      if (kind == 0) {
        TrimStorageOperation operation = request(&state, &eeprom);
        complete(&state, &eeprom, operation, false, false, 0);
      } else {
        if (kind == 2) {
          request(&state, &eeprom);
        }
        assert(trim_storage_cancel(&state));
      }
      assert(state.status == TRIM_UNCERTAIN);
      assert_tuple(state.active, previous);
      assert(state.write_issued == (cut > 8 || (cut == 8 && kind != 1)));
      assert_terminal(&state);
      assert(!trim_storage_begin_set(&state, requested));
    }
  }
  for (unsigned issued = 0; issued < 2; ++issued) {
    for (unsigned cut = 0; cut < TRIM_STORAGE_BOOT_READS; ++cut) {
      TrimStorageState state;
      Eeprom eeprom = {0};
      trim_storage_init(&state);
      assert(trim_storage_begin_boot(&state));
      for (unsigned i = 0; i < cut; ++i) {
        step(&state, &eeprom);
      }
      if (issued) {
        request(&state, &eeprom);
      }
      assert(trim_storage_cancel(&state));
      assert(state.status == TRIM_FAULT && !state.write_issued);
      assert_tuple(state.active, zeros);
      assert_terminal(&state);
    }
  }
}

static void readback_failures(void) {
  for (unsigned position = 0; position < TRIM_STORAGE_MAX_OPERATIONS; ++position) {
    TrimStorageState state;
    Eeprom eeprom = {0};
    prepare_set(&state, &eeprom);
    while (state.mode != TRIM_STORAGE_TERMINAL) {
      TrimStorageOperation operation = request(&state, &eeprom);
      bool inject = eeprom.count - 1 == position;
      /* At the write position, model an ACK with no programming effect. */
      complete(&state, &eeprom, operation, true, !inject,
               eeprom.bytes[operation.offset] ^ (inject ? 1 : 0));
    }
    assert(state.status == TRIM_UNCERTAIN);
    assert_tuple(state.active, previous);
    assert(eeprom.writes <= 1);
    assert_terminal(&state);
  }
  TrimStorageState state;
  Eeprom eeprom = {0};
  prepare_set(&state, &eeprom);
  eeprom.bytes[1] = 0x78; /* A stable external change invalidates the RAM snapshot. */
  finish(&state, &eeprom, previous);
  assert(state.status == TRIM_UNCERTAIN && eeprom.writes == 0);
}

static void identity_and_admission(void) {
  TrimStorageState state;
  Eeprom eeprom = {0};
  prepare_set(&state, &eeprom);
  TrimStorageOperation operation = request(&state, &eeprom);
  for (unsigned field = 0; field < 3; ++field) {
    TrimStorageOperation wrong = operation;
    if (field == 0) {
      ++wrong.token;
    } else if (field == 1) {
      wrong.kind = TRIM_STORAGE_WRITE;
    } else {
      ++wrong.offset;
    }
    TrimStorageState unchanged = state;
    assert(!trim_storage_complete(&state, &wrong, true, 0));
    assert_state_equal(&state, &unchanged);
  }
  TrimStorageState unchanged = state;
  assert(!trim_storage_begin_boot(&state));
  assert(!trim_storage_begin_set(&state, requested));
  assert_state_equal(&state, &unchanged);
  complete(&state, &eeprom, operation, true, false, eeprom.bytes[operation.offset]);
  unchanged = state;
  assert(!trim_storage_complete(&state, &operation, true, 0));
  assert_state_equal(&state, &unchanged);
  finish(&state, &eeprom, previous);
  for (unsigned field = 0; field < TRIM_COUNT; ++field) {
    int8_t invalid[TRIM_COUNT];
    memcpy(invalid, requested, sizeof invalid);
    invalid[field] = 4;
    unchanged = state;
    assert(!trim_storage_begin_set(&state, invalid));
    assert_state_equal(&state, &unchanged);
  }
  int8_t multiple[TRIM_COUNT] = {1, 2, 1, 2};
  unchanged = state;
  assert(!trim_storage_begin_set(&state, multiple));
  assert_state_equal(&state, &unchanged);
  prepare_set(&state, &eeprom);
  for (unsigned i = 0; i < 8; ++i) {
    step(&state, &eeprom);
  }
  operation = request(&state, &eeprom);
  TrimStorageOperation wrong = operation;
  wrong.value ^= 1;
  unchanged = state;
  assert(!trim_storage_complete(&state, &wrong, true, 0));
  assert_state_equal(&state, &unchanged);
  assert(trim_storage_cancel(&state));
  prepare_set(&state, &eeprom);
  state.token = UINT32_MAX;
  operation = request(&state, &eeprom);
  assert(operation.token == 1);
  wrong = operation;
  wrong.token = UINT32_MAX;
  assert(!trim_storage_complete(&state, &wrong, true, 0));
}

static void interrupted_byte_writes(void) {
  static const uint8_t codes[] = {0x1e, 0x2d, 0x33, 0x4b, 0x55, 0x66, 0x78};
  unsigned cases = 0;
  for (unsigned slot = 0; slot < TRIM_COUNT; ++slot) {
    for (int old = -3; old <= 3; ++old) {
      for (int value = -3; value <= 3; ++value) {
        if (old == value) {
          continue;
        }
        for (unsigned torn = 0; torn <= 255; ++torn) {
          TrimStorageState state;
          Eeprom eeprom = {0};
          int8_t before[TRIM_COUNT], desired[TRIM_COUNT], expected[TRIM_COUNT];
          memcpy(before, previous, sizeof before);
          before[slot] = (int8_t)old;
          memcpy(desired, before, sizeof desired);
          desired[slot] = (int8_t)value;
          assert(trim_record_encode(before, eeprom.bytes));
          boot(&state, &eeprom, TRIM_VERIFIED, before);
          reset_log(&eeprom);
          assert(trim_storage_begin_set(&state, desired));
          for (unsigned i = 0; i < 8; ++i) {
            step(&state, &eeprom);
          }
          TrimStorageOperation operation = request(&state, &eeprom);
          assert(operation.kind == TRIM_STORAGE_WRITE && operation.offset == slot);
          /* Byte-fault model only; this does not prove physical page isolation. */
          eeprom.bytes[slot] = (uint8_t)torn;
          memcpy(expected, before, sizeof expected);
          unsigned code = 0;
          while (code < sizeof codes && codes[code] != torn) {
            ++code;
          }
          expected[slot] = code < sizeof codes ? (int8_t)((int)code - 3) : 0;
          boot(&state, &eeprom, code < sizeof codes ? TRIM_VERIFIED : TRIM_DEFAULT, expected);
          ++cases;
        }
      }
    }
  }
  assert(cases == 43008);
  printf("Interrupted single-byte write cases: %u\n", cases);
}

int main(void) {
  boot_records();
  all_tuples();
  failures_and_cancellation();
  readback_failures();
  identity_and_admission();
  interrupted_byte_writes();
  puts("Independent trim storage checks passed");
  return 0;
}
