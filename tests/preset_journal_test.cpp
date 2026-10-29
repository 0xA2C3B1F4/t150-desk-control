#include "desk_protocol/preset_journal.h"
#include <cstdlib>
#include <iostream>
#include <string>

using namespace desk_protocol;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while (false)

// Frozen vector generated independently with Python struct.pack and zlib.crc32.
// Format: <IHBB + <HHIIIiIHHHHHHHHHhhH + <HH + <I CRC32.
static PresetJournal::Encoded golden() {
  const std::string hex =
      "344b5344400001000100010004000000cc010000d502000062fbffffa4040000"
      "18002700da029c040800e4020600e40208000700faff0100ab00080045040371";
  CHECK(hex.size() == 128);
  PresetJournal::Encoded bytes{};
  for (std::size_t i = 0; i < bytes.size(); ++i)
    bytes[i] = static_cast<uint8_t>(std::strtoul(hex.substr(i * 2, 2).c_str(), nullptr, 16));
  return bytes;
}
static PresetJournal retained() {
  PresetJournal journal;
  journal.state = PresetJournal::State::Retained;
  journal.profile = HeightProfile::known_t150().identity();
  journal.original = 171;
  journal.temporary = 8;
  return journal;
}
static void checksum(PresetJournal::Encoded &bytes) {
  journal_detail::write32(bytes.data() + 60, journal_detail::crc32(bytes.data(), 60));
}
static void rejected(const uint8_t *data, std::size_t size) {
  auto unchanged = retained();
  unchanged.original = 0xBA98;
  const auto before = unchanged.encode();
  CHECK(!PresetJournal::decode(data, size, unchanged));
  CHECK(unchanged.encode() == before);
}
static void rejected(const PresetJournal::Encoded &data) { rejected(data.data(), data.size()); }

static void encoding() {
  const uint8_t check[] = {'1','2','3','4','5','6','7','8','9'};
  CHECK(journal_detail::crc32(check, sizeof(check)) == 0xCBF43926U);
  const auto bytes = golden();
  const auto expected = retained();
  CHECK(expected.encode() == bytes);
  PresetJournal decoded;
  CHECK(PresetJournal::decode(bytes, decoded));
  CHECK(decoded.state == PresetJournal::State::Retained);
  CHECK(decoded.profile == expected.profile);
  CHECK(decoded.original == 171 && decoded.temporary == 8);
  CHECK(decoded.encode() == bytes);
  for (uint16_t original : {uint16_t(0), uint16_t(1), uint16_t(0xFFFF)}) {
    for (uint16_t temporary : {uint16_t(0), uint16_t(256), uint16_t(0xFFFF)}) {
      auto journal = retained();
      journal.original = original;
      journal.temporary = temporary;
      CHECK(PresetJournal::decode(journal.encode(), decoded));
      CHECK(decoded.original == original && decoded.temporary == temporary);
    }
  }
  auto resolved = retained();
  resolved.state = PresetJournal::State::Resolved;
  CHECK(PresetJournal::decode(resolved.encode(), decoded));
  CHECK(decoded.state == PresetJournal::State::Resolved);
  CHECK(decoded.profile == resolved.profile && decoded.original == resolved.original);
}

static void malformed_records() {
  const auto bytes = golden();
  // Every serialized bit, including CRC bits, participates in validation.
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    for (uint8_t bit = 0; bit < 8; ++bit) {
      auto altered = bytes;
      altered[i] ^= static_cast<uint8_t>(1U << bit);
      rejected(altered);
    }
  }
  for (std::size_t length = 0; length < bytes.size(); ++length) rejected(bytes.data(), length);
  std::array<uint8_t, 65> oversized{};
  for (std::size_t i = 0; i < bytes.size(); ++i) oversized[i] = bytes[i];
  rejected(oversized.data(), oversized.size());
  rejected(nullptr, 64);
  // Recomputed checksums must not hide invalid envelope values.
  for (std::size_t offset : {std::size_t(0), std::size_t(1), std::size_t(2), std::size_t(3),
                             std::size_t(4), std::size_t(5), std::size_t(7)}) {
    auto altered = bytes;
    altered[offset] ^= 1;
    checksum(altered);
    rejected(altered);
  }
  for (unsigned state = 2; state <= 255; ++state) {
    auto altered = bytes;
    altered[6] = static_cast<uint8_t>(state);
    checksum(altered);
    rejected(altered);
  }
  rejected(PresetJournal::Encoded{});
}

