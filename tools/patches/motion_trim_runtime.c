#include <stddef.h>
#include <stdint.h>

/* Only the compiler helpers used by the trim build are provided here. These
 * loops must compile without a runtime-library dependency of their own. */
void __aeabi_memcpy(void* destination, const void* source, size_t size) {
  uint8_t* output = destination;
  const uint8_t* input = source;
  for (size_t i = 0; i < size; ++i) {
    output[i] = input[i];
  }
}

void __aeabi_memcpy4(void* destination, const void* source, size_t size) {
  /* Alignment does not imply that size is a multiple of four. */
  __aeabi_memcpy(destination, source, size);
}

void __aeabi_memclr4(void* destination, size_t size) {
  uint8_t* output = destination;
  for (size_t i = 0; i < size; ++i) {
    output[i] = 0;
  }
}

void __aeabi_memclr8(void* destination, size_t size) {
  /* The compiler can infer eight-byte alignment for fixed platform slots. */
  __aeabi_memclr4(destination, size);
}

uint64_t __aeabi_lmul(uint64_t left, uint64_t right) {
  /* Unsigned addition gives the required low 64 bits for either signedness.
   * Fixed one-bit shifts avoid recursively requesting another ABI helper. */
  uint64_t result = 0;
  while (right) {
    if (right & 1u) {
      result += left;
    }
    left <<= 1;
    right >>= 1;
  }
  return result;
}
