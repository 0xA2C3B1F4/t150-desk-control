#pragma once
#include "target.h"

namespace desk_protocol {
inline bool read_preset_raw(uint8_t slot, Frame &out) {
  out = Frame{};
  if (slot < 1 || slot > 3) return false;
  out.size = 4; out.bytes[0] = 0x15; out.bytes[1] = 0x1C + slot;
  return true;
}
// The same recovered metric profile as target.h. Preserve decimal precision
// above 100 cm instead of applying the panel's whole-centimetre formatting.
inline bool preset_height(uint16_t raw, uint16_t &tenths) {
  if (raw < 8 || raw > 740) return false;
  tenths = static_cast<uint16_t>(metric_display(static_cast<uint16_t>(static_cast<uint32_t>(raw) * 24 / 39)).before_rounding_tenths_cm);
  return tenths >= 730 && tenths <= 1180;
}

// One explicit settings transaction, with no motion or EEPROM-save command.
// Every read/write validates active scaling. A write is sent once, then ACKed
// and read back. Timeout/cancel does not undo an already transmitted write.
class PresetSettings {
 public:
  enum class Phase { Idle, ScaleA, ScaleB, Read, WriteAck, Verify, Done };
  struct Result {
    uint8_t slot;
    bool write, success, value_known;
    uint16_t raw;
    const char *error;
  };
  bool begin_read(uint8_t slot, uint32_t now) { return begin_(slot, false, 0, now); }
  bool begin_write(uint8_t slot, uint16_t tenths, uint32_t now) {
    uint16_t raw;
    if (!metric_target_raw(tenths, raw)) return false;
    return begin_(slot, true, raw, now);
  }
  bool busy() const { return phase_ != Phase::Idle; }
  bool writing() const { return write_; }
  uint8_t slot() const { return slot_; }
  bool take_frame(Frame &out) {
    if (!queued_) return false;
    out = frame_; queued_ = false; return true;
  }
  bool take_result(Result &out) {
    if (phase_ != Phase::Done) return false;
    out = {slot_, write_, error_ == nullptr, known_, actual_, error_};
    phase_ = Phase::Idle; return true;
  }
  void cancel(const char *reason) { if (busy() && phase_ != Phase::Done) finish_(reason); }
  void tick(uint32_t now) {
    if (busy() && phase_ != Phase::Done && static_cast<uint32_t>(now - started_) >= 700)
      finish_("Preset setting timeout; value unknown, no write retry");
  }
  void accept(const Frame &f, uint32_t now) {
    if (!busy() || phase_ == Phase::Done || static_cast<uint32_t>(now - started_) >= 700) return;
    uint16_t v;
    switch (phase_) {
      case Phase::ScaleA:
        if (!target_parameter(f, 0x0B, v)) return;
        if (v != 24) { finish_("Unsupported preset height scale A"); return; }
        read_(0x0C, Phase::ScaleB, now); break;
      case Phase::ScaleB:
        if (!target_parameter(f, 0x0C, v)) return;
        if (v != 39) { finish_("Unsupported preset height scale B"); return; }
        if (write_) {
          write_preset_raw(slot_, expected_, frame_);
          queued_ = true; phase_ = Phase::WriteAck; started_ = now;
        } else read_(0x1C + slot_, Phase::Read, now);
        break;
      case Phase::WriteAck: {
        Ack a;
        if (!decode_ack(f, a) || a.kind != 0x1A || a.first != 0x1C + slot_) return;
        if (a.second != 0) { finish_("Preset setting rejected by controller"); return; }
        read_(0x1C + slot_, Phase::Verify, now); break;
      }
      case Phase::Read:
      case Phase::Verify:
        if (!target_parameter(f, 0x1C + slot_, v)) return;
        actual_ = v; known_ = true;
        finish_(phase_ == Phase::Verify && v != expected_ ? "Preset setting readback mismatch; actual value shown" : nullptr);
        break;
      default: break;
    }
  }
 private:
  bool begin_(uint8_t slot, bool write, uint16_t raw, uint32_t now) {
    if (busy() || slot < 1 || slot > 3) return false;
    slot_ = slot; write_ = write; expected_ = raw; actual_ = 0; known_ = false; error_ = nullptr;
    read_(0x0B, Phase::ScaleA, now); return true;
  }
  void read_(uint8_t selector, Phase next, uint32_t now) {
    frame_ = Frame{}; frame_.size = 4; frame_.bytes[0] = 0x15; frame_.bytes[1] = selector;
    queued_ = true; phase_ = next; started_ = now;
  }
  void finish_(const char *error) { error_ = error; queued_ = false; phase_ = Phase::Done; }
  Phase phase_{Phase::Idle};
  Frame frame_{};
  uint32_t started_{0};
  uint16_t expected_{0}, actual_{0};
  uint8_t slot_{0};
  bool write_{false}, known_{false}, queued_{false};
  const char *error_{nullptr};
};
}  // namespace desk_protocol
