#include "motion_trim_core.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static int64_t signed_value(uint32_t bits) {
  return bits <= INT32_MAX ? (int64_t)bits : (int64_t)bits - INT64_C(4294967296);
}

static uint32_t native_target(uint32_t requested, uint32_t current) {
  int64_t p = signed_value(requested);
  if (p > signed_value(current + 4u))
    return requested + 7u;
  if (p < signed_value(current - 4u))
    return requested - 6u;
  return requested;
}

/* Contract oracle, not proof of equivalence to the original firmware hooks. */
static uint32_t reference_target(uint32_t requested, uint32_t current, const int8_t values[4],
                                 uint8_t speed) {
  int64_t base = native_target(requested, current);
  int64_t request_signed = signed_value(requested);
  if (base < 6 || base > 747)
    return (uint32_t)base;
  if (request_signed > signed_value(current + 4u))
    base += values[2 * speed];
  else if (request_signed < signed_value(current - 4u))
    base -= values[2 * speed + 1];
  return (uint32_t)(base < 6 ? 6 : base > 747 ? 747 : base);
}

static void assert_zero(const int8_t values[TRIM_COUNT]) {
  for (unsigned i = 0; i < TRIM_COUNT; ++i)
    assert(values[i] == 0);
}

static void all_tuples(void) {
  unsigned tuples = 0;
  for (int a = -3; a <= 3; ++a)
    for (int b = -3; b <= 3; ++b)
      for (int c = -3; c <= 3; ++c)
        for (int d = -3; d <= 3; ++d) {
          int8_t values[4] = {(int8_t)a, (int8_t)b, (int8_t)c, (int8_t)d}, decoded[4];
          uint8_t record[TRIM_RECORD_SIZE], damaged[TRIM_RECORD_SIZE];
          assert(trim_record_encode(values, record));
          assert(trim_record_decode(record, record, decoded) == TRIM_VERIFIED);
          assert(memcmp(values, decoded, sizeof values) == 0);
          for (unsigned speed = 0; speed < 2; ++speed) {
            assert(trim_target(300, 100, values, speed, true) ==
                   (uint32_t)(307 + values[2 * speed]));
            assert(trim_target(100, 300, values, speed, true) ==
                   (uint32_t)(94 - values[2 * speed + 1]));
            for (uint32_t p = 196; p <= 204; ++p)
              assert(trim_target(p, 200, values, speed, true) == p);
          }
          assert(trim_target(300, 100, values, 2, true) == 307);
          assert(trim_target(300, 100, values, 0, false) == 307);
          assert(trim_target(100, 300, values, 1, false) == 94);
          for (unsigned bit = 0; bit < sizeof record * CHAR_BIT; ++bit) {
            memcpy(damaged, record, sizeof record);
            damaged[bit / 8] ^= (uint8_t)(1u << (bit % 8));
            assert(trim_record_decode(damaged, damaged, decoded) == TRIM_DEFAULT);
            for (unsigned i = 0; i < TRIM_COUNT; ++i) {
              assert(decoded[i] == (i == bit / 8 ? 0 : values[i]));
            }
            assert(trim_record_decode(record, damaged, decoded) == TRIM_FAULT);
            assert_zero(decoded);
          }
          ++tuples;
        }
  assert(tuples == 2401);
}

