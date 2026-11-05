#ifdef DESK_DISPLAY_TIMEOUT
#include "display_controller.h"
#endif
#ifdef DESK_LOCAL_PANEL_LOCK
#include "local_lock_controller.h"
#endif
#include "motion_trim_controller.h"
#include "motion_trim_motion.h"
#include "motion_trim_native.h"
#include "platform_runtime.h"

#define BYTE(offset) (*(volatile uint8_t*)(0x20000000u + (offset)))
#define HALF(offset) (*(volatile uint16_t*)(0x20000000u + (offset)))
#define WORD(offset) (*(volatile uint32_t*)(0x20000000u + (offset)))
#define REGISTER(address) (*(volatile uint32_t*)(address))
#define ADAPTER ((TrimAdapterState*)PLATFORM_TRIM_ADAPTER_ADDRESS)
#define CONTROL ((TrimControllerState*)PLATFORM_TRANSACTION_ADDRESS)
#define CONTROL_MAGIC 0x54524331u

enum { CONTROL_BOOT, CONTROL_IDLE, CONTROL_SET, CONTROL_REPLY };
enum { REPLY_OK, REPLY_BAD, REPLY_STALE, REPLY_BUSY, REPLY_GUARD, REPLY_STORAGE_ERROR };
enum { REPLY_CONTROLS_LOCKED = 1, REPLY_PLATFORM_FAULT = 2 };

typedef struct {
  uint32_t magic;
  uint32_t reply_started;
  uint16_t nonce;
  uint8_t phase;
  uint8_t reserved;
  uint8_t release_gate;
  uint8_t publish_pending;
  uint8_t reply[16];
} TrimControllerState;

_Static_assert(sizeof(TrimControllerState) <= PLATFORM_TRANSACTION_SIZE,
               "Trim protocol exceeds its platform allocation");

extern void trim_original_reset_scanners(void);

static uint32_t mask(void) {
  uint32_t previous;
  __asm volatile("mrs %0, PRIMASK\ncpsid i" : "=r"(previous)::"memory");
  return previous;
}

static void restore(uint32_t previous) {
  __asm volatile("msr PRIMASK, %0" ::"r"(previous) : "memory");
}

bool trim_controller_ready(void) {
  return platform_ready() && CONTROL->magic == CONTROL_MAGIC;
}

bool trim_controller_started(void) {
  /* Every application hook follows platform_bootstrap's full-slot writes.
   * Header/guard faults may withdraw healthy admission while an intact adapter
   * still owes cleanup. Check the physical RAM profile before reading its slot. */
  return platform_profile_matches() && CONTROL->magic == CONTROL_MAGIC;
}

bool trim_controller_activate(void) {
  if (!platform_ready()) {
    return false;
  }
  uint32_t previous = mask();
  bool valid = CONTROL->magic != CONTROL_MAGIC &&
               ADAPTER->owner == (TrimOwnerState*)PLATFORM_TRIM_OWNER_ADDRESS &&
               ADAPTER->record == (TrimStorageState*)PLATFORM_TRIM_RECORD_ADDRESS &&
               ADAPTER->driver == (TrimI2cState*)PLATFORM_TRIM_I2C_ADDRESS &&
               ADAPTER->budget.owner.window == TRIM_STORAGE_WINDOW &&
               ADAPTER->mode == TRIM_STORAGE_BOOT && ADAPTER->phase == TRIM_ADAPTER_WAIT;
  if (valid) {
    *CONTROL = (TrimControllerState){0};
    CONTROL->phase = CONTROL_BOOT;
    CONTROL->release_gate = 1;
    trim_original_reset_scanners();
    CONTROL->magic = CONTROL_MAGIC;
  }
  restore(previous);
  return valid;
}

static bool escaped(uint8_t value) {
  return value == 0xfa || value == 0xfe || value == 0xfd;
}

static uint8_t room(void) {
  return (uint8_t)(BYTE(0xf4) - BYTE(0xf5) - 1);
}

/* The caller holds the interrupt mask. Bytes are invisible to the consumer
 * until the final head store, including when the frame wraps around the ring. */
