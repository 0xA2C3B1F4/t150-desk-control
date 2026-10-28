#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <vector>
#include "firmware_profile.h"
#include "esphome/core/log.h"

namespace firmware_profile_test {
namespace fp = desk_protocol::firmware_profile;
using esphome::desk_firmware_export::identify_firmware_profile;
using Scalars = desk_protocol::HeightProfile::Scalars;

inline void require(bool ok, const char *message) {
  if (!ok) {
    ESP_LOGE("profile_test", "%s", message);
    std::exit(1);
  }
}

inline void word(std::vector<uint8_t> &image, size_t offset, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) image[offset + i] = static_cast<uint8_t>(value >> (8 * i));
}

// Explicit layout oracle; do not obtain these offsets from the implementation.
inline void set_scalars(std::vector<uint8_t> &image, const Scalars &value) {
  word(image, 0x17B4, value.display_offset);
  word(image, 0x17B8, static_cast<uint32_t>(value.high_test_addend));
  word(image, 0x17BC, value.high_display);
  word(image, 0x5FD4, value.upper_seed);
  word(image, 0x5FD8, value.lower_seed);
}

inline void no_control(const fp::Result &result) {
  const auto profile = result.research_profile(24, 39);
  require(!profile.control_approved(), "Image recognition granted control approval");
  uint16_t target = 0;
  require(!profile.target_raw(730, target), "Extracted profile generated a control target");
  if (result.extracted()) {
    const auto assessed = profile.assess();
    require(assessed.error == desk_protocol::HeightProfile::Error::BootScaleUnknown,
            "Image recognition inferred boot coefficients");
    require(!assessed.mapping_usable && !assessed.control_approved, "Missing boot evidence approved mapping");
  }
}

inline void check(const std::vector<uint8_t> &image, fp::Status status, const Scalars *expected = nullptr) {
  const auto result = identify_firmware_profile(image.data(), image.size());
  require(result.status == status, "Unexpected image recognition status");
  no_control(result);
  if (expected) {
    const auto &actual = result.scalars;
    require(result.extracted() && actual.lower_seed == expected->lower_seed &&
            actual.upper_seed == expected->upper_seed && actual.display_offset == expected->display_offset &&
            actual.high_test_addend == expected->high_test_addend && actual.high_display == expected->high_display,
            "Extracted fields differ from independently specified values");
  } else {
    require(!result.extracted(), "Rejected image exposed a usable profile");
  }
}

inline void synthetic() {
  std::vector<uint8_t> image(fp::APPLICATION_SIZE);
  for (size_t i = 0; i < image.size(); ++i) image[i] = static_cast<uint8_t>(i * 37 + 11);
  // The digest is checked independently with Python hashlib after the host exits.
  esphome::sha256::SHA256 digest;
  digest.init();
  for (size_t i = 0; i < fp::NORMALIZED_SPAN_COUNT; ++i) {
    fp::Span span{};
    require(fp::normalized_span(image.data(), image.size(), i, span), "Cannot normalize supported image size");
    if (span.size) digest.add(span.data, span.size);
  }
  digest.calculate();
  char hex[65]{};
  digest.get_hex(hex);
  ESP_LOGI("profile_test", "PROFILE_NORMALIZED_SHA=%s", hex);
  check(image, fp::Status::Unrecognized);
  std::fill(image.begin(), image.end(), 0);
  check(image, fp::Status::Unrecognized);
  set_scalars(image, Scalars(4, 460, 725, -1182, 1188));
  check(image, fp::Status::Unrecognized);
  image.resize(fp::APPLICATION_SIZE - 1);
  check(image, fp::Status::UnsupportedSize);
  image.resize(fp::APPLICATION_SIZE + 1);
  check(image, fp::Status::UnsupportedSize);
  ESP_LOGI("profile_test", "PROFILE_SYNTHETIC=passed");
}

inline void private_fixture(const char *path) {
  std::ifstream stream(path, std::ios::binary);
  require(stream.good(), "Cannot open optional local firmware fixture");
  std::vector<uint8_t> original(fp::APPLICATION_SIZE);
  stream.read(reinterpret_cast<char *>(original.data()), original.size());
  require(stream.gcount() == static_cast<std::streamsize>(original.size()) &&
              stream.peek() == std::char_traits<char>::eof(),
          "Optional fixture size differs from known application");
  const Scalars known(4, 460, 725, -1182, 1188);
  check(original, fp::Status::Known, &known);
  const Scalars variants[] = {
      Scalars(5, 460, 725, -1182, 1188),
      Scalars(4, 620, 725, -1182, 1188),
      Scalars(4, 460, 625, -1182, 1188),
      Scalars(4, 460, 725, -1242, 1188),
      Scalars(4, 460, 725, -1182, 1248),
      Scalars(5, 620, 625, -1242, 1248),
  };
  for (const auto &value : variants) {
    auto image = original;
    set_scalars(image, value);
    check(image, fp::Status::ScalarVariant, &value);
  }
  const Scalars invalid[] = {
      Scalars(460, 460, 725, -1182, 1188),
      Scalars(4, 10001, 725, -1182, 1188),
      Scalars(4, 460, 5001, -1182, 1188),
      Scalars(4, 460, 725, 0, 1188),
      Scalars(4, 460, 725, INT32_MIN, 1188),
      Scalars(4, 460, 725, -1182, 0),
      Scalars(4, 460, 725, -1182, 10001),
  };
  for (const auto &value : invalid) {
    auto image = original;
    set_scalars(image, value);
    check(image, fp::Status::InvalidScalars);
  }
  // Adjacent non-scalar bytes catch an accidentally widened normalization span.
  const size_t offsets[] = {0, 0x17B3, 0x17C0, 0x5FD3, 0x5FDC, fp::APPLICATION_SIZE - 1};
  for (size_t offset : offsets) {
    auto image = original;
    image[offset] ^= 1;
    check(image, fp::Status::Unrecognized);
  }
  auto shifted = original;
  std::rotate(shifted.begin(), shifted.begin() + 2, shifted.end());
  check(shifted, fp::Status::Unrecognized);
  auto truncated = original;
  truncated.pop_back();
  check(truncated, fp::Status::UnsupportedSize);
  auto extended = original;
  extended.push_back(0);
  check(extended, fp::Status::UnsupportedSize);
  ESP_LOGI("profile_test", "PROFILE_PRIVATE=passed");
}

inline void run() {
  synthetic();
  const char *fixture = std::getenv("T150_PROFILE_TEST_FIRMWARE");
  if (fixture && *fixture) private_fixture(fixture);
}
}  // namespace firmware_profile_test
