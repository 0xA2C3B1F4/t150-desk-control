#pragma once

// Portable C++11. No I/O, clock, heap allocation, or platform headers.
#include <cstddef>
#include <cstdint>
#include "height_profile.h"

namespace desk_protocol {

constexpr uint8_t START = 0xFA, END = 0xFD, ESCAPE = 0xFE;
constexpr std::size_t MAX_BODY = 64;  // kind + arguments, excludes checksum
constexpr std::size_t MAX_WIRE = 2 * (MAX_BODY + 1) + 2;

struct Frame {
  uint8_t bytes[MAX_BODY]{};
  std::size_t size{0};
};
struct Packet {
  uint8_t bytes[MAX_WIRE]{};
  std::size_t size{0};
};

inline uint16_t be16(const uint8_t *p) {
  return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}
inline bool reserved(uint8_t b) { return b == START || b == END || b == ESCAPE; }

// Supports the framing grammar; only known command builders below should be
// used against a device. Unknown, checksum-valid messages are not commands.
inline bool encode(const Frame &frame, Packet &out) {
  out.size = 0;
  if (frame.size == 0 || frame.size > MAX_BODY) return false;
  out.bytes[out.size++] = START;
  uint8_t checksum = 0;
  for (std::size_t i = 0; i <= frame.size; ++i) {
    const uint8_t b = i == frame.size ? checksum : frame.bytes[i];
    if (i < frame.size) checksum ^= b;
    if (reserved(b)) out.bytes[out.size++] = ESCAPE;
    out.bytes[out.size++] = b;
  }
  out.bytes[out.size++] = END;
  return true;
}

enum class ParseResult { None, FrameReady, BadChecksum, BadLength, Overflow };

class Parser {
 public:
  explicit Parser(uint32_t inter_byte_timeout_ms = 250) : timeout_(inter_byte_timeout_ms) {}
  void reset() { active_ = escaped_ = false; used_ = 0; }
  bool expire(uint32_t now) {
    if (active_ && static_cast<uint32_t>(now - last_byte_) >= timeout_) {
      reset();
      return true;
    }
    return false;
  }
  // Caller supplies a monotonic millisecond clock; uint32 wrap is supported.
  // 'out' changes only on FrameReady. A fresh unescaped FA resynchronizes.
  ParseResult feed(uint8_t b, uint32_t now, Frame &out) {
    expire(now);
    last_byte_ = now;
    if (!active_) {
      if (b == START) { active_ = true; used_ = 0; }
      return ParseResult::None;
    }
    if (escaped_) {
      escaped_ = false;
      return append_(b);
    }
    if (b == START) { used_ = 0; return ParseResult::None; }
    if (b == ESCAPE) { escaped_ = true; return ParseResult::None; }
    if (b != END) return append_(b);
    const std::size_t n = used_;
    reset();
    if (n < 2) return ParseResult::BadLength;
    uint8_t checksum = 0;
    for (std::size_t i = 0; i < n; ++i) checksum ^= buffer_[i];
    if (checksum != 0) return ParseResult::BadChecksum;
    out.size = n - 1;
    for (std::size_t i = 0; i < out.size; ++i) out.bytes[i] = buffer_[i];
    return ParseResult::FrameReady;
  }