static bool enqueue(const volatile uint8_t* body, uint8_t length, uint8_t reserve) {
  if (!length) {
    return false;
  }
  uint8_t checksum = body[0];
  unsigned count = 4;
  for (unsigned i = 1; i < length; ++i) {
    checksum ^= body[i];
    count += 1 + escaped(body[i]);
  }
  count += escaped(checksum);
  if (count + reserve > room()) {
    return false;
  }
  volatile uint8_t* output = (volatile uint8_t*)WORD(0xf0);
  uint8_t head = BYTE(0xf5);
  output[head++] = 0xfa;
  output[head++] = body[0];
  for (unsigned i = 1; i <= length; ++i) {
    uint8_t value = i == length ? checksum : body[i];
    if (escaped(value)) {
      output[head++] = 0xfe;
    }
    output[head++] = value;
  }
  output[head++] = 0xfd;
  BYTE(0xf5) = head;
  return true;
}

bool trim_controller_tx_reserved(void) {
  return trim_controller_started() && CONTROL->reserved;
}

uint32_t trim_controller_send_native(volatile uint8_t* ring, const volatile uint8_t* frame) {
  uint32_t previous = mask();
  const volatile uint8_t* input = *(const volatile uint8_t* const volatile*)frame;
  uint8_t length = frame[4];
  uint8_t checksum = input[1];
  for (unsigned i = 1; i < length; ++i) {
    checksum ^= input[i + 1];
  }
  /* This hook is bound to the controller UART ring, not a general encoder. */
  if (ring == (volatile uint8_t*)0x200000f0u) {
    enqueue(input + 1, length, trim_controller_tx_reserved() ? TRIM_REPLY_RESERVE : 0);
  }
  restore(previous);
  return checksum;
}

#ifdef DESK_DISPLAY_TIMEOUT
bool trim_controller_reply_room(uint8_t needed) {
  unsigned reserve = trim_controller_tx_reserved() ? TRIM_REPLY_RESERVE : 0;
  return room() >= needed + reserve;
}

void trim_controller_reply(const uint8_t* reply, uint8_t length) {
  uint8_t reserve = trim_controller_tx_reserved() ? TRIM_REPLY_RESERVE : 0;
  if (enqueue(reply, length, reserve)) {
    HALF(0x120)++;
  }
}
#endif

static void snapshot(uint8_t* reply, uint8_t operation, uint16_t nonce, uint8_t result,
                     const TrimAdapterView* view) {
  reply[0] = 0xda;
  reply[1] = 1;
  reply[2] = 3;
  reply[3] = operation;
  reply[4] = result;
  reply[5] = view->storage;
  reply[6] = BYTE(0xfe2) <= 1 ? BYTE(0xfe2) : 0;
  bool healthy = trim_controller_ready();
  reply[7] = (view->closed && !view->handoff) || (!healthy && CONTROL->publish_pending)
                 ? REPLY_CONTROLS_LOCKED
                 : 0;
  if (!healthy) {
    reply[7] |= REPLY_PLATFORM_FAULT;
  }
  reply[8] = (uint8_t)(nonce >> 8);
  reply[9] = (uint8_t)nonce;
  for (unsigned i = 0; i < TRIM_COUNT; ++i) {
    reply[12 + i] = (uint8_t)view->active[i];
  }
}

static bool send_reply(uint8_t* reply, uint8_t reserve) {
  uint16_t sequence = HALF(0x120);
  reply[10] = (uint8_t)(sequence >> 8);
  reply[11] = (uint8_t)sequence;
  if (!enqueue(reply, 16, reserve)) {
    return false;
  }
  HALF(0x120) = (uint16_t)(sequence + 1);
  return true;
}

static bool raw_key(void) {
  return (REGISTER(0x48000010u) & 0x1f00u) != 0x1f00u ||
         (REGISTER(0x48000410u) & 0xc000u) != 0xc000u;
}

