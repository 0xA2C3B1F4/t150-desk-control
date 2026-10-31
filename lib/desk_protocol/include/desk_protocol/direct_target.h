#pragma once
#include "protocol.h"
#include <cstring>

namespace desk_protocol {

// DT02 runtime advertisement, not cryptographic firmware attestation.
struct DirectReply {
  uint8_t op{0}, status{0}, features{0}, mode{0}, previous{0};
  uint16_t nonce{0}, requested{0}, current{0}, guards{0};
  int32_t target{0};
};
inline uint32_t direct_be32(const uint8_t *p) {
  return (static_cast<uint32_t>(be16(p)) << 16) | be16(p + 2);
}
inline bool direct_reply(const Frame &f, DirectReply &out) {
  if (f.size != 37 || f.bytes[0] != 0xD2 || f.bytes[1] != 1 ||
      f.bytes[2] != 3 || std::memcmp(f.bytes + 5, "DT02", 4) != 0 ||
      f.bytes[3] > 1 || f.bytes[4] > 6 ||
      direct_be32(f.bytes + 19) != 6 || direct_be32(f.bytes + 23) != 747 ||
      be16(f.bytes + 27) != 24 || be16(f.bytes + 29) != 39 ||
      (be16(f.bytes + 33) & ~uint16_t(0x3F))) return false;
  DirectReply r;
  r.op = f.bytes[3]; r.status = f.bytes[4]; r.features = f.bytes[2];
  r.nonce = be16(f.bytes + 9); r.requested = be16(f.bytes + 11);
  r.current = be16(f.bytes + 13);
  const uint32_t raw = direct_be32(f.bytes + 15);
  r.target = static_cast<int32_t>(raw <= 0x7FFFFFFFU ? static_cast<int64_t>(raw) :
                                  static_cast<int64_t>(raw) - 0x100000000LL);
  r.mode = f.bytes[31]; r.previous = f.bytes[32]; r.guards = be16(f.bytes + 33);
  out = r; return true;
}
inline int32_t direct_committed_target(uint16_t s, uint16_t c) {
  int32_t t = s;
  if (static_cast<int32_t>(s) > static_cast<int32_t>(c) + 4) t += 7;
  else if (static_cast<int32_t>(s) < static_cast<int32_t>(c) - 4) t -= 6;
  return t < 6 ? 6 : t > 747 ? 747 : t;
}

// One GET/SET transaction; no preset, persistence, movement retry or retarget queue.
// Caller supplies fresh, stable telemetry, handles one brake event, and owns Stop.
class DirectTarget {
 public:
  enum class Phase { Unknown, Probe, Ready, Check, SetAck, Active, Fault };
  void seed(uint16_t nonce) { next_nonce_ = nonce; }
  uint16_t next_nonce() const { return next_nonce_; }
  uint16_t pending_nonce() const { return nonce_; }
  bool probe(uint32_t now) {
    if (busy()) return false;
    error_ = nullptr; request_(0, 0, Phase::Probe, now); return true;
  }
  bool begin(uint16_t s, uint16_t q, uint32_t now) {
    if (busy() || s < 6 || s > 747) return false;
    requested_ = s; baseline_q_ = q; motion_seen_ = false; error_ = nullptr;
    request_(0, 0, Phase::Check, now); return true;
  }
  void accept(const Frame &f, uint32_t now) {
    if (!waiting() || queued_ || f.size != 37 || f.bytes[0] != 0xD2 ||
        be16(f.bytes + 9) != nonce_) return;
    if (static_cast<uint32_t>(now - sent_at_) >= 800) { fail_("DT02 reply arrived after deadline"); return; }
    const uint8_t op = phase_ == Phase::SetAck ? 1 : 0;
    if (f.bytes[3] != op) return;
    DirectReply r;
    if (!direct_reply(f, r) || r.requested != (op ? requested_ : 0)) {
      fail_("DT02 capability, profile or reply binding mismatch"); return;
    }
    reply_ = r; known_ = true;
    if (op == 0 && r.status != 0) { fail_("DT02 GET rejected"); return; }
    if (phase_ == Phase::Probe) { phase_ = Phase::Ready; return; }
    if (phase_ == Phase::Check) {
      if (r.guards || r.mode || r.previous || r.current < 6 || r.current > 747) {
        phase_ = Phase::Ready; error_ = "DT02 controller inhibited, busy or outside profile"; return;
      }
      request_(1, requested_, Phase::SetAck, now); return;
    }
    if (r.status != 0) {
      // Only a strictly bound explicit rejection proves this SET was not accepted.
      phase_ = Phase::Ready; set_sent_ = false; error_ = "DT02 SET explicitly rejected"; return;
    }
    if (r.current < 6 || r.current > 747 || r.guards || r.mode != 1 || r.previous ||
        r.target != direct_committed_target(requested_, r.current)) {
      fail_("DT02 SET acknowledgement inconsistent; braking"); return;
    }
    phase_ = Phase::Active; accepted_at_ = now; accepted_ = true;
  }
  void tick(uint32_t now, bool fresh, uint32_t stable_ms) {
    if (!fresh) {
      if (known_ || busy()) fail_("UART freshness lost; direct capability invalidated");
      return;
    }
    if (waiting() && static_cast<uint32_t>(now - sent_at_) >= 800)
      fail_(set_sent_ ? "DT02 SET acknowledgement uncertain; braking" : "DT02 capability response missing");
    if (phase_ == Phase::Active && static_cast<uint32_t>(now - accepted_at_) >= 2000 &&
        stable_ms >= 1500 && (motion_seen_ || static_cast<uint32_t>(now - accepted_at_) >= 3000)) {
      phase_ = Phase::Ready; set_sent_ = false;
    }
  }
  void observe_q(uint16_t q) { if (active() && q != baseline_q_) motion_seen_ = true; }
  void cancel() {
    queued_ = brake_ = set_sent_ = accepted_ = false;
    phase_ = known_ ? Phase::Ready : Phase::Unknown;
    error_ = nullptr;
  }
  void disconnect() { fail_("Direct capability lost; select target again"); }
  bool take_frame(Frame &f) {
    if (!queued_) return false;
    f = frame_; queued_ = false;
    if (phase_ == Phase::SetAck) set_sent_ = true;
    return true;
  }
  bool take_brake() { const bool b = brake_; brake_ = false; return b; }
  bool take_accepted() { const bool b = accepted_; accepted_ = false; return b; }
  bool active() const { return phase_ == Phase::Active; }
  bool awaiting_set() const { return phase_ == Phase::SetAck; }
  bool set_sent() const { return set_sent_; }
  bool waiting() const { return phase_ == Phase::Probe || phase_ == Phase::Check || phase_ == Phase::SetAck; }
  bool brake_pending() const { return brake_; }
  bool busy() const { return waiting() || active() || brake_; }
  bool known() const { return known_; }
  bool sleep_wrap() const { return known_ && (reply_.features & 2); }
  uint16_t requested() const { return requested_; }
  const DirectReply &reply() const { return reply_; }
  const char *error() const { return error_; }
  Phase phase() const { return phase_; }
 private:
  void request_(uint8_t op, uint16_t s, Phase phase, uint32_t now) {
    frame_ = Frame{}; frame_.size = 7; frame_.bytes[0] = 0xD1; frame_.bytes[1] = 1;
    frame_.bytes[2] = op; nonce_ = next_nonce_++;
    frame_.bytes[3] = static_cast<uint8_t>(nonce_ >> 8); frame_.bytes[4] = static_cast<uint8_t>(nonce_);
    frame_.bytes[5] = static_cast<uint8_t>(s >> 8); frame_.bytes[6] = static_cast<uint8_t>(s);
    phase_ = phase; sent_at_ = now; queued_ = true;
  }
  void fail_(const char *error) {
    brake_ = brake_ || set_sent_ || active();
    phase_ = Phase::Fault; known_ = queued_ = set_sent_ = accepted_ = false; error_ = error;
  }
  Frame frame_{};
  DirectReply reply_{};
  Phase phase_{Phase::Unknown};
  uint16_t next_nonce_{0}, nonce_{0}, requested_{0}, baseline_q_{0};
  uint32_t sent_at_{0}, accepted_at_{0};
  bool queued_{false}, known_{false}, set_sent_{false}, brake_{false}, accepted_{false}, motion_seen_{false};
  const char *error_{nullptr};
};
}  // namespace desk_protocol
