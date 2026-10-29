#pragma once
#include "height_profile.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace desk_protocol {
namespace journal_detail {
inline uint16_t read16(const uint8_t *data) {
  return static_cast<uint16_t>(data[0] | static_cast<uint16_t>(data[1]) << 8);
}
inline uint32_t read32(const uint8_t *data) {
  return static_cast<uint32_t>(data[0]) | static_cast<uint32_t>(data[1]) << 8 |
      static_cast<uint32_t>(data[2]) << 16 | static_cast<uint32_t>(data[3]) << 24;
}
inline void write16(uint8_t *data, uint16_t value) {
  data[0] = static_cast<uint8_t>(value);
  data[1] = static_cast<uint8_t>(value >> 8);
}
inline void write32(uint8_t *data, uint32_t value) {
  for (uint8_t shift = 0; shift < 32; shift += 8)
    *data++ = static_cast<uint8_t>(value >> shift);
}
// CRC-32/ISO-HDLC: reflected polynomial, initial/final XOR 0xffffffff.
inline uint32_t crc32(const uint8_t *data, std::size_t size) {
  uint32_t crc = 0xFFFFFFFFU;
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ ((crc & 1U) ? 0xEDB88320U : 0U);
  }
  return crc ^ 0xFFFFFFFFU;
}
}  // namespace journal_detail

// Serialized bytes have a fixed layout across compiler padding and endianness.
// Decoding validates the envelope, not whether a profile may control the desk.
struct PresetJournal {
  enum class State : uint8_t { Resolved = 0, Retained = 1 };
  using Encoded = std::array<uint8_t, 64>;
  State state{State::Resolved};
  HeightProfileIdentity profile{};
  uint16_t original{0}, temporary{0};

  Encoded encode() const {
    Encoded data{};
    journal_detail::write32(data.data(), 0x44534B34U);
    journal_detail::write16(data.data() + 4, static_cast<uint16_t>(data.size()));
    data[6] = static_cast<uint8_t>(state);
    for (std::size_t i = 0; i < profile.bytes.size(); ++i) data[8 + i] = profile.bytes[i];
    journal_detail::write16(data.data() + 56, original);
    journal_detail::write16(data.data() + 58, temporary);
    journal_detail::write32(data.data() + 60, journal_detail::crc32(data.data(), 60));
    return data;
  }
  static bool decode(const uint8_t *data, std::size_t size, PresetJournal &out) {
    if (!data || size != 64 || journal_detail::read32(data) != 0x44534B34U ||
        journal_detail::read16(data + 4) != 64 || data[6] > 1 || data[7] != 0 ||
        journal_detail::read32(data + 60) != journal_detail::crc32(data, 60)) return false;
    PresetJournal parsed;
    parsed.state = static_cast<State>(data[6]);
    for (std::size_t i = 0; i < parsed.profile.bytes.size(); ++i) parsed.profile.bytes[i] = data[8 + i];
    parsed.original = journal_detail::read16(data + 56);
    parsed.temporary = journal_detail::read16(data + 58);
    out = parsed;
    return true;
  }
  static bool decode(const Encoded &data, PresetJournal &out) {
    return decode(data.data(), data.size(), out);
  }
};

// Versions 1-3 used the same eight little-endian bytes and carried no identity.
// An all-zero legacy record means cleared storage; it is not an active backup.
struct LegacyPresetJournal {
  // Versions 1-3 implicitly used these fixed semantics. Do not derive this
  // identity from a future default profile when migrating historical records.
  static HeightProfileIdentity profile_identity() {
    HeightProfileIdentity result;
    result.bytes = {{
      0x01,0x00,0x01,0x00,0x04,0x00,0x00,0x00,0xCC,0x01,0x00,0x00,
      0xD5,0x02,0x00,0x00,0x62,0xFB,0xFF,0xFF,0xA4,0x04,0x00,0x00,
      0x18,0x00,0x27,0x00,0xDA,0x02,0x9C,0x04,0x08,0x00,0xE4,0x02,
      0x06,0x00,0xE4,0x02,0x08,0x00,0x07,0x00,0xFA,0xFF,0x01,0x00
    }};
    return result;
  }
  uint32_t magic{0};
  uint16_t original{0}, temporary{0};
  static bool decode(const uint8_t *data, std::size_t size, LegacyPresetJournal &out) {
    if (!data || size != 8) return false;
    const uint32_t magic = journal_detail::read32(data);
    if (magic != 0x44534B31U && magic != 0x44534B32U && magic != 0x44534B33U) return false;
    LegacyPresetJournal parsed;
    parsed.magic = magic;
    parsed.original = journal_detail::read16(data + 4);
    parsed.temporary = journal_detail::read16(data + 6);
    out = parsed;
    return true;
  }
};
}  // namespace desk_protocol