static uint8_t admission(void) {
  if (WORD(0xfe4) != 0x52533031u || BYTE(0xfca)) {
    return REPLY_GUARD;
  }
  if ((BYTE(0xfe0) != 0 && BYTE(0xfe0) != 4) || BYTE(0x88d) || BYTE(0x895) || HALF(0x75a) ||
      REGISTER(0x4000043cu) || REGISTER(0x40000440u) || raw_key()) {
    return REPLY_BUSY;
  }
  static const uint16_t guards[] = {0x88f, 0x890, 0x148, 0x8b8, 0x892, 0x894,
                                    0xaa0, 0xaa1, 0x18e, 0x8ba, 0x754};
  for (unsigned i = 0; i < sizeof(guards) / sizeof(guards[0]); ++i) {
    if (BYTE(guards[i])) {
      return REPLY_GUARD;
    }
  }
  for (unsigned i = 0x7ad; i <= 0x7b4; ++i) {
    if (BYTE(i)) {
      return REPLY_BUSY;
    }
  }
  static const uint16_t keys[] = {0x184, 0x185, 0x186, 0x187, 0x188, 0x189, 0x18a, 0x18d};
  for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
    if (BYTE(keys[i])) {
      return REPLY_BUSY;
    }
  }
  for (unsigned i = 0xa10; i < 0xa90; i += 16) {
    if (BYTE(i + 4) || HALF(i + 12) || BYTE(i + 14)) {
      return REPLY_BUSY;
    }
  }
  return REPLY_OK;
}

static uint32_t request_token(const volatile uint8_t* request) {
  uint32_t value = 0;
  for (unsigned i = 5; i < 9; ++i) {
    value = (value << 8) | request[i];
  }
  return value;
}

static bool values(const volatile uint8_t* input, int8_t output[TRIM_COUNT]) {
  for (unsigned i = 0; i < TRIM_COUNT; ++i) {
    int value = input[i] < 128 ? input[i] : (int)input[i] - 256;
    if (value < -3 || value > 3) {
      return false;
    }
    output[i] = (int8_t)value;
  }
  return true;
}

static bool native_request_allowed(const volatile uint8_t* request, uint8_t length) {
  /* Only known read-only requests and releases may enter the old dispatcher
   * during ownership. In particular, no loader flag or delayed write is queued. */
  switch (request[0]) {
    case 0x00:
    case 0xa1:
      return length == 2;
    case 0xd1:
      return length == 8 && request[1] == 1 && request[2] == 0;
    case 0xd3:
      return length == 6 && request[1] == 1 && request[2] == 0;
    case 0xd5:
      return length == 11 && request[1] == 1 && (request[2] == 0 || request[2] == 4);
    case 0x17:
      return (length == 5 && request[1] == 1 && request[3] == 0) ||
             (length == 4 && request[1] >= 3 && request[1] <= 9 && request[2] == 0);
    default:
      return false;
  }
}

int trim_controller_route(void) {
#ifdef DESK_LOCAL_PANEL_LOCK
  if (local_lock_controller_route()) {
    return 1;
  }
#endif
#ifdef DESK_DISPLAY_TIMEOUT
  if (display_controller_route()) {
    return 1;
  }
#endif
  const volatile uint8_t* request = (const volatile uint8_t*)WORD(0xe8);
  if (request[0] != 0xd9) {
    return trim_controller_blocks_native() && !native_request_allowed(request, BYTE(0x124));
  }
  if (BYTE(0x124) != 18 || !trim_controller_started()) {
    return 1;
  }
  uint32_t previous = mask();
  uint8_t operation = request[2];
  if (request[1] != 1 || operation > 1) {
    restore(previous);
    return 1;
  }
  uint16_t nonce = (uint16_t)((uint16_t)request[3] << 8 | request[4]);
  TrimAdapterView view;
  trim_adapter_view(ADAPTER, &view);
  uint8_t result = REPLY_OK;
  int8_t expected[TRIM_COUNT], desired[TRIM_COUNT];
  if (!values(request + 9, expected) || !values(request + 13, desired)) {
    result = REPLY_BAD;
  } else if (!operation) {
    for (unsigned i = 5; i < 17; ++i) {
      if (request[i]) {
        result = REPLY_BAD;
      }
    }
  }
  if (result == REPLY_OK &&
      (view.busy || CONTROL->phase != CONTROL_IDLE || CONTROL->publish_pending)) {
    result = REPLY_BUSY;
  }
  if (operation && !trim_controller_ready()) {
    result = REPLY_GUARD;
  }
  if (result == REPLY_OK && operation) {
    uint32_t token = request_token(request);
    if (previous || (result = admission()) != REPLY_OK) {
      result = previous ? REPLY_GUARD : result;
    } else if (!token || token != WORD(0xfd0) || (int32_t)(WORD(0x75c) - WORD(0xfd8)) >= 0) {
      result = REPLY_STALE;
    } else {
      unsigned changes = 0;
      for (unsigned i = 0; i < TRIM_COUNT; ++i) {
        if (expected[i] != view.active[i]) {
          result = REPLY_STALE;
        }
        changes += desired[i] != expected[i];
      }
      if (result == REPLY_OK && changes > 1) {
        result = REPLY_BAD;
      }
    }
    if (result == REPLY_OK) {
      if (room() < TRIM_REPLY_RESERVE) {
        restore(previous);
        return 1;
      }
      CONTROL->reserved = 1;
      if (trim_adapter_begin_set(ADAPTER, desired)) {
        WORD(0xfd0) = 0;
        WORD(0xfd8) = 0;
        CONTROL->nonce = nonce;
        CONTROL->phase = CONTROL_SET;
        CONTROL->release_gate = 1;
        trim_original_reset_scanners();
        restore(previous);
        return 1;
      }
      CONTROL->reserved = 0;
      result = REPLY_STORAGE_ERROR;
    }
  }
  uint8_t reply[16];
  snapshot(reply, operation, nonce, result, &view);
  send_reply(reply, CONTROL->reserved ? TRIM_REPLY_RESERVE : 0);
  restore(previous);
  return 1;
}

