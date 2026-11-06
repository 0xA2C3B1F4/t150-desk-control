#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace esphome {
namespace desk_provisioning {
// Wire layout: magic[4], version, secret length, reserved[2], API key[32],
// web secret[16], CRC32 little-endian[4]. CRC detects corruption, not tampering.
using Record = std::array<uint8_t, 60>;
enum class ReadResult { FOUND, MISSING, ERROR };
enum class InitResult { LOADED, CREATED, FAILED };

inline uint32_t checksum(const uint8_t* data, size_t size) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (unsigned bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  }
  return ~crc;
}
inline bool usable_secret(const uint8_t* data, size_t size) {
  bool zero = true, ones = true;
  for (size_t i = 0; i < size; ++i) {
    zero &= data[i] == 0;
    ones &= data[i] == 255;
  }
  return !zero && !ones;
}
inline void seal(Record& record) {
  record[0] = 'T';
  record[1] = '1';
  record[2] = '5';
  record[3] = 'P';
  record[4] = 1;
  record[5] = 48;
  record[6] = 0;
  record[7] = 0;
  uint32_t crc = checksum(record.data(), 56);
  for (unsigned i = 0; i < 4; ++i) {
    record[56 + i] = static_cast<uint8_t>(crc >> (8 * i));
  }
}
inline bool valid(const Record& record) {
  if (record[0] != 'T' || record[1] != '1' || record[2] != '5' || record[3] != 'P' ||
      record[4] != 1 || record[5] != 48 || record[6] || record[7]) {
    return false;
  }
  uint32_t crc = 0;
  for (unsigned i = 0; i < 4; ++i) {
    crc |= uint32_t(record[56 + i]) << (8 * i);
  }
  return crc == checksum(record.data(), 56) && usable_secret(record.data() + 8, 32) &&
         usable_secret(record.data() + 40, 16);
}

// Storage owns open/commit/close. A successful save must be followed by a fresh
// read.
class Backend {
 public:
  virtual ~Backend() = default;
  virtual ReadResult read(Record& record) = 0;
  virtual bool save(const Record& record) = 0;
  virtual bool random(uint8_t* data, size_t size) = 0;
};
inline InitResult initialize(Backend& backend, Record& record) {
  record.fill(0);
  const auto found = backend.read(record);
  if (found == ReadResult::FOUND) {
    return valid(record) ? InitResult::LOADED : InitResult::FAILED;
  }
  if (found != ReadResult::MISSING) {
    return InitResult::FAILED;
  }
  Record candidate{};
  if (!backend.random(candidate.data() + 8, 48)) {
    return InitResult::FAILED;
  }
  seal(candidate);
  if (!valid(candidate) || !backend.save(candidate)) {
    return InitResult::FAILED;
  }
  Record check{};
  if (backend.read(check) != ReadResult::FOUND || check != candidate || !valid(check)) {
    return InitResult::FAILED;
  }
  record = check;
  return InitResult::CREATED;
}

// Unsigned subtraction handles millis() wrap. A held boot button must first be
// released.
class HoldButton {
 public:
  bool update(bool pressed, uint32_t now) {
    if (!initialized_) {
      initialized_ = true;
      raw_ = pressed;
      changed_ = now;
      return false;
    }
    if (pressed != raw_) {
      raw_ = pressed;
      changed_ = now;
    }
    if (uint32_t(now - changed_) < 50) {
      return false;
    }
    if (!pressed) {
      armed_ = true;
      holding_ = false;
      return false;
    }
    if (!armed_) {
      return false;
    }
    if (!holding_) {
      holding_ = true;
      started_ = now;
    }
    if (uint32_t(now - started_) < 2000) {
      return false;
    }
    armed_ = false;
    holding_ = false;
    return true;
  }

 private:
  bool initialized_{false}, raw_{false}, armed_{false}, holding_{false};
  uint32_t changed_{0}, started_{0};
};
}  // namespace desk_provisioning
}  // namespace esphome
