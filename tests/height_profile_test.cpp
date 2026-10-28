#include "desk_protocol/height_profile.h"
#include "desk_protocol/presets.h"
#include <cstdlib>
#include <iostream>
#include <initializer_list>

using namespace desk_protocol;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while (false)

// Frozen pre-profile behavior: this oracle deliberately does not use profile
// helpers. Exhaustive checks catch changes in rounding, range and selection.
static uint32_t legacy_height(uint16_t q) {
  if (q >= 4 && q <= 7) return 730;
  if (q >= 457 && q <= 459) return 1188;
  return static_cast<uint32_t>(q) + 725;
}
static bool legacy_target(uint16_t tenths, uint16_t &raw) {
  if (tenths < 730 || tenths > 1180) return false;
  if (tenths == 730) { raw = 8; return true; }
  raw = static_cast<uint16_t>((static_cast<uint32_t>(tenths - 725) * 39 + 23) / 24);
  return raw >= 8 && raw <= 740;
}
static void legacy_compatibility() {
  const auto profile = HeightProfile::known_t150();
  for (uint32_t input = 0; input <= 65535; ++input) {
    const uint16_t value = static_cast<uint16_t>(input);
    const uint32_t height = legacy_height(value);
    const auto display = metric_display(value);
    CHECK(display.valid);
    CHECK(display.before_rounding_tenths_cm == height);
    CHECK(display.displayed_tenths_cm == (height < 1000 ? height : height / 10 * 10));
    CHECK(display.decimal_places == (height < 1000 ? 1 : 0));

    uint16_t expected = 0xFFFF, actual = 0xFFFF;
    CHECK(profile.scaled_q(value, actual));
    CHECK(actual == input * 24 / 39);
    const bool accepted = legacy_target(value, expected);
    CHECK(metric_target_raw(value, actual) == accepted);
    if (accepted) CHECK(actual == expected);
    CHECK(profile.target_raw(value, actual) == accepted);
    if (accepted) CHECK(actual == expected);

    const bool preset_valid = value >= 8 && value <= 740;
    CHECK(preset_height(value, actual) == preset_valid);
    if (preset_valid) CHECK(actual == legacy_height(static_cast<uint16_t>(input * 24 / 39)));
    CHECK(profile.keep_awake_raw(value) == (value >= 6 && value <= 740));
  }
}

