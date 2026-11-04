#pragma once

#include "protocol.h"

namespace desk_protocol {
// Experimental D7/D8 extension. A response confirms controller acceptance,
// not the elapsed timer or a physically audible sound. Never retry writes.
class CustomTimerBuzzer {
 public:
  static constexpr uint32_t timeout_ms = 3500;
  enum Operation : uint8_t { Probe = 0, SetTimer = 1, CancelTimer = 2, Beep = 3 };
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
    }
    return false;
  }
  bool accept(const Frame& f, uint32_t now) {
    if (!pending_ || static_cast<uint32_t>(now - start_) >= timeout_ms || f.size != 6 ||
        f.bytes[0] != 0xD8 || f.bytes[1] != 1 || f.bytes[2] != op_ || be16(f.bytes + 3) != nonce_ ||
        f.bytes[5] > 3)
      return false;
    pending_ = false;
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
      available_ = false;
      event_ = Event::Timeout;
    }
  }
  // Stop/releases and every competing transaction may proceed immediately.
  // Any interrupted write has an unknown outcome and must not be retried.
  void observe_outgoing(const Frame& f) {
    if (pending_ && !(f.size == 10 && f.bytes[0] == 0xD7 && f.bytes[1] == 1 && f.bytes[2] == op_ &&
                      be16(f.bytes + 3) == nonce_))
      disconnect();
  }
  void disconnect() {
    available_ = false;
    if (pending_) {
      pending_ = false;
      event_ = Event::LinkLost;
    }
  }

 private:
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
    if (!CustomTimerBuzzer::valid(CustomTimerBuzzer::Beep, on_ms, off_ms, count))
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
}  // namespace desk_protocol
