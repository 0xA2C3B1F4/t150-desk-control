#pragma once
#include "protocol.h"
#include <cstring>

namespace desk_protocol {

struct ChildLockReply {
  uint8_t operation{0}, status{0};
  bool locked{false}, physical_gate{false}, storage_fault{false};
  uint16_t nonce{0}, sequence{0};
};

inline bool child_lock_reply(const Frame &frame, ChildLockReply &out) {
  if (frame.size != 15 || frame.bytes[0] != 0xD4 || frame.bytes[1] != 1 ||
      frame.bytes[2] > 2 || frame.bytes[3] > 3 || frame.bytes[4] > 1 ||
      frame.bytes[5] > 1 || frame.bytes[6] > 1 ||
      std::memcmp(frame.bytes + 7, "CL01", 4) != 0) return false;
  ChildLockReply r;
  r.operation = frame.bytes[2]; r.status = frame.bytes[3];
  r.locked = frame.bytes[4]; r.physical_gate = frame.bytes[5]; r.storage_fault = frame.bytes[6];
  r.nonce = be16(frame.bytes + 11); r.sequence = be16(frame.bytes + 13);
  // Every failed storage operation leaves RAM locked and the release gate set.
  // A successful SET must have cleared the storage fault. GET can report one.
  if ((r.storage_fault && (!r.locked || !r.physical_gate)) ||
      (r.locked && !r.physical_gate) ||
      (r.status == 3 && !r.storage_fault) ||
      (r.status == 0 && r.operation && (r.storage_fault || r.locked != (r.operation == 1)))) return false;
  out = r;
  return true;
}

// Single-attempt CL01 GET/SET. The caller owns UART exclusivity, settling and
// cancellation. A cached GET with storage_fault never proves a durable lock.
class ChildLock {
 public:
  void seed(uint16_t nonce) { next_nonce_ = nonce; }
  uint16_t next_nonce() const { return next_nonce_; }
  // Feed accepted telemetry from the controller's shared sequence counter.
  // GETs can be idle for more than half a sequence epoch while UART stays live.
  void observe_sequence(uint16_t sequence) { sequence_ = sequence; have_sequence_ = true; }
  bool probe(uint32_t now) {
    if (busy_) return false;
    request_(0, now); return true;
  }
  bool set(bool locked, uint32_t now) {
    if (busy_ || !supported_) return false;
    state_known_ = false;
    request_(locked ? 1 : 2, now); return true;
  }
  void accept(const Frame &frame, uint32_t now) {
    if (!busy_ || queued_ || frame.size != 15 || frame.bytes[0] != 0xD4 ||
        be16(frame.bytes + 11) != nonce_ || frame.bytes[2] != operation_) return;
    if (static_cast<uint32_t>(now - sent_at_) >= 800) { timeout_(); return; }
    ChildLockReply r;
    if (!child_lock_reply(frame, r) ||
        (have_sequence_ && (static_cast<uint16_t>(r.sequence - sequence_) == 0 ||
                           static_cast<uint16_t>(r.sequence - sequence_) >= 0x8000))) {
      disconnect("CL01 reply invalid or stale; state unknown"); return;
    }
    reply_ = r; sequence_ = r.sequence; have_sequence_ = true; busy_ = false; completed_ = true;
    supported_ = r.status != 1;
    state_known_ = supported_ && !r.storage_fault;
    error_ = r.status == 1 ? "CL01 request rejected; state unknown" :
             r.status == 2 ? "CL01 request busy; no change or retry" :
             r.storage_fault ? "CL01 storage fault; runtime locked, persistence uncertain" : nullptr;
  }
  void tick(uint32_t now, bool fresh) {
    if (!fresh) {
      if (supported_ || busy_ || state_known_) disconnect("CL01 UART unavailable; state unknown");
    } else if (busy_ && static_cast<uint32_t>(now - sent_at_) >= 800) timeout_();
  }
  void disconnect(const char *reason = "CL01 unavailable; state unknown", bool report_failure = true) {
    if (!report_failure) failure_ = nullptr;
    else if (busy_ && operation_ && !queued_) failure_ = reason;
    busy_ = queued_ = supported_ = state_known_ = have_sequence_ = completed_ = false;
    error_ = reason;
  }
  bool take_frame(Frame &frame) {
    if (!queued_) return false;
    frame = frame_; queued_ = false; return true;
  }
  bool take_completed() { const bool result = completed_; completed_ = false; return result; }
  const char *take_failure() { const char *result = failure_; failure_ = nullptr; return result; }
  bool busy() const { return busy_; }
  bool supported() const { return supported_; }
  bool state_known() const { return state_known_; }
  uint8_t operation() const { return operation_; }
  const ChildLockReply &reply() const { return reply_; }
  const char *error() const { return error_; }
 private:
  void request_(uint8_t operation, uint32_t now) {
    frame_ = Frame{}; frame_.size = 5;
    frame_.bytes[0] = 0xD3; frame_.bytes[1] = 1; frame_.bytes[2] = operation;
    operation_ = operation; nonce_ = next_nonce_++;
    frame_.bytes[3] = static_cast<uint8_t>(nonce_ >> 8);
    frame_.bytes[4] = static_cast<uint8_t>(nonce_);
    sent_at_ = now; busy_ = queued_ = true; completed_ = false; error_ = nullptr;
  }
  void timeout_() {
    disconnect(operation_ ? "CL01 SET acknowledgement missing; state and persistence unknown, no retry" :
                           "CL01 GET unavailable; state unknown");
  }
  Frame frame_{};
  ChildLockReply reply_{};
  uint16_t next_nonce_{0}, nonce_{0}, sequence_{0};
  uint32_t sent_at_{0};
  uint8_t operation_{0};
  bool busy_{false}, queued_{false}, supported_{false}, state_known_{false};
  bool have_sequence_{false}, completed_{false};
  const char *error_{nullptr}, *failure_{nullptr};
};
}  // namespace desk_protocol
