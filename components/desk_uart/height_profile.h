#pragma once
#include <cstdint>
#include <array>
#include <limits>

namespace desk_protocol {
// Canonical configured semantics, independent of units, entity names and user limits.
// This is not evidence of a connected controller's firmware or boot coefficients.
struct HeightProfileIdentity {
  std::array<uint8_t, 48> bytes{};
  bool operator==(const HeightProfileIdentity &other) const { return bytes == other.bytes; }
  bool operator!=(const HeightProfileIdentity &other) const { return !(*this == other); }
};

struct MetricDisplay {
  uint32_t before_rounding_tenths_cm{0};
  uint32_t displayed_tenths_cm{0};
  uint8_t decimal_places{0};
  bool valid{false};
};

// Coordinate mapping and control policy are separate. The built-in profile is
// the existing configured T150 assumption, not identification of a live dump.
// Research profiles describe the same code family with different scalar data;
// passing their mathematical checks never authorizes device control.
class HeightProfile {
 public:
  struct Scalars {
    uint32_t lower_seed, upper_seed, display_offset;
    int32_t high_test_addend;
    uint32_t high_display;
    Scalars(uint32_t lower, uint32_t upper, uint32_t offset,
            int32_t high_addend, uint32_t high)
        : lower_seed(lower), upper_seed(upper), display_offset(offset),
          high_test_addend(high_addend), high_display(high) {}
  };
  enum class Error {
    None, UnsupportedProfile, InvalidScalars, InvalidScale, BootScaleUnknown,
    BootScaleChanged, RawBounds, ScaledCoordinateOverflow,
    NonmonotonicMapping, HomingOutsideBounds, NoBiasInterval
  };
  struct Assessment {
    bool extraction_supported{false};
    bool mapping_usable{false};
    // Requires both usable mathematics and the built-in control policy. This
    // still does not establish live identity or physical stopping heights.
    bool control_approved{false};
    Error error{Error::None};
    uint16_t raw_lower{0}, raw_upper{0}, bias_lower{0}, bias_upper{0};
  };
  struct Candidates {
    Error error{Error::None};
    uint16_t first{0}, last{0};
    uint32_t count{0};  // Zero with Error::None means an unrepresented height.
  };

  static HeightProfile known_t150() {
    return HeightProfile(Scalars(4, 460, 725, -1182, 1188), 24, 39, true, true);
  }
  // Recognition is supplied by an independent extractor, not inferred from A/B.
  // Copies the values; subsequent edits of the caller's Scalars cannot alter it.
  static HeightProfile research(const Scalars &values, uint16_t current_a,
                                uint16_t current_b, bool extraction_supported) {
    return HeightProfile(values, current_a, current_b, extraction_supported, false);
  }
  HeightProfileIdentity identity() const {
    HeightProfileIdentity result;
    std::size_t offset = 0;
    const auto u16 = [&result, &offset](uint16_t value) {
      result.bytes[offset++] = static_cast<uint8_t>(value);
      result.bytes[offset++] = static_cast<uint8_t>(value >> 8);
    };
    const auto u32 = [&result, &offset](uint32_t value) {
      for (uint8_t shift = 0; shift < 32; shift += 8)
        result.bytes[offset++] = static_cast<uint8_t>(value >> shift);
    };
    // Bump these revisions whenever mapping or control semantics change.
    u16(1); u16(builtin_ ? 1 : 0);
    u32(values_.lower_seed); u32(values_.upper_seed); u32(values_.display_offset);
    u32(static_cast<uint32_t>(values_.high_test_addend)); u32(values_.high_display);
    u16(a_); u16(b_); u16(control_min_tenths()); u16(control_max_tenths());
    u16(builtin_ ? 8 : 0); u16(builtin_ ? 740 : 0);
    u16(builtin_ ? 6 : 0); u16(builtin_ ? 740 : 0);
    u16(8); u16(7); u16(static_cast<uint16_t>(-6)); u16(1);
    return result;
  }
  uint16_t scale_a() const { return a_; }
  uint16_t scale_b() const { return b_; }
  bool extraction_supported() const { return recognized_; }
  // Only the fixed factory can approve the legacy operational policy. Runtime
  // freshness and A/B guards remain the transaction caller's responsibility.
  bool control_approved() const { return builtin_; }
  uint16_t control_min_tenths() const { return builtin_ ? 730 : 0; }
  uint16_t control_max_tenths() const { return builtin_ ? 1180 : 0; }