static void identity() {
  const auto known = HeightProfile::known_t150();
  const auto key = known.identity();
  const auto empty = HeightProfileIdentity{};
  for (uint8_t byte : empty.bytes) CHECK(byte == 0);
  CHECK(key != empty);
  CHECK(key == HeightProfile::known_t150().identity());
  // Every identity byte is compared, including revision, signed values and the
  // high bytes that are zero in the built-in values. A valid envelope carrying
  // another identity must decode without being mistaken for this profile.
  for (std::size_t i = 0; i < key.bytes.size(); ++i) {
    auto changed = key;
    changed.bytes[i] ^= 1;
    CHECK(changed != key);
    CHECK(!(changed == key));
    auto bytes = golden();
    bytes[8 + i] ^= 1;
    checksum(bytes);
    PresetJournal decoded;
    CHECK(PresetJournal::decode(bytes, decoded));
    CHECK(decoded.profile == changed && decoded.profile != key);
  }
  const HeightProfile::Scalars scalars(4, 460, 725, -1182, 1188);
  const auto research = HeightProfile::research(scalars, 24, 39, true);
  const auto research_key = research.identity();
  CHECK(research_key != key);  // Same data is not the approved control policy.
  CHECK(!research.control_approved());
  CHECK(research_key == HeightProfile::research(scalars, 24, 39, false).identity());
  CHECK(research_key != HeightProfile::research(scalars, 25, 39, true).identity());
  CHECK(research_key != HeightProfile::research(scalars, 24, 40, true).identity());
  CHECK(research_key != HeightProfile::research(HeightProfile::Scalars(5,460,725,-1182,1188),24,39,true).identity());
  CHECK(research_key != HeightProfile::research(HeightProfile::Scalars(4,461,725,-1182,1188),24,39,true).identity());
  CHECK(research_key != HeightProfile::research(HeightProfile::Scalars(4,460,726,-1182,1188),24,39,true).identity());
  CHECK(research_key != HeightProfile::research(HeightProfile::Scalars(4,460,725,-1183,1188),24,39,true).identity());
  CHECK(research_key != HeightProfile::research(HeightProfile::Scalars(4,460,725,-1182,1189),24,39,true).identity());
}

static void legacy() {
  const auto historical = LegacyPresetJournal::profile_identity();
  const auto vector = golden();
  for (std::size_t i = 0; i < historical.bytes.size(); ++i)
    CHECK(historical.bytes[i] == vector[8 + i]);
  CHECK(historical == HeightProfile::known_t150().identity());
  for (uint8_t version = 0x31; version <= 0x33; ++version) {
    const uint8_t bytes[] = {version,0x4B,0x53,0x44,0xCD,0xAB,0x34,0x12};
    LegacyPresetJournal old;
    CHECK(LegacyPresetJournal::decode(bytes, sizeof(bytes), old));
    CHECK(old.magic == (0x44534B00U | version));
    CHECK(old.original == 0xABCD && old.temporary == 0x1234);
    auto migrated = retained();
    migrated.original = old.original;
    migrated.temporary = old.temporary;
    PresetJournal decoded;
    CHECK(PresetJournal::decode(migrated.encode(), decoded));
    CHECK(decoded.original == 0xABCD && decoded.temporary == 0x1234);
    for (std::size_t length = 0; length < 8; ++length)
      CHECK(!LegacyPresetJournal::decode(bytes, length, old));
    CHECK(old.original == 0xABCD && old.temporary == 0x1234);
  }
  LegacyPresetJournal out;
  const uint8_t cleared[8]{};
  CHECK(!LegacyPresetJournal::decode(cleared, sizeof(cleared), out));
  uint8_t bad[] = {0x34,0x4B,0x53,0x44,0xCD,0xAB,0x34,0x12,0};
  CHECK(!LegacyPresetJournal::decode(bad, 8, out));
  bad[0] = 0x33;
  CHECK(!LegacyPresetJournal::decode(bad, sizeof(bad), out));
  CHECK(!LegacyPresetJournal::decode(nullptr, 8, out));
  for (std::size_t i = 1; i < 4; ++i) {
    bad[i] ^= 1;
    CHECK(!LegacyPresetJournal::decode(bad, 8, out));
    bad[i] ^= 1;
  }
}

int main() {
  encoding();
  malformed_records();
  identity();
  legacy();
  std::cout << "preset journal tests passed\n";
}