static void record_failures(void) {
  static const uint8_t codes[] = {0x1e, 0x2d, 0x33, 0x4b, 0x55, 0x66, 0x78};
  const int8_t original[TRIM_COUNT] = {-2, 1, 3, -1};
  uint8_t record[TRIM_RECORD_SIZE];
  int8_t decoded[TRIM_COUNT];
  for (unsigned slot = 0; slot < TRIM_COUNT; ++slot) {
    for (unsigned byte = 0; byte <= 255; ++byte) {
      assert(trim_record_encode(original, record));
      record[slot] = (uint8_t)byte;
      unsigned code = 0;
      while (code < sizeof codes && codes[code] != byte) {
        ++code;
      }
      bool valid = code < sizeof codes;
      assert(trim_record_decode(record, record, decoded) == (valid ? TRIM_VERIFIED : TRIM_DEFAULT));
      for (unsigned i = 0; i < TRIM_COUNT; ++i) {
        int8_t expected = i == slot ? (valid ? (int8_t)((int)code - 3) : 0) : original[i];
        assert(decoded[i] == expected);
      }
    }
  }
  for (unsigned code = 0; code < sizeof codes; ++code) {
    for (unsigned mask = 1; mask <= 255; ++mask) {
      unsigned bits = 0;
      for (unsigned bit = 0; bit < 8; ++bit) {
        bits += (mask >> bit) & 1u;
      }
      if (bits <= 3) {
        assert(trim_record_encode(original, record));
        record[0] = codes[code] ^ mask;
        assert(trim_record_decode(record, record, decoded) == TRIM_DEFAULT);
        assert(decoded[0] == 0);
        assert(memcmp(decoded + 1, original + 1, TRIM_COUNT - 1) == 0);
      }
    }
  }
  for (unsigned index = 0; index < 4; ++index) {
    for (int value = -128; value <= 127; ++value) {
      int8_t tuple[4] = {2, 2, 2, 2};
      tuple[index] = (int8_t)value;
      memset(record, 0xcc, sizeof record);
      bool valid = value >= -3 && value <= 3;
      assert(trim_record_encode(tuple, record) == valid);
      if (!valid) {
        for (unsigned i = 0; i < sizeof record; ++i)
          assert(record[i] == 0xcc);
        assert(trim_target(300, 100, tuple, 0, true) == 307);
        assert(trim_target(300, 100, tuple, 1, true) == 307);
        assert(trim_target(100, 300, tuple, 0, true) == 94);
        assert(trim_target(100, 300, tuple, 1, true) == 94);
      }
    }
  }
}

static void target_boundaries(void) {
  int8_t positive[4] = {3, 3, 3, 3}, negative[4] = {-3, -3, -3, -3}, zero[4] = {0};
  assert(trim_target(740, 100, positive, 0, true) == 747);
  assert(trim_target(12, 100, positive, 1, true) == 6);
  assert(trim_target(740, 100, negative, 1, true) == 744);
  assert(trim_target(12, 100, negative, 0, true) == 9);
  assert(trim_target(UINT32_MAX, UINT32_MAX - 16, negative, 0, true) == 6);
  assert(trim_target(753, 800, negative, 1, true) == 747);
  /* Out-of-profile bases retain the native untrimmed value, without clamping. */
  assert(trim_target(741, 100, positive, 0, true) == 748);
  assert(trim_target(11, 100, positive, 0, true) == 5);
  uint32_t edges[] = {0,          1,          3,          4,          5,          6,
                      7,          743,        747,        748,        0x7ffffffb, 0x7ffffffc,
                      0x7fffffff, 0x80000000, 0x80000004, 0xfffffffb, 0xffffffff};
  for (unsigned i = 0; i < sizeof edges / sizeof edges[0]; ++i) {
    for (unsigned j = 0; j < sizeof edges / sizeof edges[0]; ++j) {
      uint32_t expected = native_target(edges[i], edges[j]);
      assert(trim_target(edges[i], edges[j], zero, 0, true) == expected);
      assert(trim_target(edges[i], edges[j], positive, 0, false) == expected);
      int8_t mixed[4] = {3, -2, -1, 2};
      const int8_t* tuples[] = {positive, negative, mixed};
      for (unsigned tuple = 0; tuple < 3; ++tuple)
        for (uint8_t speed = 0; speed < 2; ++speed)
          assert(trim_target(edges[i], edges[j], tuples[tuple], speed, true) ==
                 reference_target(edges[i], edges[j], tuples[tuple], speed));
    }
  }
  uint32_t random = 0x12be90af;
  for (unsigned i = 0; i < 100000; ++i) {
    random = random * 1664525u + 1013904223u;
    uint32_t p = random;
    random = random * 1664525u + 1013904223u;
    assert(trim_target(p, random, zero, i % 2, true) == native_target(p, random));
    int8_t values[4];
    uint32_t bits = p;
    for (unsigned index = 0; index < 4; ++index) {
      values[index] = (int8_t)((int)(bits % 7) - 3);
      bits >>= 3;
    }
    assert(trim_target(p, random, values, i % 2, true) ==
           reference_target(p, random, values, i % 2));
  }
}

int main(void) {
  all_tuples();
  record_failures();
  target_boundaries();
  puts("Motion trim core: 2401 tuples, corruption checks and target boundaries passed");
  return 0;
}
