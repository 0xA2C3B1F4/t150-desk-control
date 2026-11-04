#include "motion_trim_owner.h"
#include "motion_trim_storage.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const TrimOwnerLimits limits = {1500, 10, 20};
static const int8_t old_trims[TRIM_COUNT] = {-3, 0, 1, 2};
static const int8_t new_trims[TRIM_COUNT] = {3, 0, 1, 2};
static const uint8_t snapshot_a[TRIM_NATIVE_BYTES] = {0, 100, 0, 163, 0, 0};
static const uint8_t snapshot_b[TRIM_NATIVE_BYTES] = {0, 101, 0, 163, 0, 0};
static const uint8_t snapshot_c[TRIM_NATIVE_BYTES] = {0, 99, 0, 163, 0, 0};

typedef struct {
  TrimOwnerState owner;
  TrimStorageState storage;
  TrimStorageOperation record_operation;
  uint8_t record[TRIM_RECORD_SIZE];
  uint8_t native[TRIM_NATIVE_BYTES];
  uint32_t now;
  unsigned trim_steps;
  unsigned native_writes;
  unsigned native_reads;
  unsigned cleanups;
  bool trim_byte_needs_cleanup;
} Rig;

static void assert_operation_equal(const TrimOwnerOperation* first,
                                   const TrimOwnerOperation* second) {
  assert(first->token == second->token);
  assert(first->kind == second->kind);
  assert(first->index == second->index);
  assert(first->value == second->value);
}

static void assert_state_equal(const TrimOwnerState* first, const TrimOwnerState* second) {
  assert(first->limits.window == second->limits.window);
  assert(first->limits.byte_limit == second->limits.byte_limit);
  assert(first->limits.cleanup_limit == second->limits.cleanup_limit);
  assert_operation_equal(&first->operation, &second->operation);
  assert(first->started == second->started);
  assert(first->issued_at == second->issued_at);
  assert(first->issued_limit == second->issued_limit);
  assert(first->token == second->token);
  assert(first->generation == second->generation);
  assert(memcmp(first->native_active, second->native_active, TRIM_NATIVE_BYTES) == 0);
  assert(memcmp(first->native_pending, second->native_pending, TRIM_NATIVE_BYTES) == 0);
  assert(first->phase == second->phase);
  assert(first->result == second->result);
  assert(first->reason == second->reason);
  assert(first->outstanding == second->outstanding);
  assert(first->pending_valid == second->pending_valid);
  assert(first->active_valid == second->active_valid);
  assert(first->active_verified == second->active_verified);
  assert(first->native_index == second->native_index);
  assert(first->native_failed == second->native_failed);
  assert(first->trim_verified == second->trim_verified);
  assert(first->trim_cancelled == second->trim_cancelled);
  assert(first->bus_ready == second->bus_ready);
  assert(first->handoff == second->handoff);
}

static void init(Rig* rig, uint32_t now, uint8_t commit) {
  memset(rig, 0, sizeof *rig);
  rig->now = now;
  trim_owner_init(&rig->owner);
  trim_storage_init(&rig->storage);
  memcpy(rig->storage.active, old_trims, sizeof old_trims);
  rig->storage.status = TRIM_VERIFIED;
  assert(trim_record_encode(old_trims, rig->record));
  if (commit != 0xa5) {
    rig->record[0] = commit;
  }
  rig->storage.status = trim_record_decode(rig->record, rig->record, rig->storage.active);
  assert(trim_storage_begin_set(&rig->storage, new_trims));
  assert(trim_owner_begin(&rig->owner, &limits, now));
}

static bool next(Rig* rig, TrimOwnerOperation* operation) {
  bool issued = trim_owner_next(&rig->owner, rig->now, operation);
  if (rig->owner.trim_cancelled) {
    trim_storage_cancel(&rig->storage);
  }
  if (issued && operation->kind == TRIM_OWNER_TRIM_STEP) {
    assert(trim_storage_next(&rig->storage, &rig->record_operation));
    rig->trim_byte_needs_cleanup = true;
  }
  if (issued && operation->kind == TRIM_OWNER_NATIVE_WRITE) {
    assert(!rig->trim_byte_needs_cleanup);
  }
  return issued;
}

