#pragma once
#include "direct_target.h"
#include "identity.h"

namespace desk_protocol {

// Read-only, exclusive UART transaction for the supported controller identity.
// EE/03 has no nonce: require two rejections separated by a fresh identity reply.
// This selects a compatible backend, not a cryptographic firmware identity.
class TargetDetection {
 public:
  enum class Backend { Unknown, Preset, Direct };
  void seed(uint16_t nonce) { probe_.seed(nonce); }
  uint16_t next_nonce() const { return probe_.next_nonce(); }
  void reset() {
    probe_.disconnect(); probe_.cancel();
    stage_ = Stage::Idle; backend_ = Backend::Unknown;
    queued_ = false; index_ = 0; have_sequence_ = false; error_ = nullptr;
  }
  bool start(uint32_t now) {
    if (stage_ != Stage::Idle) return false;
    stage_ = Stage::Identity; index_ = 0; identity_(now); return true;
  }
  bool start(uint32_t now, uint16_t sequence) {
    if (!start(now)) return false;
    sequence_ = sequence; have_sequence_ = true; return true;
  }
  void tick(uint32_t now) {
    if (busy() && static_cast<uint32_t>(now - sent_at_) >= 800)
      fail_("Firmware detection timed out; target control unavailable");
  }
  void accept(const Frame &f, uint32_t now) {
    tick(now);
    if (!busy()) return;
    Height h;
    if (decode_height(f, h)) { advance_(h.sequence); return; }
    if (queued_) return;
    if (stage_ == Stage::Probe || stage_ == Stage::Confirm) {
      if (f.size && f.bytes[0] == 0xD2) {
        if (f.size >= 11 && be16(f.bytes + 9) != probe_.pending_nonce()) return;
        DirectReply reply;
        if (direct_reply(f, reply) && reply.op == 0 && reply.requested == 0 && reply.status != 0) {
          fail_("DT02 capability GET rejected; target control unavailable"); return;
        }
        probe_.accept(f, now);
        if (probe_.known()) {
          if (stage_ == Stage::Confirm) fail_("Conflicting firmware capability replies");
          else { backend_ = Backend::Direct; stage_ = Stage::Done; }
        } else fail_("Invalid or unbound DT02 capability reply");
        return;
      }
      if (!f.size || f.bytes[0] != 0xEE) return;
      if (f.size != 4 || f.bytes[1] != 3 ||
          static_cast<uint16_t>(be16(f.bytes + 2) - sequence_) > 32 || !advance_(be16(f.bytes + 2))) {
        fail_("Unbound or unexpected firmware error reply"); return;
      }
      probe_.cancel();
      if (stage_ == Stage::Confirm) { backend_ = Backend::Preset; stage_ = Stage::Done; }
      else { stage_ = Stage::Barrier; index_ = 0; identity_(now); }
      return;
    }
    if (f.size && f.bytes[0] == 0xEE) { fail_("Unexpected error during firmware identity check"); return; }
    if (stage_ == Stage::Identity || stage_ == Stage::Barrier) {
      IdentityText value;
      if (!decode_identity(f, value) || value.selector != selector_(index_)) return;
      if (std::strcmp(value.text, expected_(index_)) || !advance_(value.sequence)) {
        fail_("Controller identity unsupported for automatic target control"); return;
      }
      if (stage_ == Stage::Barrier) { stage_ = Stage::Confirm; start_probe_(now); }
      else if (++index_ < 6) identity_(now);
      else { stage_ = Stage::ScaleA; parameter_(0x0B, now); }
      return;
    }
    const uint8_t selector = stage_ == Stage::ScaleA ? 0x0B : 0x0C;
    if (f.size != 6 || f.bytes[0] != 0x16 || f.bytes[1] != selector) return;
    if (be16(f.bytes + 2) != (selector == 0x0B ? 24 : 39) || !advance_(be16(f.bytes + 4))) {
      fail_("Height coefficients unsupported for automatic target control"); return;
    }
    if (stage_ == Stage::ScaleA) { stage_ = Stage::ScaleB; parameter_(0x0C, now); }
    else { stage_ = Stage::Probe; start_probe_(now); }
  }
  bool take_frame(Frame &out) {
    if (!queued_) return false;
    queued_ = false;
    if (stage_ == Stage::Probe || stage_ == Stage::Confirm) return probe_.take_frame(out);
    out = frame_; return true;
  }
  bool busy() const { return stage_ != Stage::Idle && stage_ != Stage::Done && stage_ != Stage::Failed; }
  bool idle() const { return stage_ == Stage::Idle; }
  Backend backend() const { return backend_; }
  const char *error() const { return error_; }
 private:
  enum class Stage { Idle, Identity, ScaleA, ScaleB, Probe, Barrier, Confirm, Done, Failed };
  static uint8_t selector_(uint8_t index) { return index == 5 ? 9 : index + 3; }
  static const char *expected_(uint8_t index) {
    const char *const values[] = {"T150-22-03", "LUMI LEGEND GROUP", "1.5.0", "1.6", "1.9.0118", "21/01/18"};
    return values[index];
  }
  bool advance_(uint16_t seq) {
    const uint16_t delta = static_cast<uint16_t>(seq - sequence_);
    if (have_sequence_ && (delta == 0 || delta >= 0x8000)) return false;
    sequence_ = seq; have_sequence_ = true; return true;
  }
  void identity_(uint32_t now) { read_identity(selector_(index_), frame_); queued_ = true; sent_at_ = now; }
  void parameter_(uint8_t selector, uint32_t now) {
    frame_ = Frame{}; frame_.size = 4; frame_.bytes[0] = 0x15; frame_.bytes[1] = selector;
    queued_ = true; sent_at_ = now;
  }
  void start_probe_(uint32_t now) { probe_.probe(now); queued_ = true; sent_at_ = now; }
  void fail_(const char *error) { stage_ = Stage::Failed; backend_ = Backend::Unknown; queued_ = false; error_ = error; }
  DirectTarget probe_{};
  Frame frame_{};
  Stage stage_{Stage::Idle};
  Backend backend_{Backend::Unknown};
  uint32_t sent_at_{0};
  uint16_t sequence_{0};
  uint8_t index_{0};
  bool queued_{false}, have_sequence_{false};
  const char *error_{nullptr};
};
}  // namespace desk_protocol
