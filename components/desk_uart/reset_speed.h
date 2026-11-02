#pragma once
#include "direct_target.h"

namespace desk_protocol {

// RS01 runtime advertisement and replay guards, not firmware authentication.
struct ResetSpeedReply {
  uint8_t operation{0}, result{0}, state{0}, reason{0}, speed{0}, flags{0};
  uint8_t mode{0}, previous{0}, child_lock{0}, fault_a{0}, fault_b{0}, error{0}, direction{0};
  bool admission_diagnostics{false};
  uint8_t start_result{0}, resume_result{0};
  uint16_t nonce{0}, lease{0}, speed_raw{0}, pwm{0}, sequence{0};
  uint32_t token{0}, position_bits{0}, target{0}, tick{0}, velocity_bits{0};
  bool active() const { return state == 1 || state == 2; }
  bool outputs_off() const { return flags & 4; }
  bool lease_active() const { return flags & 8; }
};

inline bool reset_speed_reply(const Frame& f, ResetSpeedReply& out) {
  if (f.size != 48 || f.bytes[0] != 0xD6 || f.bytes[1] != 1 ||
      (f.bytes[2] != 0x0F && f.bytes[2] != 0x1F) || f.bytes[3] > 5 || f.bytes[4] > 6 ||
      f.bytes[5] > 5 || f.bytes[6] > 4 || f.bytes[7] > 1 || (f.bytes[8] & 0xF0) ||
      (f.bytes[11] & 0xF8) ||
      (f.bytes[2] == 0x0F
           ? (f.bytes[46] || f.bytes[47])
           : (f.bytes[46] > 6 || f.bytes[47] > 6 || bool(f.bytes[8] & 1) != (f.bytes[46] == 0) ||
              bool(f.bytes[8] & 2) != (f.bytes[47] == 0))))
    return false;
  ResetSpeedReply r;
  r.operation = f.bytes[3];
  r.result = f.bytes[4];
  r.state = f.bytes[5];
  r.reason = f.bytes[6];
  r.speed = f.bytes[7];
  r.flags = f.bytes[8];
  r.mode = f.bytes[9];
  r.previous = f.bytes[10];
  r.child_lock = f.bytes[11];
  r.nonce = be16(f.bytes + 12);
  r.token = direct_be32(f.bytes + 14);
  r.lease = be16(f.bytes + 18);
  r.speed_raw = be16(f.bytes + 20);
  r.pwm = be16(f.bytes + 22);
  r.position_bits = direct_be32(f.bytes + 24);
  r.target = direct_be32(f.bytes + 28);
  r.tick = direct_be32(f.bytes + 32);
  r.sequence = be16(f.bytes + 36);
  r.fault_a = f.bytes[38];
  r.fault_b = f.bytes[39];
  r.error = f.bytes[40];
  r.direction = f.bytes[41];
  r.velocity_bits = direct_be32(f.bytes + 42);
  r.admission_diagnostics = f.bytes[2] == 0x1F;
  r.start_result = f.bytes[46];
  r.resume_result = f.bytes[47];
  if (r.lease > 1500 || r.lease_active() != (r.lease != 0) || (r.active() && !r.lease_active()) ||
      (r.active() && !r.token) || (!r.active() && r.lease_active()) ||
      ((r.state == 3 || r.state == 4 || r.state == 5) && !r.outputs_off()) ||
      (r.state == 4 && (r.mode || r.previous)) || ((r.flags & 1) && (r.active() || r.child_lock)) ||
      ((r.flags & 2) && (r.state != 3 || r.child_lock)))
    return false;
  out = r;
  return true;
}

// Single-attempt controls with a short controller lease. There is deliberately
// no whole-reset deadline: renewal does not infer progress or completion.
class ResetSpeed {
 public:
  enum Operation : uint8_t { Get = 0, Start = 1, Resume = 2, Renew = 3, Cancel = 4, Speed = 5 };
  void seed(uint16_t nonce) { next_nonce_ = nonce; }
  void observe_sequence(uint16_t sequence) {
    sequence_ = sequence;
    have_sequence_ = true;
  }
  bool probe(uint32_t now) {
    if (busy_ || owned_)
      return false;
    request_(Get, 0, now);
    return true;
  }
  bool begin(bool resume, uint32_t now) {
    if (busy_ || owned_ || !fresh(now) || !reply_.token || reply_.child_lock ||
        !(reply_.flags & (resume ? 2 : 1)) || (resume ? reply_.state != 3 : reply_.active()))
      return false;
    uncertain_ = true;
    request_(resume ? Resume : Start, 0, now);
    return true;
  }
  bool set_speed(bool slow, uint32_t now) {
    if (busy_ || blocking() || !fresh(now) || !reply_.token || !reply_.outputs_off() ||
        reply_.mode || reply_.previous)
      return false;
    request_(Speed, slow ? 1 : 0, now);
    return true;
  }
  // Stop never needs a token. Read-only discovery alone never emits CANCEL.
  bool cancel(uint32_t now) {
    const bool needed = owned_ || uncertain_ ||
                        (known_ && (reply_.active() || reply_.state == 3 || reply_.state == 5)) ||
                        (busy_ && operation_ != Get && operation_ != Speed);
    busy_ = queued_ = owned_ = completed_ = brake_ = false;
    failure_ = nullptr;
    if (!needed)
      return false;
    uncertain_ = true;
    request_(Cancel, 0, now);
    return true;
  }
  void yield_read() {
    if (busy_ && operation_ == Get) {
      busy_ = queued_ = false;
    }
  }
  void accept(const Frame& f, uint32_t now) {
    if (!busy_ || queued_ || f.size != 48 || f.bytes[0] != 0xD6 || be16(f.bytes + 12) != nonce_ ||
        f.bytes[3] != operation_)
      return;
    if (static_cast<uint32_t>(now - sent_at_) >= 800) {
      timeout_(true);
      return;
    }
    ResetSpeedReply r;
    if (!reset_speed_reply(f, r) ||
        (have_sequence_ && (static_cast<uint16_t>(r.sequence - sequence_) == 0 ||
                            static_cast<uint16_t>(r.sequence - sequence_) >= 0x8000))) {
      fail_(operation_ == Get ? "RS01 status reply invalid or stale; capability unverified"
                              : "RS01 reply invalid or stale; reset ownership lost");
      return;
    }
    const bool motion_op = operation_ == Start || operation_ == Resume || operation_ == Renew;
    const bool completed = r.state == 4 && r.outputs_off() && !r.lease_active();
    // Completion clears the firmware token before the next RENEW reaches it.
    // Its STALE result is successful completion only for our bound, owned
    // renewal and the explicit completed/off/no-lease/cleared-token state.
    const bool owned_completion =
        operation_ == Renew && owned_ && r.result == 2 && completed && !r.token;
    if (r.result == 0 &&
        ((motion_op && !completed && (!r.active() || !r.lease_active() || r.child_lock)) ||
         (operation_ == Cancel && (r.active() || !r.outputs_off())) ||
         (operation_ == Speed && r.speed != argument_))) {
      fail_("RS01 acknowledgement inconsistent; reset ownership lost");
      return;
    }
    if (operation_ == Renew && owned_ && r.result && !owned_completion)
      brake_ = true;
    reply_ = r;
    sequence_ = r.sequence;
    have_sequence_ = known_ = supported_ = true;
    received_at_ = now;
    busy_ = false;
    completed_ = true;
    // Only our acknowledged START/RESUME or its RENEW owns a lease. A GET of
    // somebody else's active operation never authorizes renewal.
    owned_ = r.result == 0 && motion_op && r.active();
    if (owned_)
      renewed_at_ = now;
    uncertain_ = r.active();
    error_ = r.result && !owned_completion ? "RS01 request rejected; no retry" : nullptr;
  }
  void tick(uint32_t now, bool link_fresh) {
    if (!link_fresh) {
      if (known_ || busy_ || owned_)
        disconnect("RS01 UART unavailable; state unknown");
      return;
    }
    if (busy_ && static_cast<uint32_t>(now - sent_at_) >= 800) {
      timeout_(false);
      return;
    }
    if (owned_ && !fresh(now)) {
      fail_("RS01 status stale; reset state unknown");
      return;
    }
    if (owned_ && !busy_ && static_cast<uint32_t>(now - renewed_at_) >= 300)
      request_(Renew, 0, now);
  }
  void disconnect(const char* reason) {
    if (owned_ || (busy_ && (operation_ == Start || operation_ == Resume || operation_ == Renew)))
      brake_ = true;
    if (owned_ || (known_ && (reply_.active() || reply_.state == 3 || reply_.state == 5)) ||
        (busy_ && operation_ != Get && operation_ != Speed))
      uncertain_ = true;
    const bool report = busy_ && operation_ != Get;
    busy_ = queued_ = owned_ = known_ = supported_ = have_sequence_ = completed_ = false;
    error_ = reason;
    if (report)
      failure_ = reason;
  }
  bool take_frame(Frame& f) {
    if (!queued_)
      return false;
    f = frame_;
    queued_ = false;
    return true;
  }
  bool take_completed() {
    const bool value = completed_;
    completed_ = false;
    return value;
  }
  const char* take_failure() {
    const char* value = failure_;
    failure_ = nullptr;
    return value;
  }
  bool take_brake() {
    const bool value = brake_;
    brake_ = false;
    return value;
  }
  bool fresh(uint32_t now) const {
    return known_ && static_cast<uint32_t>(now - received_at_) < 750;
  }
  bool busy() const { return busy_; }
  bool owned() const { return owned_; }
  bool known() const { return known_; }
  bool supported() const { return supported_; }
  bool blocking() const {
    return uncertain_ || (known_ && (reply_.active() || reply_.state == 3 || reply_.state == 5)) ||
           (busy_ && operation_ != Get);
  }
  bool maintenance_safe(uint32_t now) const {
    return !blocking() || (!busy_ && fresh(now) && reply_.outputs_off() && !reply_.lease_active() &&
                           !reply_.active());
  }
  uint8_t operation() const { return operation_; }
  const ResetSpeedReply& reply() const { return reply_; }
  const char* error() const { return error_; }

