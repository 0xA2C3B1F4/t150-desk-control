#pragma once
#include "height_profile.h"
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace desk_protocol {
namespace firmware_profile {
static constexpr std::size_t APPLICATION_SIZE = 55296;
static constexpr std::size_t NORMALIZED_SPAN_COUNT = 11;

enum class Status { NoImage, UnsupportedSize, Unrecognized, InvalidScalars, Known, ScalarVariant };
struct Result {
  Status status{Status::NoImage};
  HeightProfile::Scalars scalars{0, 0, 0, 0, 0};
  bool extracted() const { return status == Status::Known || status == Status::ScalarVariant; }
  const char *status_text() const {
    switch (status) {
      case Status::NoImage: return "No firmware image";
      case Status::UnsupportedSize: return "Unsupported image size";
      case Status::Unrecognized: return "Unrecognized firmware";
      case Status::InvalidScalars: return "Invalid profile values";
      case Status::Known: return "Known T150 profile";
      case Status::ScalarVariant: return "Recognized scalar variant";
    }
    return "Unrecognized firmware";
  }
  // Recognition never selects the component's approved operational profile.
  HeightProfile research_profile(uint16_t a, uint16_t b) const {
    return HeightProfile::research(scalars, a, b, extracted());
  }
};
struct Span { const uint8_t *data; std::size_t size; };

namespace detail {
// Offsets from application address 0x08002800. Only the pinned normalized
// digest establishes this layout; matching scalar values alone proves nothing.
static const std::size_t SCALAR_OFFSETS[5] = {0x17B4, 0x17B8, 0x17BC, 0x5FD4, 0x5FD8};
static const uint8_t FAMILY_SHA256[32] = {
  0x3f,0x0b,0x62,0x22,0x73,0xa2,0xc2,0xc8,0x70,0x60,0x8b,0xf8,0x57,0xed,0x52,0xb3,
  0xa4,0x7d,0x40,0x3e,0x97,0xb4,0x4a,0x15,0xd0,0xd2,0x1b,0xc9,0x2a,0xb7,0x4b,0x6d
};
inline uint32_t read_u32(const uint8_t *data, std::size_t offset) {
  return static_cast<uint32_t>(data[offset]) |
      (static_cast<uint32_t>(data[offset + 1]) << 8) |
      (static_cast<uint32_t>(data[offset + 2]) << 16) |
      (static_cast<uint32_t>(data[offset + 3]) << 24);
}
inline int32_t read_i32(const uint8_t *data, std::size_t offset) {
  const uint32_t word = read_u32(data, offset);
  // Avoid implementation-defined uint32-to-int32 conversion above INT32_MAX.
  const int64_t value = word <= 0x7FFFFFFFu ? static_cast<int64_t>(word) :
      static_cast<int64_t>(word) - 4294967296LL;
  return static_cast<int32_t>(value);
}
}  // namespace detail

// Hash these spans, in index order, as one SHA-256 stream. Original intervals
// alternate with four-byte zero spans; adjacent scalar words produce empty
// original intervals. Source bytes are never copied or changed. The image must
// remain unchanged through hashing and identify(); full-flash input is not accepted.
inline bool normalized_span(const uint8_t *data, std::size_t size,
                            std::size_t index, Span &out) {
  out = {nullptr, 0};
  if (!data || size != APPLICATION_SIZE || index >= NORMALIZED_SPAN_COUNT) return false;
  static const uint8_t zeros[4] = {};
  if (index % 2) { out = {zeros, sizeof(zeros)}; return true; }
  const std::size_t slot = index / 2;
  const std::size_t begin = slot ? detail::SCALAR_OFFSETS[slot - 1] + 4 : 0;
  const std::size_t end = slot < 5 ? detail::SCALAR_OFFSETS[slot] : size;
  out = {data + begin, end - begin};
  return true;
}

// The caller supplies SHA-256 of exactly normalized_span()'s stream for this
// immutable image. Hashing stays outside the portable parser and can use the
// platform implementation. Validate size and digest before reading any fields.
inline Result identify(const uint8_t *data, std::size_t size,
                       const uint8_t normalized_sha256[32]) {
  Result result;
  if (!data) return result;
  if (size != APPLICATION_SIZE) { result.status = Status::UnsupportedSize; return result; }
  result.status = Status::Unrecognized;
  if (!normalized_sha256 || std::memcmp(normalized_sha256, detail::FAMILY_SHA256, 32)) return result;
  result.scalars = HeightProfile::Scalars(detail::read_u32(data, 0x5FD8),
      detail::read_u32(data, 0x5FD4), detail::read_u32(data, 0x17B4),
      detail::read_i32(data, 0x17B8), detail::read_u32(data, 0x17BC));
  // Reuse the scalar plausibility checks without inventing current/boot A/B.
  if (!HeightProfile::research(result.scalars, 1, 1, true).display(0).valid) {
    result.status = Status::InvalidScalars;
    return result;
  }
  const auto &s = result.scalars;
  result.status = s.lower_seed == 4 && s.upper_seed == 460 && s.display_offset == 725 &&
      s.high_test_addend == -1182 && s.high_display == 1188 ? Status::Known : Status::ScalarVariant;
  return result;
}
}  // namespace firmware_profile
}  // namespace desk_protocol