static void finish_operation(Rig* rig, TrimOwnerOperation operation, bool succeed,
                             bool corrupt_read) {
  uint8_t result = succeed ? TRIM_OWNER_STEP_OK : TRIM_OWNER_STEP_FAILED;
  uint8_t value = 0;
  switch (operation.kind) {
    case TRIM_OWNER_TRIM_STEP:
      ++rig->trim_steps;
      if (succeed && rig->record_operation.kind == TRIM_STORAGE_WRITE) {
        rig->record[rig->record_operation.offset] = rig->record_operation.value;
      }
      value = rig->record[rig->record_operation.offset];
      if (rig->owner.trim_cancelled) {
        trim_storage_cancel(&rig->storage);
      } else {
        assert(trim_storage_complete(&rig->storage, &rig->record_operation, succeed, value));
        if (rig->storage.mode == TRIM_STORAGE_TERMINAL) {
          result = rig->storage.status == TRIM_VERIFIED ? TRIM_OWNER_TRIM_VERIFIED
                                                        : TRIM_OWNER_STEP_FAILED;
        }
      }
      break;
    case TRIM_OWNER_NATIVE_WRITE:
      assert(operation.index == rig->native_writes % TRIM_NATIVE_BYTES);
      ++rig->native_writes;
      if (succeed) {
        rig->native[operation.index] = operation.value;
      }
      break;
    case TRIM_OWNER_NATIVE_READ:
      ++rig->native_reads;
      value = rig->native[operation.index] ^ (corrupt_read ? 0xff : 0);
      break;
    case TRIM_OWNER_BUS_CLEANUP:
      ++rig->cleanups;
      if (succeed) {
        rig->trim_byte_needs_cleanup = false;
      }
      break;
    default:
      assert(false);
  }
  ++rig->now;
  assert(trim_owner_complete(&rig->owner, &operation, rig->now, result, value));
  TrimOwnerState once = rig->owner;
  assert(!trim_owner_complete(&rig->owner, &operation, rig->now, result, value));
  assert_state_equal(&once, &rig->owner);
}

static void run(Rig* rig) {
  for (unsigned i = 0; i < 200 && rig->owner.phase != TRIM_OWNER_TERMINAL; ++i) {
    TrimOwnerOperation operation;
    if (next(rig, &operation)) {
      finish_operation(rig, operation, true, false);
    }
  }
  assert(rig->owner.phase == TRIM_OWNER_TERMINAL);
}

static void terminal_unchanged(Rig* rig) {
  TrimOwnerState saved = rig->owner;
  TrimOwnerOperation untouched = {79, 88, 99, 111};
  TrimOwnerOperation operation = untouched;
  assert(!trim_owner_next(&rig->owner, rig->now, &operation));
  assert_operation_equal(&untouched, &operation);
  assert(!trim_owner_capture(&rig->owner, snapshot_c));
  assert_state_equal(&saved, &rig->owner);
}

