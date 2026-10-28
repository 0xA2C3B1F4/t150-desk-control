#include "desk_protocol/firmware_profile.h"
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>

using namespace desk_protocol;
namespace fp = desk_protocol::firmware_profile;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while (false)

// Synthetic bytes only. Supplying this expected digest tests the parser's hash
// boundary and extraction, not SHA-256 or recognition of an actual application.
// Real hashing and the private firmware cross-check belong to integration tests.
static const uint8_t EXPECTED_DIGEST[32] = {
  0x3f,0x0b,0x62,0x22,0x73,0xa2,0xc2,0xc8,0x70,0x60,0x8b,0xf8,0x57,0xed,0x52,0xb3,
  0xa4,0x7d,0x40,0x3e,0x97,0xb4,0x4a,0x15,0xd0,0xd2,0x1b,0xc9,0x2a,0xb7,0x4b,0x6d
};
static const std::size_t OFFSETS[5] = {0x17B4, 0x17B8, 0x17BC, 0x5FD4, 0x5FD8};
static void word(std::vector<uint8_t> &image, std::size_t offset, uint32_t value) {
  for (unsigned byte = 0; byte < 4; ++byte)
    image[offset + byte] = static_cast<uint8_t>(value >> (byte * 8));
}
static std::vector<uint8_t> synthetic_image() {
  std::vector<uint8_t> image(fp::APPLICATION_SIZE);
  for (std::size_t i = 0; i < image.size(); ++i) image[i] = static_cast<uint8_t>(i * 37 + 11);
  word(image, 0x17B4, 725); word(image, 0x17B8, static_cast<uint32_t>(-1182));
  word(image, 0x17BC, 1188); word(image, 0x5FD4, 460); word(image, 0x5FD8, 4);
  return image;
}
static std::vector<uint8_t> normalized(const std::vector<uint8_t> &image) {
  std::vector<uint8_t> result;
  fp::Span span{nullptr, 0};
  for (std::size_t i = 0; i < fp::NORMALIZED_SPAN_COUNT; ++i) {
    CHECK(fp::normalized_span(image.data(), image.size(), i, span));
    CHECK(span.data != nullptr);
    result.insert(result.end(), span.data, span.data + span.size);
  }
  CHECK(result.size() == fp::APPLICATION_SIZE);
  return result;
}
static void blocked_control(const fp::Result &result) {
  const auto profile = result.research_profile(24, 39);
  CHECK(profile.extraction_supported() == result.extracted());
  CHECK(!profile.control_approved());
  CHECK(!profile.assess(24, 39).control_approved);
  uint16_t value = 0;
  CHECK(!profile.target_raw(730, value));
  CHECK(!profile.preset_height(8, value));
  CHECK(!profile.keep_awake_raw(8));
}
static fp::Result parse(const std::vector<uint8_t> &image) {
  const auto result = fp::identify(image.data(), image.size(), EXPECTED_DIGEST);
  blocked_control(result);
  return result;
}
static void normalization() {
  const auto image = synthetic_image();
  const auto untouched = image;
  const auto actual = normalized(image);
  std::vector<uint8_t> expected = image;
  for (const auto offset : OFFSETS)
    for (unsigned byte = 0; byte < 4; ++byte) expected[offset + byte] = 0;
  CHECK(actual == expected); CHECK(image == untouched);
  fp::Span span{image.data(), 1};
  CHECK(!fp::normalized_span(nullptr, fp::APPLICATION_SIZE, 0, span));
  CHECK(span.data == nullptr && span.size == 0);
  CHECK(!fp::normalized_span(image.data(), image.size(), fp::NORMALIZED_SPAN_COUNT, span));
  CHECK(!fp::normalized_span(image.data(), image.size(), std::numeric_limits<std::size_t>::max(), span));
  for (std::size_t index : {std::size_t(2), std::size_t(4), std::size_t(8)}) {
    CHECK(fp::normalized_span(image.data(), image.size(), index, span)); CHECK(span.size == 0);
  }
  // Every scalar byte is excluded; surrounding and unrelated bytes remain.
  for (auto offset : OFFSETS) {
    for (unsigned byte = 0; byte < 4; ++byte) {
      auto changed = image; changed[offset + byte] ^= 0xFF;
      CHECK(normalized(changed) == actual);
    }
  }
  for (auto offset : {std::size_t(0), std::size_t(0x17B3), std::size_t(0x17C0),
                     std::size_t(0x5FD3), std::size_t(0x5FDC), fp::APPLICATION_SIZE - 1}) {
    auto changed = image; changed[offset] ^= 1;
    auto changed_normalized = normalized(changed);
    CHECK(changed_normalized != actual); changed_normalized[offset] ^= 1;
    CHECK(changed_normalized == actual);
  }
}
static void recognition_and_bounds() {
  const auto image = synthetic_image();
  const auto known = parse(image);
  CHECK(known.status == fp::Status::Known && known.extracted());
  CHECK(known.scalars.lower_seed == 4 && known.scalars.upper_seed == 460);
  CHECK(known.scalars.display_offset == 725 && known.scalars.high_test_addend == -1182);
  CHECK(known.scalars.high_display == 1188);
  CHECK(known.research_profile(24, 39).assess(24, 39).mapping_usable);
  CHECK(!known.research_profile(24, 39).assess().mapping_usable);
  CHECK(!known.research_profile(25, 39).assess(24, 39).mapping_usable);
  CHECK(std::strcmp(known.status_text(), "Known T150 profile") == 0);
  CHECK(fp::identify(nullptr, 0, nullptr).status == fp::Status::NoImage);
  CHECK(fp::identify(nullptr, fp::APPLICATION_SIZE, EXPECTED_DIGEST).status == fp::Status::NoImage);
  CHECK(fp::identify(image.data(), image.size(), nullptr).status == fp::Status::Unrecognized);
  for (auto size : {std::size_t(0), std::size_t(1), image.size() - 1, image.size() + 1,
                    std::size_t(65536), std::numeric_limits<std::size_t>::max()}) {
    const auto result = fp::identify(image.data(), size, EXPECTED_DIGEST);
    CHECK(result.status == fp::Status::UnsupportedSize && !result.extracted()); blocked_control(result);
    fp::Span span{nullptr, 0}; CHECK(!fp::normalized_span(image.data(), size, 0, span));
  }
  for (unsigned i = 0; i < 32; ++i) {
    uint8_t wrong[32]; std::memcpy(wrong, EXPECTED_DIGEST, sizeof(wrong)); wrong[i] ^= 1;
    const auto result = fp::identify(image.data(), image.size(), wrong);
    CHECK(result.status == fp::Status::Unrecognized && !result.extracted()); blocked_control(result);
    CHECK(result.scalars.lower_seed == 0 && result.scalars.high_test_addend == 0);
  }
  auto invalid = image; word(invalid, 0x17B8, 0);
  uint8_t wrong[32]{};
  CHECK(fp::identify(invalid.data(), invalid.size(), wrong).status == fp::Status::Unrecognized);
}
static void scalar_variants() {
  const auto image = synthetic_image();
  struct Change { std::size_t offset; uint32_t value; };
  const Change single[] = {{0x5FD8,5}, {0x5FD4,620}, {0x17B4,625},
      {0x17B8,static_cast<uint32_t>(-1242)}, {0x17BC,1248}};
  for (const auto change : single) {
    auto candidate = image; word(candidate, change.offset, change.value);
    const auto result = parse(candidate);
    CHECK(result.status == fp::Status::ScalarVariant && result.extracted());
    CHECK(normalized(candidate) == normalized(image));
  }
  auto candidate = image;
  for (const auto change : single) word(candidate, change.offset, change.value);
  const auto all = parse(candidate);
  CHECK(all.status == fp::Status::ScalarVariant && all.extracted());
  CHECK(all.scalars.lower_seed == 5 && all.scalars.upper_seed == 620);
  CHECK(all.scalars.display_offset == 625 && all.scalars.high_test_addend == -1242);
  CHECK(all.scalars.high_display == 1248);
  const auto assessment = all.research_profile(24,39).assess(24,39);
  CHECK(assessment.mapping_usable && assessment.raw_lower == 8 && assessment.raw_upper == 1007);
  // Correct signed decoding at the negative boundary; rejected without overflow.
  word(candidate, 0x17B8, 0x80000000u);
  const auto minimum = parse(candidate);
  CHECK(minimum.status == fp::Status::InvalidScalars);
  CHECK(minimum.scalars.high_test_addend == std::numeric_limits<int32_t>::min());
  word(candidate, 0x17B8, 0x7FFFFFFFu);
  const auto maximum = parse(candidate);
  CHECK(maximum.status == fp::Status::InvalidScalars);
  CHECK(maximum.scalars.high_test_addend == std::numeric_limits<int32_t>::max());
  word(candidate, 0x17B4, 0); word(candidate, 0x17B8, 0xFFFFFFFFu);
  const auto negative_one = parse(candidate);
  CHECK(negative_one.status == fp::Status::ScalarVariant);
  CHECK(negative_one.scalars.high_test_addend == -1);
}
static void invalid_scalars() {
  const auto image = synthetic_image();
  struct Change { std::size_t offset; uint32_t value; };
  const Change invalid[] = {{0x5FD8,460}, {0x5FD8,0xFFFFFFFFu}, {0x5FD4,4},
      {0x5FD4,10001}, {0x17B4,5001}, {0x17B8,0},
      {0x17B8,static_cast<uint32_t>(-10726)}, {0x17BC,0}, {0x17BC,10001}};
  for (const auto change : invalid) {
    auto candidate = image; word(candidate, change.offset, change.value);
    const auto result = parse(candidate);
    CHECK(result.status == fp::Status::InvalidScalars && !result.extracted());
  }
  auto boundary = image;
  word(boundary, 0x17B8, static_cast<uint32_t>(-725));  // high_start = 0
  CHECK(parse(boundary).status == fp::Status::ScalarVariant);
  word(boundary, 0x17B8, static_cast<uint32_t>(-724));  // high_start = -1
  CHECK(parse(boundary).status == fp::Status::InvalidScalars);
  boundary = image;
  word(boundary, 0x5FD8, 459);  // Largest lower seed strictly below upper seed 460.
  const auto adjacent_seeds = parse(boundary);
  CHECK(adjacent_seeds.status == fp::Status::ScalarVariant && adjacent_seeds.extracted());
  CHECK(adjacent_seeds.scalars.lower_seed == 459 && adjacent_seeds.scalars.upper_seed == 460);
  auto limits = image;
  word(limits, 0x5FD8, 0); word(limits, 0x5FD4, 10000); word(limits, 0x17B4, 5000);
  word(limits, 0x17B8, static_cast<uint32_t>(-15000)); word(limits, 0x17BC, 10000);
  CHECK(parse(limits).status == fp::Status::ScalarVariant);
}
int main() {
  normalization(); recognition_and_bounds(); scalar_variants(); invalid_scalars();
  std::cout << "Firmware profile span/digest-boundary/scalar tests passed (synthetic supplied digest).\n";
}