 private:
  void timeout_(bool late) {
    fail_(operation_ == Get     ? "RS01 status unavailable; capability unverified"
          : operation_ == Speed ? "RS01 speed acknowledgement missing; state unknown, no retry"
          : late                ? "RS01 acknowledgement late; reset state unknown, no retry"
                                : "RS01 acknowledgement missing; reset state unknown, no retry");
  }
  void request_(uint8_t operation, uint8_t argument, uint32_t now) {
    frame_ = Frame{};
    frame_.size = 10;
    frame_.bytes[0] = 0xD5;
    frame_.bytes[1] = 1;
    frame_.bytes[2] = operation;
    nonce_ = next_nonce_++;
    operation_ = operation;
    argument_ = argument;
    frame_.bytes[3] = static_cast<uint8_t>(nonce_ >> 8);
    frame_.bytes[4] = static_cast<uint8_t>(nonce_);
    const uint32_t token = operation == Get || operation == Cancel ? 0 : reply_.token;
    for (uint8_t i = 0; i < 4; ++i)
      frame_.bytes[5 + i] = static_cast<uint8_t>(token >> (24 - 8 * i));
    frame_.bytes[9] = argument;
    busy_ = queued_ = true;
    completed_ = false;
    sent_at_ = now;
    error_ = nullptr;
  }
  void fail_(const char* reason) { disconnect(reason); }
  Frame frame_{};
  ResetSpeedReply reply_{};
  uint16_t next_nonce_{0}, nonce_{0}, sequence_{0};
  uint32_t sent_at_{0}, received_at_{0}, renewed_at_{0};
  uint8_t operation_{0}, argument_{0};
  bool busy_{false}, queued_{false}, known_{false}, supported_{false}, owned_{false},
      uncertain_{false};
  bool have_sequence_{false}, completed_{false}, brake_{false};
  const char *error_{nullptr}, *failure_{nullptr};
};
}  // namespace desk_protocol