static void normal_and_admission(void) {
  const TrimOwnerLimits invalid[] = {{0, 1, 1},    {0x80000000, 1, 1},    {1500, 0, 1},
                                     {1500, 1, 0}, {1500, UINT32_MAX, 1}, {1500, 1, UINT32_MAX},
                                     {15, 1, 1}};
  TrimOwnerState owner;
  trim_owner_init(&owner);
  for (unsigned i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) {
    TrimOwnerState saved = owner;
    assert(!trim_owner_begin(&owner, &invalid[i], 0));
    assert_state_equal(&owner, &saved);
  }
  for (unsigned wrapping = 0; wrapping < 2; ++wrapping) {
    Rig rig;
    init(&rig, wrapping ? UINT32_MAX - 20 : 0, 0xa5);
    TrimOwnerState saved = rig.owner;
    assert(!trim_owner_begin(&rig.owner, &limits, rig.now));
    assert_state_equal(&saved, &rig.owner);
    run(&rig);
    assert(rig.trim_steps == TRIM_STORAGE_MAX_OPERATIONS && rig.native_writes == 0 &&
           rig.native_reads == 0);
    assert(rig.cleanups == 1);
    assert(rig.owner.result == TRIM_OWNER_OK && rig.owner.handoff);
    assert(!trim_owner_native_unverified(&rig.owner));
    int8_t decoded[TRIM_COUNT];
    assert(trim_record_decode(rig.record, rig.record, decoded) == TRIM_VERIFIED);
    assert(memcmp(decoded, new_trims, sizeof decoded) == 0);
    terminal_unchanged(&rig);
    uint32_t previous_token = rig.owner.token;
    assert(trim_owner_begin(&rig.owner, &limits, rig.now));
    TrimOwnerOperation action;
    assert(trim_owner_next(&rig.owner, rig.now, &action));
    assert(action.token > previous_token);
  }
}

static void preempt_every_record_boundary(void) {
  const uint8_t commits[] = {0xa5, 0, 0xff};
  unsigned cases = 0;
  for (unsigned record = 0; record < sizeof commits; ++record) {
    unsigned steps = TRIM_STORAGE_MAX_OPERATIONS;
    for (unsigned boundary = 0; boundary <= steps; ++boundary) {
      for (unsigned in_flight = 0; in_flight < 2; ++in_flight) {
        Rig rig;
        init(&rig, 0, commits[record]);
        TrimOwnerOperation action;
        for (unsigned i = 0; i < boundary; ++i) {
          assert(next(&rig, &action));
          assert(action.kind == TRIM_OWNER_TRIM_STEP);
          finish_operation(&rig, action, true, false);
        }
        if (in_flight) {
          assert(next(&rig, &action));
        }
        assert(trim_owner_capture(&rig.owner, snapshot_a));
        assert(rig.owner.trim_cancelled);
        if (in_flight) {
          finish_operation(&rig, action, true, false);
        }
        run(&rig);
        assert(rig.owner.result == TRIM_OWNER_STORAGE_ERROR && rig.owner.handoff);
        assert(rig.owner.reason == TRIM_OWNER_PREEMPTED);
        assert(!trim_owner_native_unverified(&rig.owner));
        assert(memcmp(rig.native, snapshot_a, sizeof rig.native) == 0);
        assert(rig.native_writes == 6 && rig.native_reads == 6);
        terminal_unchanged(&rig);
        assert(!trim_owner_begin(&rig.owner, &limits, rig.now));
        ++cases;
      }
    }
  }
  assert(cases == 72);
}

static void start_native(Rig* rig) {
  init(rig, 0, 0xa5);
  assert(trim_owner_capture(&rig->owner, snapshot_a));
  TrimOwnerOperation action;
  assert(!next(rig, &action));
  assert(rig->owner.phase == TRIM_OWNER_DRAIN && rig->owner.generation == 1);
}

