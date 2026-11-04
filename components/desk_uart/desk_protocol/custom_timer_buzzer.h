#pragma once

#include "protocol.h"

namespace desk_protocol {
// Experimental D7/D8 extension. A response confirms controller acceptance,
// not the elapsed timer or a physically audible sound. Never retry writes.
class CustomTimerBuzzer {
 public:
  static constexpr uint32_t timeout_ms = 3500;
  enum Operation : uint8_t {
    Probe = 0,
    SetTimer = 1,
    CancelTimer = 2,
    Beep = 3,
    CancelBuzzer = 4,
    ProfileBeep = 9,
    ProfileTimer = 10,
    ProbeProfiles = 0x80,
    CancelProfileTimer = 0x82
  };
  enum class Event : uint8_t {
    None,
    Available,
    Accepted,
    Invalid,
    Busy,
    BackendError,
    Timeout,
    LinkLost
  };
  void seed(uint16_t seed) { nonce_ = seed; }
  bool available() const { return available_; }
  bool pending() const { return pending_; }
  bool is_probe() const { return op_ == Probe || op_ == ProbeProfiles; }
  bool profiles_checked() const { return profiles_checked_; }
  bool profiles_available() const { return available_ && (capabilities_ & 0x72) == 0x72; }
  bool timer_profiles_available() const {
    return profiles_available() && (capabilities_ & 0x81) == 0x81;
  }
  Operation operation() const { return op_; }
  Event take_event() {
    const Event e = event_;
    event_ = Event::None;
    return e;
  }

  bool begin(Operation op, uint16_t a, uint16_t b, uint8_t count, uint32_t now, Frame& out) {
    if (pending_ || (op != Probe && !available_) || !valid(op, a, b, count))
      return false;
    ++nonce_;
    op_ = op;
    start_ = now;
    version_ = 1;
    wire_size_ = 10;
    pending_ = true;
    event_ = Event::None;
    out = Frame{};
    out.size = 10;
    out.bytes[0] = 0xD7;
    out.bytes[1] = 1;
    out.bytes[2] = op;
    out.bytes[3] = static_cast<uint8_t>(nonce_ >> 8);
    out.bytes[4] = static_cast<uint8_t>(nonce_);
    out.bytes[5] = static_cast<uint8_t>(a >> 8);
    out.bytes[6] = static_cast<uint8_t>(a);
    out.bytes[7] = static_cast<uint8_t>(b >> 8);
    out.bytes[8] = static_cast<uint8_t>(b);
    out.bytes[9] = count;
    return true;
  }
  static bool valid(Operation op, uint16_t a, uint16_t b, uint8_t count) {
    switch (op) {
      case Probe:
      case CancelTimer:
        return a == 0 && b == 0 && count == 0;
      case SetTimer:
        return a >= 2 && a <= 720 && b == 0 && count == 0;
      case Beep:
        return a >= 50 && a <= 2000 && b >= 50 && b <= 5000 && count >= 1 && count <= 20;
      default:
        return false;
    }
    return false;
  }
  static bool valid_profile(int on_ms, int off_ms, int count, int volume) {
    return on_ms >= 5 && on_ms <= 2000 && off_ms >= 5 && off_ms <= 5000 && count >= 1 &&
           count <= 20 && volume >= 0 && volume <= 100;
  }
  bool begin_profile(Operation op, uint16_t minutes, uint16_t on_ms, uint16_t off_ms, uint8_t count,
                     uint8_t volume, bool soften, uint32_t now, Frame& out) {
    if (pending_ || !available_) {
      return false;
    }
    const bool profile = op == ProfileBeep || op == ProfileTimer;
    if (profile) {
      if (!valid_profile(on_ms, off_ms, count, volume) ||
          (op == ProfileTimer ? (!timer_profiles_available() || minutes < 2 || minutes > 720)
                              : (!profiles_available() || minutes != 0 || volume == 0))) {
        return false;
      }
    } else if (op != ProbeProfiles && op != CancelBuzzer && op != CancelProfileTimer) {
      return false;
    } else if (minutes || on_ms || off_ms || count || volume || soften ||
               (op != ProbeProfiles && !profiles_available())) {
      return false;
    }
    ++nonce_;
    op_ = op;
    version_ = 3;
    wire_size_ = profile ? (op == ProfileTimer ? 21 : 19) : 5;
    start_ = now;
    pending_ = true;
    event_ = Event::None;
    out = Frame{};
    out.size = wire_size_;
    out.bytes[0] = 0xD7;
    out.bytes[1] = 3;
    out.bytes[2] = wire_operation();
    out.bytes[3] = static_cast<uint8_t>(nonce_ >> 8);
    out.bytes[4] = static_cast<uint8_t>(nonce_);
    if (profile) {
      out.bytes[5] = 0x4E;
      out.bytes[6] = 0x20;
      out.bytes[7] = volume;
      out.bytes[8] = static_cast<uint8_t>(on_ms >> 8);
      out.bytes[9] = static_cast<uint8_t>(on_ms);
      out.bytes[10] = static_cast<uint8_t>(off_ms >> 8);
      out.bytes[11] = static_cast<uint8_t>(off_ms);
      out.bytes[12] = count;
      if (soften) {
        const uint8_t curve[] = {2, 5, 20, 20, 80, 100};
        for (unsigned i = 0; i < sizeof(curve); ++i) {
          out.bytes[13 + i] = curve[i];
        }
      }
      if (op == ProfileTimer) {
        out.bytes[19] = static_cast<uint8_t>(minutes >> 8);
        out.bytes[20] = static_cast<uint8_t>(minutes);
      }
    }
    return true;
  }
  bool accept(const Frame& f, uint32_t now) {
    if (!pending_ || static_cast<uint32_t>(now - start_) >= timeout_ms ||
        f.size != (version_ == 3 ? 8u : 6u) || f.bytes[0] != 0xD8 || f.bytes[1] != version_ ||
        f.bytes[2] != wire_operation() || be16(f.bytes + 3) != nonce_ || f.bytes[5] > 3)
      return false;
    pending_ = false;
    if (op_ == ProbeProfiles) {
      profiles_checked_ = true;
      capabilities_ = f.bytes[5] == 0 ? f.bytes[6] : 0;
      event_ = profiles_available() ? Event::Available : Event::Invalid;
      return true;
    }
    if (f.bytes[5] == 0) {
      if (op_ == Probe) {
        available_ = true;
        event_ = Event::Available;
      } else
        event_ = Event::Accepted;
    } else {
      if (op_ == Probe)
        available_ = false;
      event_ = f.bytes[5] == 1   ? Event::Invalid
               : f.bytes[5] == 2 ? Event::Busy
                                 : Event::BackendError;
    }
    return true;
  }
  void tick(uint32_t now) {
    if (pending_ && static_cast<uint32_t>(now - start_) >= timeout_ms) {
      pending_ = false;
      if (op_ == ProbeProfiles) {
        profiles_checked_ = true;
        capabilities_ = 0;
      } else {
        available_ = false;
        profiles_checked_ = false;
        capabilities_ = 0;
      }
      event_ = Event::Timeout;
    }
  }
  // Stop/releases and every competing transaction may proceed immediately.
  // Any interrupted write has an unknown outcome and must not be retried.
  void observe_outgoing(const Frame& f) {
    if (pending_ && !(f.size == wire_size_ && f.bytes[0] == 0xD7 && f.bytes[1] == version_ &&
                      f.bytes[2] == wire_operation() && be16(f.bytes + 3) == nonce_))
      disconnect();
  }
  void yield_probe() {
    if (pending_ && is_probe()) {
      pending_ = false;
      event_ = Event::None;
    }
  }
  void disconnect() {
    capabilities_ = 0;
    profiles_checked_ = false;
    available_ = false;
    if (pending_) {
      pending_ = false;
      event_ = Event::LinkLost;
    }
  }

