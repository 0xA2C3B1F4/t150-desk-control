#pragma once
#include "protocol.h"

namespace desk_protocol {
// T-key workflow from the recovered controller profile. UART ACKs confirm only
// flag writes. There is no readback of the selection, RTC alarm or physical key.
// The operator must leave the local keys alone throughout the transaction.
class TimerSequence {
 public:
  enum class Phase { Idle, Quiet, Pressed, Acks, Gap, Settle, Drain };
  bool begin(uint16_t minutes, uint32_t now) {
    if (busy() || release_fault_ || (minutes != 0 && minutes != 30 && minutes != 60 && minutes != 90 && minutes != 120))
      return false;
    remaining_ = minutes == 0 ? 5 : minutes / 30;
    error_ = nullptr;
    completed_ = false;
    started_ = now;
    phase_ = Phase::Quiet;
    return true;
  }
  bool busy() const { return phase_ != Phase::Idle; }
  bool completed() const { return completed_; }
  bool release_fault() const { return release_fault_; }
  const char *error() const { return error_; }
  Phase phase() const { return phase_; }
  bool take_frame(Frame &frame) {
    if (!queued_) return false;
    frame = output_; queued_ = false; return true;
  }
  void accept(const Frame &frame, uint32_t now) {
    press_ack_.accept(frame, now);
    release_ack_.accept(frame, now);
  }
  // Call before draining RX, so a stream of input cannot postpone release.
  void poll_release(uint32_t now) {
    Frame release;
    if (!pulse_.poll(now, release)) return;
    queue_release_(release, now);
    if (static_cast<uint32_t>(now - last_press_) >= 500) {
      // A delayed hold can have a different meaning, including a units change.
      // Do not allow more remote buttons merely because release is later ACKed.
      release_fault_ = true;
      fail_("T release delayed; timer state unknown, no retry", now);
      return;
    }
    if (phase_ != Phase::Drain) phase_ = Phase::Acks;
  }
  // A failed sequence has already released the key and may still be draining
  // its ACK. Preserve that failure when Stop cancels the remaining work.
  void cancel(uint32_t now) { if (busy() && !error_) fail_("Timer sequence interrupted; timer state unknown", now); }
  void tick(uint32_t now, bool fresh, uint32_t stable_ms) {
    if (!busy()) return;
    poll_release(now);
    if (press_ack_.expired(now)) fail_("No T press ACK; timer state unknown, no retry", now);
    if (release_ack_.expired(now)) {
      release_fault_ = true;
      error_ = "No T release ACK; remote buttons blocked until ESP restart";
      press_ack_ = AckTracker{};
      phase_ = Phase::Idle;
      return;
    }
    if (phase_ == Phase::Drain) {
      if (!release_ack_.pending()) phase_ = Phase::Idle;
      return;
    }
    if (!fresh || stable_ms < 1500) {
      fail_("Telemetry lost or height changed; timer state unknown, no retry", now);
      return;
    }
    switch (phase_) {
      case Phase::Quiet:
        // Let a pre-existing T selection expire and its selection byte reset.
        // Includes the delayed single-click event and the 4000-tick menu timer.
        if (static_cast<uint32_t>(now - started_) >= 5000) press_(now);
        break;
      case Phase::Acks:
        if (!press_ack_.pending() && !release_ack_.pending()) {
          --remaining_;
          phase_ = remaining_ ? Phase::Gap : Phase::Settle;
          if (!remaining_) started_ = now;
        }
        break;
      case Phase::Gap: {
        const uint32_t elapsed = now - last_press_;
        // A long scheduling/ACK delay could allow the menu to commit between
        // clicks. Abort instead of continuing from an unknown selection.
        if (elapsed >= 2000) fail_("T sequence delayed; timer state unknown, no retry", now);
        else if (elapsed >= 1150) press_(now);
        break;
      }
      case Phase::Settle:
        if (static_cast<uint32_t>(now - started_) >= 5000) {
          completed_ = true;
          phase_ = Phase::Idle;
        }
        break;
      default: break;
    }
  }

 private:
  void press_(uint32_t now) {
    if (!pulse_.begin(Button::Timer, now, output_)) return;
    queued_ = true;
    press_ack_.begin(0x18, 9, 1, now);
    last_press_ = now;
    phase_ = Phase::Pressed;
  }
  void queue_release_(const Frame &release, uint32_t now) {
    output_ = release;
    queued_ = true;
    release_ack_.begin(0x18, 9, 0, now);
  }
  void fail_(const char *reason, uint32_t now) {
    error_ = reason;
    completed_ = false;
    // Discard an unsent press, but preserve an already queued release.
    if (queued_ && output_.bytes[2] == 1) queued_ = false;
    Frame release;
    if (pulse_.cancel(release)) queue_release_(release, now);
    press_ack_ = AckTracker{};
    phase_ = release_ack_.pending() ? Phase::Drain : Phase::Idle;
  }
  Phase phase_{Phase::Idle};
  ButtonPulse pulse_{};
  AckTracker press_ack_{}, release_ack_{};
  Frame output_{};
  uint32_t started_{0}, last_press_{0};
  uint8_t remaining_{0};
  bool queued_{false}, completed_{false}, release_fault_{false};
  const char *error_{nullptr};
};
}  // namespace desk_protocol