static void native_updates_and_failures(void) {
  for (unsigned position = 0; position <= 12; ++position) {
    for (unsigned variant = 0; variant < 3; ++variant) {
      Rig rig;
      start_native(&rig);
      TrimOwnerOperation action;
      for (unsigned i = 0; i < position; ++i) {
        assert(next(&rig, &action));
        finish_operation(&rig, action, true, false);
      }
      assert(next(&rig, &action));
      assert(trim_owner_capture(&rig.owner, snapshot_b));
      const uint8_t* latest = variant == 0 ? snapshot_a : variant == 1 ? snapshot_b : snapshot_c;
      assert(trim_owner_capture(&rig.owner, latest));
      assert(memcmp(rig.owner.native_active, snapshot_a, TRIM_NATIVE_BYTES) == 0);
      finish_operation(&rig, action, true, false);
      run(&rig);
      assert(rig.native_writes == (variant == 0 ? 6u : 12u));
      assert(rig.native_reads == rig.native_writes);
      assert(memcmp(rig.native, latest, TRIM_NATIVE_BYTES) == 0);
      assert(!trim_owner_native_unverified(&rig.owner));
      assert(rig.owner.result == TRIM_OWNER_STORAGE_ERROR && rig.owner.handoff);
    }
  }
  for (unsigned position = 0; position < 12; ++position) {
    for (unsigned mismatch = 0; mismatch < (position >= 6 ? 2u : 1u); ++mismatch) {
      Rig rig;
      start_native(&rig);
      TrimOwnerOperation action;
      for (unsigned i = 0; i < position; ++i) {
        assert(next(&rig, &action));
        finish_operation(&rig, action, true, false);
      }
      assert(next(&rig, &action));
      assert(trim_owner_capture(&rig.owner, snapshot_c));
      finish_operation(&rig, action, mismatch != 0, mismatch != 0);
      run(&rig);
      assert(rig.owner.result == TRIM_OWNER_STORAGE_ERROR && rig.owner.handoff);
      assert(rig.owner.reason == (mismatch ? TRIM_OWNER_VERIFY_ERROR : TRIM_OWNER_NATIVE_ERROR));
      assert(trim_owner_native_unverified(&rig.owner));
      assert(memcmp(rig.owner.native_pending, snapshot_c, TRIM_NATIVE_BYTES) == 0);
      assert(rig.native_writes == (position < 6 ? position + 1 : 6));
    }
  }
}

static void partial_write_invalidates_previous_image(void) {
  Rig rig;
  start_native(&rig);
  TrimOwnerOperation action;
  for (unsigned i = 0; i < 12; ++i) {
    assert(next(&rig, &action));
    finish_operation(&rig, action, true, false);
  }
  assert(rig.owner.active_verified);
  assert(trim_owner_capture(&rig.owner, snapshot_b));
  assert(next(&rig, &action) && action.kind == TRIM_OWNER_BUS_CLEANUP);
  finish_operation(&rig, action, true, false);
  assert(!rig.owner.active_verified);
  for (unsigned i = 0; i < 2; ++i) {
    assert(next(&rig, &action));
    finish_operation(&rig, action, true, false);
  }
  assert(rig.native[1] == snapshot_b[1]);
  assert(trim_owner_capture(&rig.owner, snapshot_a));
  assert(next(&rig, &action));
  finish_operation(&rig, action, false, false);
  run(&rig);
  assert(rig.owner.pending_valid && trim_owner_native_unverified(&rig.owner));
  assert(memcmp(rig.owner.native_pending, snapshot_a, TRIM_NATIVE_BYTES) == 0);
  assert(rig.owner.result == TRIM_OWNER_STORAGE_ERROR && rig.owner.handoff);
  assert(rig.native[1] == snapshot_b[1]);
}