 private:
  uint8_t wire_operation() const { return static_cast<uint8_t>(op_) & 0x7F; }
  uint8_t version_{1}, wire_size_{10}, capabilities_{0};
  bool profiles_checked_{false};
  uint32_t start_{0};
  uint16_t nonce_{0};
  Operation op_{Probe};
  Event event_{Event::None};
  bool pending_{false}, available_{false};
};
// A transmitted beep may still run after a missing or rejected acknowledgement.
// This estimate only defers automatic preset keep-awake; explicit controls and
// recovery keep their normal admission and cancellation rules.
class CustomBuzzerDeferral {
 public:
  bool begin(uint16_t on_ms, uint16_t off_ms, uint8_t count, uint32_t now) {
    if (!CustomTimerBuzzer::valid_profile(on_ms, off_ms, count, 50))
      return false;
    const uint32_t proposed = CustomTimerBuzzer::timeout_ms + static_cast<uint32_t>(count) * on_ms +
                              static_cast<uint32_t>(count - 1) * off_ms;
    // A new request can be rejected while an earlier sequence still runs.
    // Preserve its remaining hold, including across the uint32 clock wrap.
    const uint32_t remaining = active(now) ? duration_ - static_cast<uint32_t>(now - started_) : 0;
    started_ = now;
    duration_ = remaining > proposed ? remaining : proposed;
    return true;
  }
  bool active(uint32_t now) const {
    return duration_ && static_cast<uint32_t>(now - started_) < duration_;
  }
  void expire(uint32_t now) {
    if (duration_ && !active(now))
      duration_ = 0;
  }

 private:
  uint32_t started_{0}, duration_{0};
};
// The RTC rounds seconds down, so a remote timer can expire one minute early.
// This hold affects only automatic preset keep-awake, never explicit controls.
class CustomAlarmDeferral {
 public:
  void clear() { end_ = 0; }
  bool begin(uint16_t minutes, uint16_t on_ms, uint16_t off_ms, uint8_t count, uint32_t now) {
    if (minutes < 2 || minutes > 720 ||
        !CustomTimerBuzzer::valid_profile(on_ms, off_ms, count, 50)) {
      return false;
    }
    const uint32_t start = static_cast<uint32_t>(minutes - 1) * 60000;
    const uint32_t end =
        static_cast<uint32_t>(minutes) * 60000 + static_cast<uint32_t>(on_ms) * count +
        static_cast<uint32_t>(off_ms) * (count - 1) + CustomTimerBuzzer::timeout_ms;
    const uint32_t elapsed = now - started_;
    if (end_ && elapsed < end_) {
      const uint32_t old_start = elapsed < start_ ? start_ - elapsed : 0;
      start_ = old_start < start ? old_start : start;
      const uint32_t old_end = end_ - elapsed;
      end_ = old_end > end ? old_end : end;
    } else {
      start_ = start;
      end_ = end;
    }
    started_ = now;
    return true;
  }
  bool active(uint32_t now) const {
    const uint32_t elapsed = now - started_;
    return end_ && elapsed >= start_ && elapsed < end_;
  }
  void expire(uint32_t now) {
    if (end_ && static_cast<uint32_t>(now - started_) >= end_) {
      end_ = 0;
    }
  }

 private:
  uint32_t started_{0}, start_{0}, end_{0};
};
}  // namespace desk_protocol