using Error = HeightProfile::Error;
static HeightProfile::Scalars known_scalars() {
  return HeightProfile::Scalars(4, 460, 725, -1182, 1188);
}
static void candidate(const HeightProfile &profile, uint32_t tenths,
                      uint16_t first, uint16_t last, uint32_t count) {
  const auto result = profile.inverse_candidates(tenths, 24, 39);
  CHECK(result.error == Error::None);
  CHECK(result.count == count);
  if (count) { CHECK(result.first == first); CHECK(result.last == last); }
}
static void inverse_and_formatting() {
  const auto profile = HeightProfile::known_t150();
  const auto checked = profile.assess(24, 39);
  CHECK(checked.extraction_supported && checked.mapping_usable && checked.control_approved);
  CHECK(checked.raw_lower == 6 && checked.raw_upper == 747);
  CHECK(checked.bias_lower == 12 && checked.bias_upper == 740);
  candidate(profile, 728, 6, 6, 1);
  candidate(profile, 729, 0, 0, 0);
  candidate(profile, 730, 7, 12, 6);
  candidate(profile, 731, 0, 0, 0);
  candidate(profile, 732, 0, 0, 0);
  candidate(profile, 1005, 455, 456, 2);
  candidate(profile, 1188, 743, 747, 5);
  candidate(profile, 1189, 0, 0, 0);
  candidate(profile, 0xFFFFFFFFU, 0, 0, 0);

  // Existing control selection intentionally differs from exact inversion at
  // the lower plateau. This refactor must not silently change issued commands.
  uint16_t raw = 0;
  CHECK(profile.target_raw(730, raw) && raw == 8);
  CHECK(profile.target_raw(731, raw) && raw == 10);
  CHECK(profile.preset_height(raw, raw) && raw == 730);
  CHECK(profile.target_raw(732, raw) && raw == 12);
  CHECK(profile.preset_height(raw, raw) && raw == 730);
  CHECK(profile.target_raw(1005, raw) && raw == 455);
  CHECK(profile.preset_height(456, raw) && raw == 1005);
  CHECK(profile.display(280).displayed_tenths_cm == 1000);
  CHECK(profile.display(274).decimal_places == 1);
  CHECK(profile.display(274).displayed_tenths_cm == 999);
  CHECK(profile.display(275).decimal_places == 0);
  CHECK(profile.display(275).displayed_tenths_cm == 1000);
  for (uint16_t q = 457; q <= 459; ++q) {
    CHECK(profile.display(q).before_rounding_tenths_cm == 1188);
    CHECK(profile.display(q).displayed_tenths_cm == 1180);
  }
  CHECK(profile.display(460).before_rounding_tenths_cm == 1185);

  // Firmware applies these corrections after choosing a preset. Candidates
  // remain stored preset values, not attempts to cancel the motion correction.
  CHECK(HeightProfile::motion_target(100, 96) == 100);
  CHECK(HeightProfile::motion_target(100, 95) == 107);
  CHECK(HeightProfile::motion_target(100, 104) == 100);
  CHECK(HeightProfile::motion_target(100, 105) == 94);
  CHECK(HeightProfile::motion_target(0, 5) == -6);
  CHECK(HeightProfile::motion_target(65535, 0) == 65542);
}
static void assessment_and_binary32() {
  const auto known = HeightProfile::known_t150();
  CHECK(known.scale_a() == 24 && known.scale_b() == 39);
  CHECK(known.control_approved());
  const auto unknown_boot = known.assess();
  CHECK(!unknown_boot.mapping_usable && !unknown_boot.control_approved);
  CHECK(unknown_boot.error == Error::BootScaleUnknown);
  CHECK(known.assess(24, 0).error == Error::InvalidScale);
  CHECK(known.assess(25, 39).error == Error::BootScaleChanged);
  CHECK(known.inverse_candidates(730, 0, 0).error == Error::BootScaleUnknown);

  // A built-in-like mapping alone cannot authorize controller transactions.
  const auto unrecognized = HeightProfile::research(known_scalars(), 24, 39, false);
  CHECK(!unrecognized.extraction_supported() && !unrecognized.control_approved());
  CHECK(unrecognized.assess(24, 39).error == Error::UnsupportedProfile);
  const auto research = HeightProfile::research(known_scalars(), 24, 39, true);
  CHECK(research.assess(24, 39).mapping_usable);
  auto values = known_scalars();
  const auto snapshot = HeightProfile::research(values, 24, 39, true);
  values.display_offset = 625;
  values.upper_seed = 620;
  CHECK(snapshot.display(8).before_rounding_tenths_cm == 733);
  CHECK(snapshot.assess(24, 39).raw_upper == 747);
  candidate(snapshot, 730, 7, 12, 6);
  CHECK(!research.assess(24, 39).control_approved && !research.control_approved());

  const auto synthetic = HeightProfile::research(
      HeightProfile::Scalars(5, 620, 625, -1242, 1248), 24, 39, true);
  const auto checked = synthetic.assess(24, 39);
  CHECK(checked.mapping_usable && !checked.control_approved);
  CHECK(checked.raw_lower == 8 && checked.raw_upper == 1007);
  CHECK(synthetic.display(4).before_rounding_tenths_cm == 630);
  CHECK(synthetic.display(619).before_rounding_tenths_cm == 1248);
  candidate(synthetic, 630, 8, 12, 5);
  uint16_t out = 0;
  CHECK(!synthetic.target_raw(800, out));
  CHECK(!synthetic.preset_height(122, out));
  CHECK(!synthetic.keep_awake_raw(123));
  Frame frame;
  TemporaryPreset move(synthetic);
  CHECK(!move.begin(800, 0) && !move.begin_keep_awake(0));
  CHECK(!move.take_frame(frame) && !move.busy());
  PresetSettings setting(synthetic);
  CHECK(!setting.begin_read(1, 0) && !setting.begin_write(1, 800, 0));
  CHECK(!setting.take_frame(frame) && !setting.busy());

  // This literal counterexample requires two binary32 rounding operations;
  // an integer rational implementation would truncate to 7809 instead.
  const auto fractional = HeightProfile::research(known_scalars(), 7508, 8403, true);
  CHECK(fractional.scaled_q(8741, out) && out == 7810);
  CHECK(uint32_t(8741) * 7508 / 8403 == 7809);
  const auto scaled_overflow = HeightProfile::research(known_scalars(), 65535, 1, true);
  CHECK(!scaled_overflow.scaled_q(65535, out));
  out = 0xABCD;
  CHECK(!scaled_overflow.scaled_q(65535, out) && out == 0xABCD);
  CHECK(!known.target_raw(729, out) && out == 0xABCD);
  CHECK(!known.preset_height(741, out) && out == 0xABCD);
  CHECK(HeightProfile::research(known_scalars(), 65535, 1, true)
            .assess(65535, 1).error == Error::RawBounds);
  for (const auto &invalid : {
      HeightProfile::research(known_scalars(), 0, 39, true),
      HeightProfile::research(known_scalars(), 24, 0, true)}) {
    CHECK(!invalid.scaled_q(100, out));
    CHECK(invalid.assess(24, 39).error == Error::InvalidScale);
  }

  const auto wider = HeightProfile::research(HeightProfile::Scalars(4, 620, 725, -1182, 1188), 24, 39, true);
  CHECK(wider.assess(24, 39).error == Error::NonmonotonicMapping);
  CHECK(wider.inverse_candidates(1188, 24, 39).error == Error::NonmonotonicMapping);
  CHECK(HeightProfile::research(HeightProfile::Scalars(4, 460, 725, -1182, 100), 24, 39, true)
            .assess(24, 39).error == Error::NonmonotonicMapping);
  CHECK(HeightProfile::research(known_scalars(), 1, 65535, true)
            .assess(1, 65535).error == Error::RawBounds);
  CHECK(HeightProfile::research(HeightProfile::Scalars(460, 4, 725, -1182, 1188), 24, 39, true)
            .assess(24, 39).error == Error::InvalidScalars);
  CHECK(HeightProfile::research(HeightProfile::Scalars(10, 460, 725, -1182, 1188), 24, 39, true)
            .assess(24, 39).error == Error::HomingOutsideBounds);
  CHECK(HeightProfile::research(HeightProfile::Scalars(5, 6, 725, -1182, 1188), 24, 39, true)
            .assess(24, 39).error == Error::NoBiasInterval);
  const auto bad = HeightProfile::research(HeightProfile::Scalars(4, 460, 725, 1, 1188), 24, 39, true);
  CHECK(bad.assess(24, 39).error == Error::InvalidScalars);
  CHECK(!bad.display(0).valid);
  for (const auto &extreme : {
      HeightProfile::Scalars(4, 0xFFFFFFFFU, 725, -1182, 1188),
      HeightProfile::Scalars(4, 460, 0xFFFFFFFFU, -1182, 1188),
      HeightProfile::Scalars(4, 460, 725, INT32_MIN, 1188),
      HeightProfile::Scalars(4, 460, 725, -1182, 0xFFFFFFFFU)}) {
    const auto invalid = HeightProfile::research(extreme, 24, 39, true);
    CHECK(invalid.assess(24, 39).error == Error::InvalidScalars);
    CHECK(!invalid.display(65535).valid);
    const auto candidates = invalid.inverse_candidates(730, 24, 39);
    CHECK(candidates.error == Error::InvalidScalars && candidates.count == 0);
  }
}