static void deadlines_callbacks_and_cleanup(void) {
  for (unsigned wrap = 0; wrap < 2; ++wrap) {
    for (unsigned kind = 0; kind < 4; ++kind) {
      Rig rig;
      init(&rig, wrap ? UINT32_MAX - 5 : 0, 0xa5);
      TrimOwnerOperation action;
      assert(next(&rig, &action));
      if (kind == 1) {
        finish_operation(&rig, action, false, false);
        assert(next(&rig, &action) && action.kind == TRIM_OWNER_BUS_CLEANUP);
      } else if (kind >= 2) {
        assert(trim_owner_capture(&rig.owner, snapshot_a));
        finish_operation(&rig, action, true, false);
        assert(next(&rig, &action) && action.kind == TRIM_OWNER_BUS_CLEANUP);
        finish_operation(&rig, action, true, false);
        for (unsigned i = 0; i < (kind == 3 ? 6u : 0u); ++i) {
          assert(next(&rig, &action));
          finish_operation(&rig, action, true, false);
        }
        assert(next(&rig, &action));
      }
      TrimOwnerState saved = rig.owner;
      TrimOwnerOperation wrong = action;
      ++wrong.token;
      assert(!trim_owner_complete(&rig.owner, &wrong, rig.now, TRIM_OWNER_STEP_OK, 0));
      assert_state_equal(&saved, &rig.owner);
      assert(!next(&rig, &wrong));
      assert_state_equal(&saved, &rig.owner);
      rig.now = rig.owner.issued_at + rig.owner.issued_limit + 1;
      assert(!next(&rig, &wrong));
      assert(rig.owner.result == TRIM_OWNER_UNRELEASED && !rig.owner.handoff);
      assert(rig.owner.outstanding);
      saved = rig.owner;
      assert(trim_owner_complete(&rig.owner, &action, rig.now, TRIM_OWNER_STEP_OK, 0));
      saved.outstanding = 0;
      assert_state_equal(&saved, &rig.owner);
      assert(!trim_owner_complete(&rig.owner, &action, rig.now, TRIM_OWNER_STEP_OK, 0));
      assert(!trim_owner_begin(&rig.owner, &limits, rig.now));
      terminal_unchanged(&rig);
    }
  }
  Rig rig;
  init(&rig, 0, 0xa5);
  TrimOwnerOperation action;
  assert(next(&rig, &action));
  finish_operation(&rig, action, false, false);
  assert(trim_owner_capture(&rig.owner, snapshot_b));
  run(&rig);
  assert(rig.native_writes == 6 && !trim_owner_native_unverified(&rig.owner));

  init(&rig, 0, 0xa5);
  assert(next(&rig, &action));
  finish_operation(&rig, action, false, false);
  assert(next(&rig, &action));
  finish_operation(&rig, action, false, false);
  assert(rig.owner.result == TRIM_OWNER_UNRELEASED && !rig.owner.handoff);
  assert(rig.owner.reason == TRIM_OWNER_CLEANUP_ERROR);
  terminal_unchanged(&rig);

  init(&rig, 0, 0xa5);
  assert(trim_owner_capture(&rig.owner, snapshot_b));
  rig.now = limits.window - 12 * limits.byte_limit - limits.cleanup_limit;
  assert(!next(&rig, &action));
  assert(rig.owner.result == TRIM_OWNER_STORAGE_ERROR && rig.owner.handoff);
  assert(rig.owner.reason == TRIM_OWNER_DEADLINE && rig.owner.pending_valid);

  init(&rig, 0, 0xa5);
  assert(next(&rig, &action));
  rig.now = limits.window;
  assert(trim_owner_complete(&rig.owner, &action, rig.now, TRIM_OWNER_TRIM_VERIFIED, 0));
  assert(rig.owner.result == TRIM_OWNER_UNRELEASED && !rig.owner.outstanding);
  assert(!rig.owner.trim_verified && !rig.owner.handoff);
}

static void counters_and_final_boundary(void) {
  Rig rig;
  init(&rig, 0, 0xa5);
  assert(trim_owner_capture(&rig.owner, snapshot_a));
  assert(trim_owner_capture(&rig.owner, snapshot_b));
  rig.owner.generation = UINT32_MAX;
  TrimOwnerOperation action;
  assert(!next(&rig, &action));
  assert(rig.owner.result == TRIM_OWNER_STORAGE_ERROR && rig.owner.handoff);
  assert(rig.owner.reason == TRIM_OWNER_IDENTITY_EXHAUSTED);
  assert(rig.owner.pending_valid);

  init(&rig, 0, 0xa5);
  rig.owner.token = UINT32_MAX - 2;
  assert(next(&rig, &action));
  finish_operation(&rig, action, true, false);
  assert(next(&rig, &action) && action.kind == TRIM_OWNER_BUS_CLEANUP);
  assert(action.token == UINT32_MAX);
  finish_operation(&rig, action, true, false);
  assert(rig.owner.result == TRIM_OWNER_STORAGE_ERROR && rig.owner.handoff);
  assert(rig.owner.reason == TRIM_OWNER_IDENTITY_EXHAUSTED);

  for (unsigned before = 0; before < 2; ++before) {
    start_native(&rig);
    for (unsigned i = 0; i < 12; ++i) {
      assert(next(&rig, &action));
      finish_operation(&rig, action, true, false);
    }
    assert(next(&rig, &action) && action.kind == TRIM_OWNER_BUS_CLEANUP);
    if (before) {
      assert(trim_owner_capture(&rig.owner, snapshot_c));
    }
    finish_operation(&rig, action, true, false);
    if (before) {
      assert(!rig.owner.handoff);
      run(&rig);
      assert(memcmp(rig.native, snapshot_c, TRIM_NATIVE_BYTES) == 0);
    } else {
      assert(rig.owner.handoff);
      assert(!trim_owner_capture(&rig.owner, snapshot_c));
    }
  }
}

