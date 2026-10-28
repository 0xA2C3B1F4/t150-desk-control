#pragma once
#include "desk_protocol/firmware_profile.h"
#include "esphome/components/sha256/sha256.h"

namespace esphome::desk_firmware_export {

// Keep the SHA object and every hash operation in this stack frame. ESP-IDF
// hardware SHA requires this on affected ESP32 variants. The image is streamed
// with five zero-word substitutions; no second image-sized allocation is used.
inline desk_protocol::firmware_profile::Result identify_firmware_profile(const uint8_t *image, size_t size) {
  namespace profile = desk_protocol::firmware_profile;
  uint8_t normalized_hash[32]{};
  if (image != nullptr && size == profile::APPLICATION_SIZE) {
    sha256::SHA256 digest;
    digest.init();
    for (size_t index = 0; index < profile::NORMALIZED_SPAN_COUNT; ++index) {
      profile::Span span;
      if (!profile::normalized_span(image, size, index, span)) {
        profile::Result result;
        result.status = profile::Status::Unrecognized;
        return result;
      }
      if (span.size) digest.add(span.data, span.size);
    }
    digest.calculate();
    digest.get_bytes(normalized_hash);
  }
  return profile::identify(image, size, normalized_hash);
}

}  // namespace esphome::desk_firmware_export