 private:
  ParseResult append_(uint8_t b) {
    if (used_ == sizeof(buffer_)) { reset(); return ParseResult::Overflow; }
    buffer_[used_++] = b;
    return ParseResult::None;
  }
  uint8_t buffer_[MAX_BODY + 1]{};
  std::size_t used_{0};
  uint32_t timeout_, last_byte_{0};
  bool active_{false}, escaped_{false};
};

enum class Button : uint8_t { Up = 3, Down = 4, Memory = 5, Preset1 = 6, Preset2 = 7, Preset3 = 8, Timer = 9 };

inline Frame status_query() {
  Frame f; f.size = 1; f.bytes[0] = 0xA1; return f;
}
inline bool telemetry(uint8_t channel, uint16_t period, Frame &out) {
  out = Frame{};
  // 0xFFFF disables. Zero period is deliberately not offered.
  if ((channel != 2 && channel != 3) || period == 0) return false;
  out.size = 7; out.bytes[0] = 0x13; out.bytes[1] = channel;
  out.bytes[3] = static_cast<uint8_t>(period >> 8);
  out.bytes[4] = static_cast<uint8_t>(period);
  return true;
}
inline bool button(Button button, bool pressed, Frame &out) {
  out = Frame{};
  const auto selector = static_cast<uint8_t>(button);
  if (selector < 3 || selector > 9) return false;
  out.size = 3; out.bytes[0] = 0x17; out.bytes[1] = selector;
  out.bytes[2] = pressed ? 1 : 0;
  return true;
}
// Direct controlled braking, observed during upward/downward preset movement.
// This is NOT a hard PWM cut or a universal emergency stop. The fourth body
// byte is required: selector01 does not use the ordinary button layout.
// ACK18/01/00 reports reception, not physical stop. Do not retry blindly:
// each request can recalculate the braking target from the current position.
inline Frame stop() {
  Frame f; f.size = 4; f.bytes[0] = 0x17; f.bytes[1] = 0x01;
  return f;  // body17 01 00 00 -> FA17 01 00 00 16 FD
}

// Raw controller coordinate, NOT centimetres. May later be autosaved by the
// controller. This builder neither recalls the slot nor validates travel limits.
inline bool write_preset_raw(uint8_t slot, uint16_t raw, Frame &out) {
  out = Frame{};
  if (slot < 1 || slot > 3) return false;
  out.size = 4; out.bytes[0] = 0x19; out.bytes[1] = static_cast<uint8_t>(0x1C + slot);
  out.bytes[2] = static_cast<uint8_t>(raw >> 8); out.bytes[3] = static_cast<uint8_t>(raw);
  return true;
}

// DownwardBase is shared with homing. Normal downward cutoff uses base+1200.
// 0x10 aliases the same parameter; expose only canonical selector0x0F.
enum class LoadLimit : uint8_t { Upward = 0x0E, DownwardBase = 0x0F };
inline bool valid_load_limit(LoadLimit limit) {
  return limit == LoadLimit::Upward || limit == LoadLimit::DownwardBase;
}
inline bool read_load_limit(LoadLimit limit, Frame &out) {
  out = Frame{};
  if (!valid_load_limit(limit)) return false;
  out.size = 4; out.bytes[0] = 0x15; out.bytes[1] = static_cast<uint8_t>(limit);
  return true;
}
inline bool write_load_limit(LoadLimit limit, uint16_t raw, Frame &out) {
  out = Frame{};
  if (!valid_load_limit(limit)) return false;
  out.size = 4; out.bytes[0] = 0x19; out.bytes[1] = static_cast<uint8_t>(limit);
  out.bytes[2] = static_cast<uint8_t>(raw >> 8); out.bytes[3] = static_cast<uint8_t>(raw);
  return true;
}
struct LoadLimitValue {
  LoadLimit limit{LoadLimit::Upward};
  uint16_t raw{0}, sequence{0};
};
inline bool decode_load_limit(const Frame &f, LoadLimitValue &out) {
  if (f.size != 6 || f.bytes[0] != 0x16) return false;
  const auto limit = static_cast<LoadLimit>(f.bytes[1]);
  if (!valid_load_limit(limit)) return false;
  out.limit = limit; out.raw = be16(f.bytes + 2); out.sequence = be16(f.bytes + 4);
  return true;
}
inline uint32_t downward_motion_cutoff(uint16_t base) {
  return static_cast<uint32_t>(base) + 1200;
}

struct Height { uint16_t q{0}, sequence{0}; };
inline bool decode_height(const Frame &f, Height &out) {
  if (f.size != 32 || f.bytes[0] != 0x05) return false;
  out.q = be16(f.bytes + 19); out.sequence = be16(f.bytes + 30); return true;
}
struct Status { uint8_t coarse_state{0}; uint16_t sequence{0}; };
inline bool decode_status(const Frame &f, Status &out) {
  if (f.size != 4 || f.bytes[0] != 0xA2) return false;
  out.coarse_state = f.bytes[1]; out.sequence = be16(f.bytes + 2); return true;
}
struct Ack {
  uint8_t kind{0}, first{0}, second{0}, reserved_byte{0};
  uint16_t sequence{0};
};
inline bool decode_ack(const Frame &f, Ack &out) {
  // Parameter replies are shorter: kind, selector, result, sequence BE16.
  // Unlike 14/18, they have no additional reserved byte before the sequence.
  if (f.size == 5 && f.bytes[0] == 0x1A) {
    out.kind = f.bytes[0]; out.first = f.bytes[1]; out.second = f.bytes[2];
    out.reserved_byte = 0; out.sequence = be16(f.bytes + 3); return true;
  }
  if (f.size != 6 || (f.bytes[0] != 0x14 && f.bytes[0] != 0x18)) return false;
  out.kind = f.bytes[0]; out.first = f.bytes[1]; out.second = f.bytes[2];
  out.reserved_byte = f.bytes[3]; out.sequence = be16(f.bytes + 4); return true;
}

// Backward-compatible shorthand for the configured T150 display profile.
// Channel-2 telemetry is already Q; do not apply the raw-position scale again.
inline MetricDisplay metric_display(uint16_t q) {
  return HeightProfile::known_t150().display(q);
}

class HeightMonitor {
 public:
  bool observe(const Frame &f, uint32_t now) {
    Height h;
    if (!decode_height(f, h)) return false;
    // Duplicate/out-of-order sequence alone must not refresh stale telemetry.
    // Once stale, allow a new sequence epoch after controller reboot.
    if (fresh(now) && static_cast<uint16_t>(h.sequence - height_.sequence) >= 0x8000) return false;
    if (fresh(now) && h.sequence == height_.sequence) return false;
    height_ = h; last_ = now; seen_ = true; return true;
  }
  bool fresh(uint32_t now) const { return seen_ && static_cast<uint32_t>(now - last_) < 3000; }
  const Height &height() const { return height_; }
 private:
  Height height_{};
  uint32_t last_{0};
  bool seen_{false};
};

// Single outstanding ACK. It proves handler reception, never movement or stop.
// No request ID is echoed; late identical replies remain ambiguous. No retries.
class AckTracker {
 public:
  bool begin(uint8_t kind, uint8_t first, uint8_t second, uint32_t now, uint32_t timeout_ms = 500) {
    if (pending_ || timeout_ms == 0 || timeout_ms > 60000) return false;
    kind_ = kind; first_ = first; second_ = second; start_ = now; timeout_ = timeout_ms; pending_ = true;
    return true;
  }
  bool accept(const Frame &frame, uint32_t now) {
    // Leave timeout reporting to expired(), so a late frame cannot consume
    // the timeout event before the application's polling code observes it.
    if (!pending_ || static_cast<uint32_t>(now - start_) >= timeout_) return false;
    Ack a;
    if (!decode_ack(frame, a) || a.kind != kind_ || a.first != first_ || a.second != second_ || a.reserved_byte != 0) return false;
    pending_ = false; return true;
  }
  bool expired(uint32_t now) {
    if (pending_ && static_cast<uint32_t>(now - start_) >= timeout_) { pending_ = false; return true; }
    return false;
  }
  bool pending() const { return pending_; }
 private:
  uint8_t kind_{0}, first_{0}, second_{0};
  uint32_t start_{0}, timeout_{0};
  bool pending_{false};
};

// Generates one press, then one release using the caller's local clock. This
// cannot guarantee release if the MCU stops or UART fails. Release != stop.
class ButtonPulse {
 public:
  bool begin(Button selected, uint32_t now, Frame &press) {
    if (active_ || !button(selected, true, press)) return false;
    button_ = selected; start_ = now; active_ = true; return true;
  }
  bool poll(uint32_t now, Frame &release) {
    if (!active_ || static_cast<uint32_t>(now - start_) < 150) return false;
    return cancel(release);
  }
  bool cancel(Frame &release) {
    if (!active_) return false;
    active_ = false; return button(button_, false, release);
  }
  bool active() const { return active_; }
 private:
  Button button_{Button::Preset3};
  uint32_t start_{0};
  bool active_{false};
};
}  // namespace desk_protocol