static Frame parameter(uint8_t selector, uint16_t raw) {
  Frame f; f.size = 6; f.bytes[0] = 0x16; f.bytes[1] = selector;
  f.bytes[2] = static_cast<uint8_t>(raw >> 8); f.bytes[3] = static_cast<uint8_t>(raw);
  return f;
}
static void research_recovery_is_inert() {
  for (const auto &profile : {
      HeightProfile::research(HeightProfile::Scalars(5, 620, 625, -1242, 1248), 24, 39, true),
      HeightProfile::research(known_scalars(), 24, 39, true),
      HeightProfile::research(known_scalars(), 24, 39, false)}) {
    TemporaryPreset move(profile);
    move.recover(456, 122);
    const auto retained = [&]() {
      Frame frame;
      CHECK(move.dirty() && move.failed());
      CHECK(move.original() == 456 && move.target() == 122);
      CHECK(move.phase() == TemporaryPreset::Phase::Fault);
      CHECK(!move.take_frame(frame) && !move.take_brake());
    };
    retained();
    CHECK(!move.retry_restore(1)); retained();
    CHECK(!move.restore_backup(2)); retained();
    CHECK(!move.check_recovery(3)); retained();
    CHECK(!move.accept_current(4)); retained();

    // Imported backups must remain available without allowing recovery,
    // cancellation or reconnection to bypass the profile's command gate.
    move.cancel(5);
    move.tick(3000, true, 2500, true); retained();
    move.connection_lost(3001);
    move.tick(6000, true, 2500, true); retained();
    auto reply = parameter(0x1F, 456);
    reply.bytes[5] = 1;
    move.accept(reply, 6001); retained();
    reply.bytes[5] = 2;
    move.accept(reply, 6002); retained();
    reply = parameter(0x1F, 122);
    reply.bytes[5] = 3;
    move.accept(reply, 6003); retained();
    move.commit(true, 6004);
    move.journal_cleared(true);
    move.tick(9000, true, 2500, true); retained();
  }
}
static void keep_awake_preserves_raw() {
  const auto profile = HeightProfile::known_t150();
  uint16_t tenths = 0, selected = 0;
  CHECK(profile.preset_height(123, tenths) && tenths == 800);
  CHECK(profile.target_raw(tenths, selected) && selected == 122);
  TemporaryPreset move(profile);
  Frame frame;
  CHECK(move.begin_keep_awake(0));
  CHECK(move.take_frame(frame) && frame.bytes[1] == 0x17);
  move.accept(parameter(0x17, 123), 1);
  CHECK(move.take_frame(frame) && frame.bytes[1] == 0x1F);
  move.accept(parameter(0x1F, 456), 2);
  CHECK(move.phase() == TemporaryPreset::Phase::Backup);
  move.commit(true, 3);
  CHECK(move.take_frame(frame) && frame.bytes[0] == 0x19);
  CHECK(be16(frame.bytes + 2) == 123);
}
int main() {
  legacy_compatibility();
  inverse_and_formatting();
  assessment_and_binary32();
  research_recovery_is_inert();
  keep_awake_preserves_raw();
  std::cout << "Height profile compatibility and conversion checks passed\n";
}