void trim_controller_poll(void) {
  if (!trim_controller_started()) {
    return;
  }
  /* Driver polling is outside the mask; ISR captures remain possible. */
  trim_adapter_poll(ADAPTER);
  uint32_t previous = mask();
  TrimAdapterView view;
  trim_adapter_view(ADAPTER, &view);
  if (!view.busy && view.closed) {
    if (CONTROL->phase == CONTROL_BOOT) {
      CONTROL->publish_pending = 1;
      CONTROL->phase = CONTROL_IDLE;
    } else if (CONTROL->phase == CONTROL_SET) {
      CONTROL->publish_pending = 1;
      uint8_t result = view.result == TRIM_OWNER_OK &&
                               (view.storage == TRIM_VERIFIED || view.storage == TRIM_DEFAULT)
                           ? REPLY_OK
                           : REPLY_STORAGE_ERROR;
      snapshot(CONTROL->reply, 1, CONTROL->nonce, result, &view);
      CONTROL->reply_started = WORD(0x75c);
      CONTROL->phase = CONTROL_REPLY;
    }
  }
  if (CONTROL->publish_pending && trim_motion_publish(view.active)) {
    CONTROL->publish_pending = 0;
  }
  if (CONTROL->phase == CONTROL_REPLY &&
      ((uint32_t)(WORD(0x75c) - CONTROL->reply_started) >= TRIM_REPLY_WAIT ||
       send_reply(CONTROL->reply, 0))) {
    CONTROL->reserved = 0;
    CONTROL->phase = CONTROL_IDLE;
  }
  if (CONTROL->release_gate && !trim_controller_blocks_native() && !raw_key()) {
    /* Clear only in foreground, before a scanner can retain its old state.
     * CL01's persisted lock, release gate and storage-fault bytes stay unchanged. */
    trim_original_reset_scanners();
    CONTROL->release_gate = 0;
  }
  restore(previous);
}

bool trim_controller_blocks_native(void) {
  if (!trim_controller_started()) {
    return false;
  }
  TrimAdapterView view;
  trim_adapter_view(ADAPTER, &view);
  return CONTROL->phase != CONTROL_IDLE || CONTROL->publish_pending || view.busy || !view.closed ||
         !view.handoff;
}

bool trim_controller_inputs_blocked(void) {
  if (!trim_controller_started()) {
    return false;
  }
  uint32_t previous = mask();
  bool blocked = trim_controller_blocks_native();
  if (blocked) {
    CONTROL->release_gate = 1;
  }
  blocked = blocked || CONTROL->release_gate;
  restore(previous);
  return blocked;
}

uint8_t trim_controller_capture_native(void) {
  return trim_controller_started() ? trim_native_capture(ADAPTER) : TRIM_CAPTURE_PASSTHROUGH;
}