static void repeated_native_requests_hit_finite_deadline(void) {
  for (unsigned wrap = 0; wrap < 2; ++wrap) {
    Rig rig;
    init(&rig, wrap ? UINT32_MAX - 20 : 0, 0xa5);
    uint8_t latest[TRIM_NATIVE_BYTES] = {0, 0, 0, 163, 0, 0};
    unsigned iterations = 0;
    while (rig.owner.phase != TRIM_OWNER_TERMINAL && iterations < 400) {
      ++iterations;
      latest[1] = (uint8_t)iterations;
      assert(trim_owner_capture(&rig.owner, latest));
      TrimOwnerOperation action;
      if (next(&rig, &action)) {
        rig.now += rig.owner.issued_limit - 1;
        finish_operation(&rig, action, true, false);
      }
    }
    assert(iterations < 400 && rig.owner.generation > 1);
    assert(rig.owner.phase == TRIM_OWNER_TERMINAL);
    assert(rig.owner.result == TRIM_OWNER_STORAGE_ERROR && rig.owner.handoff);
    assert(rig.owner.reason == TRIM_OWNER_DEADLINE);
    assert(rig.owner.pending_valid && trim_owner_native_unverified(&rig.owner));
    assert(memcmp(rig.owner.native_pending, latest, sizeof latest) == 0);
    assert(rig.now - rig.owner.started < limits.window);
    terminal_unchanged(&rig);
  }
}

static void completion_validation_and_exact_boundaries(void) {
  Rig rig;
  start_native(&rig);
  TrimOwnerOperation action;
  assert(next(&rig, &action));
  for (unsigned field = 0; field < 4; ++field) {
    TrimOwnerOperation wrong = action;
    if (field == 0) {
      ++wrong.token;
    } else if (field == 1) {
      ++wrong.kind;
    } else if (field == 2) {
      ++wrong.index;
    } else {
      ++wrong.value;
    }
    TrimOwnerState saved = rig.owner;
    assert(!trim_owner_complete(&rig.owner, &wrong, rig.now, TRIM_OWNER_STEP_OK, 0));
    assert_state_equal(&saved, &rig.owner);
  }
  for (unsigned result = TRIM_OWNER_TRIM_VERIFIED; result <= UINT8_MAX; ++result) {
    TrimOwnerState saved = rig.owner;
    assert(!trim_owner_complete(&rig.owner, &action, rig.now, (uint8_t)result, 0));
    assert_state_equal(&saved, &rig.owner);
  }
  for (unsigned lateness = 0; lateness < 2; ++lateness) {
    init(&rig, UINT32_MAX - 5, 0xa5);
    assert(next(&rig, &action));
    rig.now += limits.byte_limit + lateness;
    assert(trim_owner_complete(&rig.owner, &action, rig.now, TRIM_OWNER_STEP_OK, 0));
    assert(!rig.owner.outstanding);
    if (lateness) {
      assert(rig.owner.result == TRIM_OWNER_UNRELEASED && !rig.owner.handoff);
    } else {
      assert(rig.owner.phase == TRIM_OWNER_TRIM && rig.owner.result == TRIM_OWNER_PENDING);
    }
  }
  /* A quiet owner that never started a byte can release cleanly at expiry. */
  init(&rig, 0, 0xa5);
  rig.now = limits.window;
  assert(!next(&rig, &action));
  assert(rig.owner.result == TRIM_OWNER_STORAGE_ERROR && rig.owner.handoff);

  /* Once a byte was issued, exhausted cleanup time grants no handoff. */
  init(&rig, 0, 0xa5);
  assert(next(&rig, &action));
  finish_operation(&rig, action, true, false);
  rig.now = limits.window - limits.cleanup_limit;
  assert(!next(&rig, &action));
  assert(rig.owner.result == TRIM_OWNER_UNRELEASED && !rig.owner.handoff);
  assert(rig.owner.reason == TRIM_OWNER_DEADLINE);
}