  // Telemetry already contains Q. Endpoint mapping precedes panel formatting;
  // presets and target entities use before_rounding_tenths_cm for precision.
  MetricDisplay display(uint16_t q) const {
    MetricDisplay result;
    if (!valid_scalars_()) return result;
    const uint32_t high_start = static_cast<uint32_t>(high_start_());
    const uint32_t height = (q >= 4 && q <= 7) ? values_.display_offset + 5 :
        (q >= high_start && q <= high_start + 2) ? values_.high_display :
        static_cast<uint32_t>(q) + values_.display_offset;
    result.before_rounding_tenths_cm = height;
    result.decimal_places = height < 1000 ? 1 : 0;
    result.displayed_tenths_cm = height < 1000 ? height : (height / 10) * 10;
    result.valid = true;
    return result;
  }

  // Firmware rounds the multiply and divide separately to binary32. Integer
  // rational floor is not equivalent for arbitrary coefficients. Restrict this
  // helper to the positive, non-wrapping uint16 coordinate domain.
  bool scaled_q(uint16_t raw, uint16_t &q) const {
    static_assert(std::numeric_limits<float>::is_iec559 &&
                  std::numeric_limits<float>::digits == 24,
                  "Height profiles require IEEE-754 binary32 float");
    if (!a_ || !b_) return false;
    const volatile float product = static_cast<float>(raw) * static_cast<float>(a_);
    const volatile float quotient = product / static_cast<float>(b_);
    if (!(quotient >= 0.0f && quotient < 65536.0f)) return false;
    q = static_cast<uint16_t>(quotient);
    return true;
  }

  // Preserve the existing selection policy, including requests 73.1/73.2 cm
  // whose chosen raw coordinates lie in the 73.0 cm display plateau. This is
  // deliberately separate from the exact mathematical inverse below.
  bool target_raw(uint16_t tenths, uint16_t &raw) const {
    if (!builtin_ || tenths < control_min_tenths() || tenths > control_max_tenths()) return false;
    if (tenths == 730) { raw = 8; return true; }
    const uint16_t q = static_cast<uint16_t>(tenths - values_.display_offset);
    raw = static_cast<uint16_t>((static_cast<uint32_t>(q) * b_ + a_ - 1) / a_);
    return raw >= 8 && raw <= 740;
  }
  bool preset_height(uint16_t raw, uint16_t &tenths) const {
    if (!builtin_ || raw < 8 || raw > 740) return false;
    uint16_t q;
    if (!scaled_q(raw, q)) return false;
    tenths = static_cast<uint16_t>(display(q).before_rounding_tenths_cm);
    return tenths >= control_min_tenths() && tenths <= control_max_tenths();
  }
  bool keep_awake_raw(uint16_t raw) const { return builtin_ && raw >= 6 && raw <= 740; }

