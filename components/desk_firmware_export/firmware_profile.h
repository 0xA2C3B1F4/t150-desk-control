#pragma once
#include "desk_protocol/firmware_profile.h"
#include "esphome/components/sha256/sha256.h"

namespace esphome::desk_firmware_export {

// Keep the SHA object and every hash operation in this stack frame. ESP-IDF
// hardware SHA requires this on affected ESP32 variants. The image is streamed
// with five zero-word substitutions; no second image-sized allocation is used.
inline bool normalized_firmware_sha256(const uint8_t *image, size_t size, uint8_t output[32]) {
  namespace profile = desk_protocol::firmware_profile;
  if (output == nullptr) return false;
  std::memset(output, 0, 32);
  if (image == nullptr || size != profile::APPLICATION_SIZE) return false;
  sha256::SHA256 digest;
  digest.init();
  for (size_t index = 0; index < profile::NORMALIZED_SPAN_COUNT; ++index) {
    profile::Span span;
    if (!profile::normalized_span(image, size, index, span)) return false;
    if (span.size) digest.add(span.data, span.size);
  }
  digest.calculate();
  digest.get_bytes(output);
  return true;
}

inline desk_protocol::firmware_profile::Result identify_firmware_profile(const uint8_t *image, size_t size) {
  uint8_t normalized_hash[32]{};
  const bool hashed = normalized_firmware_sha256(image, size, normalized_hash);
  return desk_protocol::firmware_profile::identify(image, size, hashed ? normalized_hash : nullptr);
}

}  // namespace esphome::desk_firmware_export