static void optional_budget_still_allows_native_save(void) {
  for (unsigned fits = 0; fits < 2; ++fits) {
    Rig rig;
    init(&rig, 0, 0xa5);
    TrimOwnerOperation action;
    assert(next(&rig, &action));
    finish_operation(&rig, action, true, false);
    rig.now = fits ? 1330 : 1350;
    assert(next(&rig, &action) && action.kind == TRIM_OWNER_BUS_CLEANUP);
    assert(rig.owner.reason == TRIM_OWNER_TRIM_BUDGET && !rig.owner.native_failed);
    assert(trim_owner_capture(&rig.owner, snapshot_a));
    rig.now += 9;
    finish_operation(&rig, action, true, false);
    run(&rig);
    assert(rig.owner.result == TRIM_OWNER_STORAGE_ERROR && rig.owner.handoff);
    assert(rig.native_writes == (fits ? 6u : 0u));
    if (fits) {
      assert(memcmp(rig.native, snapshot_a, TRIM_NATIVE_BYTES) == 0);
      assert(!trim_owner_native_unverified(&rig.owner));
      assert(rig.owner.reason == TRIM_OWNER_TRIM_BUDGET);
    } else {
      assert(rig.owner.pending_valid && trim_owner_native_unverified(&rig.owner));
      assert(rig.owner.reason == TRIM_OWNER_DEADLINE);
    }
  }
}

static void unverified_without_pending_and_cleanup_failure(void) {
  for (unsigned failure = 0; failure < 3; ++failure) {
    Rig rig;
    start_native(&rig);
    TrimOwnerOperation action;
    unsigned position = failure == 0 ? 2 : 8;
    for (unsigned i = 0; i < position; ++i) {
      assert(next(&rig, &action));
      finish_operation(&rig, action, true, false);
    }
    if (failure == 2) {
      rig.now = limits.window - 4 * limits.byte_limit - limits.cleanup_limit;
      assert(next(&rig, &action) && action.kind == TRIM_OWNER_BUS_CLEANUP);
      finish_operation(&rig, action, true, false);
    } else {
      assert(next(&rig, &action));
      finish_operation(&rig, action, failure != 0, failure != 0);
    }
    run(&rig);
    assert(!rig.owner.pending_valid && trim_owner_native_unverified(&rig.owner));
    assert(rig.owner.active_valid && !rig.owner.active_verified);
    assert(memcmp(rig.owner.native_active, snapshot_a, TRIM_NATIVE_BYTES) == 0);
    assert(rig.owner.handoff && rig.owner.result == TRIM_OWNER_STORAGE_ERROR);
  }

  Rig rig;
  start_native(&rig);
  TrimOwnerOperation action;
  for (unsigned i = 0; i < 12; ++i) {
    assert(next(&rig, &action));
    finish_operation(&rig, action, true, false);
  }
  assert(next(&rig, &action) && action.kind == TRIM_OWNER_BUS_CLEANUP);
  assert(trim_owner_capture(&rig.owner, snapshot_c));
  finish_operation(&rig, action, false, false);
  assert(rig.owner.result == TRIM_OWNER_UNRELEASED && !rig.owner.handoff);
  assert(rig.owner.reason == TRIM_OWNER_CLEANUP_ERROR);
  assert(trim_owner_native_unverified(&rig.owner));
  assert(memcmp(rig.owner.native_pending, snapshot_c, TRIM_NATIVE_BYTES) == 0);
  terminal_unchanged(&rig);
}