  // Boot A/B must come from separate evidence. Current UART reads do not prove
  // which coefficients were used for the one-time startup bound calculation.
  // The derived bounds and bias interval are not physical travel limits.
  Assessment assess(uint16_t boot_a = 0, uint16_t boot_b = 0) const {
    Assessment result;
    result.extraction_supported = recognized_;
    if (!recognized_) return failed_(result, Error::UnsupportedProfile);
    if (!valid_scalars_()) return failed_(result, Error::InvalidScalars);
    if (!a_ || !b_) return failed_(result, Error::InvalidScale);
    if (!boot_a && !boot_b) return failed_(result, Error::BootScaleUnknown);
    if (!boot_a || !boot_b) return failed_(result, Error::InvalidScale);
    if (a_ != boot_a || b_ != boot_b) return failed_(result, Error::BootScaleChanged);
    const uint64_t low = static_cast<uint64_t>(values_.lower_seed) * boot_b / boot_a;
    const uint64_t high = static_cast<uint64_t>(values_.upper_seed) * boot_b / boot_a;
    if (low >= high || high > 65535) return failed_(result, Error::RawBounds);
    result.raw_lower = static_cast<uint16_t>(low);
    result.raw_upper = static_cast<uint16_t>(high);
    uint32_t previous = 0;
    for (uint32_t raw = result.raw_lower; raw <= result.raw_upper; ++raw) {
      uint16_t q;
      if (!scaled_q(static_cast<uint16_t>(raw), q))
        return failed_(result, Error::ScaledCoordinateOverflow);
      const uint32_t height = display(q).before_rounding_tenths_cm;
      if (raw != result.raw_lower && height < previous)
        return failed_(result, Error::NonmonotonicMapping);
      previous = height;
    }
    if (low > 8 || high < 8) return failed_(result, Error::HomingOutsideBounds);
    if (high < 7 || low + 6 > high - 7) return failed_(result, Error::NoBiasInterval);
    result.bias_lower = static_cast<uint16_t>(low + 6);
    result.bias_upper = static_cast<uint16_t>(high - 7);
    result.mapping_usable = true;
    result.control_approved = builtin_;
    return result;
  }

  // Bounded, allocation-free exact inverse over the checked integer domain.
  // Assessment is performed here so a result cannot be reused for a different
  // profile. Plateaus return all candidates; gaps do not select a nearby value.
  Candidates inverse_candidates(uint32_t tenths, uint16_t boot_a, uint16_t boot_b) const {
    Candidates result;
    const Assessment checked = assess(boot_a, boot_b);
    result.error = checked.error;
    if (!checked.mapping_usable) return result;
    for (uint32_t raw = checked.raw_lower; raw <= checked.raw_upper; ++raw) {
      uint16_t q;
      if (!scaled_q(static_cast<uint16_t>(raw), q)) {
        result.error = Error::ScaledCoordinateOverflow;
        result.count = 0;
        return result;
      }
      if (display(q).before_rounding_tenths_cm != tenths) continue;
      if (!result.count) result.first = static_cast<uint16_t>(raw);
      result.last = static_cast<uint16_t>(raw);
      ++result.count;
    }
    return result;
  }

  // Integer-position model of the correction after copying a preset. Actual
  // position may be fractional; a truncated UART position cannot predict its
  // branch. Never subtract this correction from the value sent to the desk.
  static int32_t motion_target(uint16_t preset_raw, uint16_t current_raw) {
    const int32_t preset = preset_raw, current = current_raw;
    return preset > current + 4 ? preset + 7 :
           preset < current - 4 ? preset - 6 : preset;
  }

 private:
  HeightProfile(const Scalars &values, uint16_t a, uint16_t b, bool recognized, bool builtin)
      : values_(values), a_(a), b_(b), recognized_(recognized), builtin_(builtin) {}
  int64_t high_start_() const {
    return -static_cast<int64_t>(values_.high_test_addend) - values_.display_offset;
  }
  // Plausibility bounds shared with the scalar-variant extractor.
  bool valid_scalars_() const {
    return values_.lower_seed < values_.upper_seed && values_.upper_seed <= 10000 &&
        values_.display_offset <= 5000 && high_start_() >= 0 && high_start_() <= 10000 &&
        values_.high_display > 0 && values_.high_display <= 10000;
  }
  static Assessment failed_(Assessment result, Error error) {
    result.error = error;
    return result;
  }
  Scalars values_;
  uint16_t a_, b_;
  bool recognized_, builtin_;
};
}  // namespace desk_protocol