static void reserve_acceptance_and_expiry_between_operations(void) {
  TrimOwnerState state;
  const TrimOwnerLimits smallest = {16, 1, 1};
  const TrimOwnerLimits largest = {0x7fffffff, 1, 1};
  trim_owner_init(&state);
  assert(trim_owner_begin(&state, &smallest, 0));
  trim_owner_init(&state);
  assert(trim_owner_begin(&state, &largest, UINT32_MAX - 3));

  for (unsigned accept = 0; accept < 2; ++accept) {
    Rig rig;
    init(&rig, 0, 0xa5);
    TrimOwnerOperation action;
    rig.now = limits.window - (13 * limits.byte_limit + 2 * limits.cleanup_limit) - accept;
    assert(next(&rig, &action) == (accept != 0));
    if (accept) {
      assert(action.kind == TRIM_OWNER_TRIM_STEP);
    } else {
      assert(rig.owner.handoff && rig.owner.reason == TRIM_OWNER_TRIM_BUDGET);
    }

    start_native(&rig);
    rig.now = limits.window - (12 * limits.byte_limit + limits.cleanup_limit) - accept;
    assert(next(&rig, &action) == (accept != 0));
    if (accept) {
      assert(action.kind == TRIM_OWNER_NATIVE_WRITE);
    } else {
      assert(rig.owner.handoff && rig.owner.reason == TRIM_OWNER_DEADLINE);
      assert(!rig.owner.pending_valid && trim_owner_native_unverified(&rig.owner));
    }

    init(&rig, 0, 0xa5);
    assert(trim_owner_capture(&rig.owner, snapshot_a));
    rig.now = limits.window - (12 * limits.byte_limit + limits.cleanup_limit) - accept;
    assert(!next(&rig, &action));
    assert(rig.owner.phase == (accept ? TRIM_OWNER_DRAIN : TRIM_OWNER_TERMINAL));

    init(&rig, 0, 0xa5);
    assert(next(&rig, &action));
    assert(trim_owner_complete(&rig.owner, &action, 1, TRIM_OWNER_TRIM_VERIFIED, 0));
    rig.now = limits.window - limits.cleanup_limit - accept;
    assert(next(&rig, &action) == (accept != 0));
    if (accept) {
      assert(action.kind == TRIM_OWNER_BUS_CLEANUP);
      assert(trim_owner_complete(&rig.owner, &action, limits.window - 1, TRIM_OWNER_STEP_OK, 0));
      assert(rig.owner.result == TRIM_OWNER_OK && rig.owner.handoff);
    } else {
      assert(rig.owner.result == TRIM_OWNER_UNRELEASED && !rig.owner.handoff);
    }
  }

  for (unsigned wrap = 0; wrap < 2; ++wrap) {
    for (unsigned native = 0; native < 2; ++native) {
      Rig rig;
      uint32_t start = wrap ? UINT32_MAX - 20 : 0;
      init(&rig, start, 0xa5);
      TrimOwnerOperation action;
      if (native) {
        assert(trim_owner_capture(&rig.owner, snapshot_a));
        assert(!next(&rig, &action));
      }
      assert(next(&rig, &action));
      finish_operation(&rig, action, true, false);
      assert(!rig.owner.outstanding && !rig.owner.bus_ready);
      rig.now = start + limits.window;
      assert(!next(&rig, &action));
      assert(rig.owner.result == TRIM_OWNER_UNRELEASED && !rig.owner.handoff);
      terminal_unchanged(&rig);
    }
  }
}

int main(void) {
  normal_and_admission();
  preempt_every_record_boundary();
  native_updates_and_failures();
  partial_write_invalidates_previous_image();
  deadlines_callbacks_and_cleanup();
  counters_and_final_boundary();
  repeated_native_requests_hit_finite_deadline();
  completion_validation_and_exact_boundaries();
  optional_budget_still_allows_native_save();
  unverified_without_pending_and_cleanup_failure();
  reserve_acceptance_and_expiry_between_operations();
  printf("Motion trim owner tests passed; state=%zu bytes\n", sizeof(TrimOwnerState));
  return 0;
}
