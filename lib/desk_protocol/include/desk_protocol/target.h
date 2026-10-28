#pragma once
#include "protocol.h"

namespace desk_protocol {
// Metric T150 profile only, using recovered A=24, B=39. The adapter reads both
// coefficients before every move. This predicts a setpoint, not stop accuracy.
inline bool metric_target_raw(uint16_t tenths_cm, uint16_t &raw) {
  if (tenths_cm < 730 || tenths_cm > 1180) return false;
  if (tenths_cm == 730) { raw = 8; return true; }
  const uint16_t q = tenths_cm - 725;
  raw = static_cast<uint16_t>((static_cast<uint32_t>(q) * 39 + 23) / 24);
  return raw >= 8 && raw <= 740;
}
inline bool read_target_parameter(uint8_t selector, Frame &out) {
  out = Frame{};
  if (selector != 0x0B && selector != 0x0C && selector != 0x1F && selector != 0x17) return false;
  out.size = 4; out.bytes[0] = 0x15; out.bytes[1] = selector;
  return true;
}
inline bool target_parameter(const Frame &f, uint8_t selector, uint16_t &value) {
  if (f.size != 6 || f.bytes[0] != 0x16 || f.bytes[1] != selector) return false;
  value = be16(f.bytes + 2); return true;
}

// No transport, storage, platform or clock dependency. Caller supplies fresh,
// deduplicated height samples and durably saves the journal before commit().
// Exactly one transaction, no movement retries and no EEPROM-save command.
class TemporaryPreset {
 public:
  enum class Phase { Idle, CurrentRaw, CheckRaw, ScaleA, ScaleB, Original, Backup, Write, Verify,
                     Recall, Moving, Settle, RestoreRead, RestoreWrite,
                     RestoreVerify, ClearJournal, Fault, RecoveryRead, AcceptRead, AcceptVerify, Monitoring, RestoreCheck, RestoreConfirm };
  bool begin(uint16_t tenths, uint32_t now) {
    uint16_t raw;
    if (busy() || !metric_target_raw(tenths, raw)) return false;
    keep_awake_ = false; target_ = raw; failed_ = false; changed_ = false; error_ = "";
    early_restore_ = watching_move_ = motion_seen_ = false;
    read_(0x0B, Phase::ScaleA, now); return true;
  }
  // 15/17 reads the integer position counter. Never write that selector.
  bool begin_keep_awake(uint32_t now) {
    if (busy()) return false;
    keep_awake_ = true; failed_ = false; changed_ = false; error_ = "";
    early_restore_ = watching_move_ = motion_seen_ = false;
    read_(0x17, Phase::CurrentRaw, now); return true;
  }
  bool keep_awake() const { return keep_awake_; }
  void abort_keep_awake(uint32_t now, bool brake) {
    if (!busy() || failed_) return;
    brake_ = brake; changed_ = true;
    fail_("Keep-awake interrupted by height change or stale sample", now);
  }
  // Yield to a user button without treating cancellation as a fault. Let an
  // already-sent recall and restoration finish normally; before recall, drain
  // outstanding parameter replies during Settle before restoring the backup.
  bool yield_keep_awake(uint32_t now) {
    if (!keep_awake_ || !busy() || failed_) return false;
    if (phase_ == Phase::Moving || phase_ == Phase::RestoreRead ||
        phase_ == Phase::RestoreWrite || phase_ == Phase::RestoreVerify ||
        phase_ == Phase::ClearJournal || phase_ == Phase::Settle) return true;
    queued_ = false;
    phase_ = dirty_ ? Phase::Settle : Phase::Idle;
    started_ = now;
    return true;
  }
  // The keep-awake cycle failed because height or position changed, or a
  // sample was late, not because the controller rejected or lost a request.
  // Whether the preset press was already sent is known only to the caller.
  bool interrupted_by_change() const { return changed_; }
  bool busy() const { return phase_ != Phase::Idle; }
  bool dirty() const { return dirty_; }
  bool failed() const { return failed_; }
  const char *error() const { return error_; }
  Phase phase() const { return phase_; }
  uint16_t original() const { return original_; }
  uint16_t target() const { return target_; }
  bool take_frame(Frame &f) {
    if (!queued_) return false;
    f = output_; queued_ = false; return true;
  }
  bool brake_pending() const { return brake_; }
  bool take_brake() { bool b = brake_; brake_ = false; return b; }
  // This handshake is deliberately outside frame processing: flash must be
  // committed before the first UART write, including on ESP power failure.
  void commit(bool stored, uint32_t now) {
    if (phase_ != Phase::Backup) return;
    if (!stored) { fail_("Cannot persist preset backup", now); return; }
    dirty_ = true; write_(target_, Phase::Write, now);
  }
  void journal_cleared(bool stored) {
    if (phase_ != Phase::ClearJournal) return;
    if (stored) {
      dirty_ = false;
      phase_ = early_restore_ && watching_move_ ? Phase::Monitoring : Phase::Idle;
      if (phase_ == Phase::Idle) watching_move_ = false;
    } else {
      if (early_restore_ && watching_move_) brake_ = true;
      watching_move_ = false;
      failed_ = true; error_ = "Cannot clear preset backup"; phase_ = Phase::Fault;
    }
  }
  void recover(uint16_t original, uint16_t temporary,
               const char *reason = "Interrupted preset transaction; restore required") {
    original_ = original; target_ = temporary; dirty_ = true;
    queued_ = false; early_restore_ = watching_move_ = motion_seen_ = false;
    failed_ = true; error_ = reason;
    phase_ = Phase::Fault;
  }
  bool retry_restore(uint32_t now) {
    if (phase_ != Phase::Fault || !dirty_) return false;
    failed_ = false; error_ = "";
    phase_ = Phase::Settle; started_ = now; return true;
  }
  // Explicit user recovery can replace an older temporary or a local value,
  // but only after two stable, distinct replies. Normal cleanup stays strict.
  bool restore_backup(uint32_t now) {
    if (phase_ != Phase::Fault || !dirty_) return false;
    failed_ = false; error_ = "";
    read_(0x1F, Phase::RestoreCheck, now); return true;
  }
  // A retained automatic journal can describe a completed cycle. On boot a
  // matching temporary value might be a newer local edit, not an interruption.
  // Inspect only; any non-original value requires an explicit user decision.
  bool check_recovery(uint32_t now) {
    if (phase_ != Phase::Fault || !dirty_) return false;
    failed_ = false; error_ = "";
    read_(0x1F, Phase::RecoveryRead, now); return true;
  }
  // Explicitly keep the controller's current preset and discard our backup.
  // Two reads must agree before the caller can durably clear the journal.
  bool accept_current(uint32_t now) {
    if (phase_ != Phase::Fault || !dirty_) return false;
    failed_ = false; error_ = "";
    read_(0x1F, Phase::AcceptRead, now); return true;
  }
  bool inspecting_recovery() const {
    return phase_ == Phase::RecoveryRead || phase_ == Phase::AcceptRead || phase_ == Phase::AcceptVerify ||
           phase_ == Phase::RestoreCheck || phase_ == Phase::RestoreConfirm;
  }
  void abort_recovery(uint32_t now) {
    if (!inspecting_recovery() && phase_ != Phase::ClearJournal &&
        phase_ != Phase::RestoreWrite && phase_ != Phase::RestoreVerify) return;
    failed_ = true; error_ = "Preset recovery interrupted; backup retained";
    queued_ = false; phase_ = Phase::Fault; started_ = now;
  }
  void recalled(uint32_t now, uint16_t baseline_q = 0xFFFF) {
    if (phase_ != Phase::Recall) return;
    phase_ = Phase::Moving; started_ = now;
    move_started_ = now; baseline_q_ = baseline_q; watching_move_ = true;
  }
  // Caller forwards only accepted, fresh, deduplicated height samples. A
  // command ACK is not movement. Restore on the first directional Q change.
  void observe_height(uint16_t q) {
    if (phase_ != Phase::Moving || keep_awake_ || baseline_q_ == 0xFFFF) return;
    const uint16_t goal_q = static_cast<uint32_t>(target_) * 24 / 39;
    if ((goal_q > baseline_q_ && q > baseline_q_) || (goal_q < baseline_q_ && q < baseline_q_))
      motion_seen_ = true;
  }
  // Called by the transport's stop path, before its brake goes onto UART.
  void cancel(uint32_t now) {
    if (!busy()) return;
    watching_move_ = false;
    queued_ = false;
    if (!failed_) { failed_ = true; error_ = "Target move interrupted"; }
    if (phase_ == Phase::ClearJournal || phase_ == Phase::Fault) return;
    if (inspecting_recovery()) { phase_ = Phase::Fault; return; }
    phase_ = dirty_ ? Phase::Settle : Phase::Idle; started_ = now;
  }
  // A lost controller session makes queued replies and automatic restoration
  // ambiguous. Preserve the backup and request braking if motion was monitored.
  void connection_lost(uint32_t now) {
    if (!busy()) return;
    const bool moving = watching_move_;
    cancel(now);
    queued_ = false;
    failed_ = true; error_ = "Controller connection lost; preset backup retained";
    phase_ = dirty_ ? Phase::Fault : Phase::Idle;
    brake_ = brake_ || moving;
  }
  void accept(const Frame &f, uint32_t now) {
    if (waiting_() && elapsed_(now) >= 700) return;
    uint16_t v;
    Ack ack;
    switch (phase_) {
      case Phase::CurrentRaw:
        if (!target_parameter(f, 0x17, v)) return;
        // Conservative supported profile range, including its raw lower edge.
        if (v < 6 || v > 740) { fail_("Keep-awake raw height outside supported range", now); return; }
        target_ = v; read_(0x1F, Phase::Original, now); break;
      case Phase::CheckRaw:
        if (!target_parameter(f, 0x17, v)) return;
        if (v != target_) { changed_ = true; fail_("Keep-awake raw height changed before recall", now); return; }
        phase_ = Phase::Recall; break;
      case Phase::ScaleA:
        if (!target_parameter(f, 0x0B, v)) return;
        if (v != 24) { fail_("Unsupported height scale A", now); return; }
        read_(0x0C, Phase::ScaleB, now); break;
      case Phase::ScaleB:
        if (!target_parameter(f, 0x0C, v)) return;
        if (v != 39) { fail_("Unsupported height scale B", now); return; }
        read_(0x1F, Phase::Original, now); break;
      case Phase::Original:
        if (!target_parameter(f, 0x1F, v)) return;
        original_ = v; phase_ = Phase::Backup; break;
      case Phase::Write:
      case Phase::RestoreWrite: {
        if (!decode_ack(f, ack) || ack.kind != 0x1A || ack.first != 0x1F) return;
        if (ack.second != 0) { fail_("Preset write rejected", now); return; }
        read_(0x1F, phase_ == Phase::Write ? Phase::Verify : Phase::RestoreVerify, now); break;
      }
      case Phase::Verify:
        if (!target_parameter(f, 0x1F, v)) return;
        if (v != target_) { fail_("Temporary preset readback mismatch", now); return; }
        if (keep_awake_) read_(0x17, Phase::CheckRaw, now);
        else phase_ = Phase::Recall;
        break;
      case Phase::RestoreRead:
        if (!target_parameter(f, 0x1F, v)) return;
        if (v == original_) { phase_ = Phase::ClearJournal; break; }
        if (v != target_) { fail_("Preset changed locally; refusing overwrite", now); return; }
        write_(original_, Phase::RestoreWrite, now); break;
      case Phase::RestoreVerify:
        if (!target_parameter(f, 0x1F, v)) return;
        if (v != original_) { fail_("Preset restoration readback mismatch", now); return; }
        phase_ = Phase::ClearJournal; break;
      case Phase::RecoveryRead:
        if (!target_parameter(f, 0x1F, v)) return;
        if (v != original_) { fail_("Preset recovery needs a decision: restore backup or accept current preset", now); return; }
        phase_ = Phase::ClearJournal; break;
      case Phase::RestoreCheck:
      case Phase::AcceptRead:
        if (!target_parameter(f, 0x1F, v)) return;
        accepted_raw_ = v; accepted_sequence_ = be16(f.bytes + 4);
        read_(0x1F, phase_ == Phase::RestoreCheck ? Phase::RestoreConfirm : Phase::AcceptVerify, now); break;
      case Phase::RestoreConfirm:
      case Phase::AcceptVerify:
        if (!target_parameter(f, 0x1F, v)) return;
        if (be16(f.bytes + 4) == accepted_sequence_) return;
        if (v != accepted_raw_) { fail_("Preset changed during recovery; backup retained", now); return; }
        if (phase_ == Phase::RestoreConfirm && v != original_) write_(original_, Phase::RestoreWrite, now);
        else phase_ = Phase::ClearJournal;
        break;
      default: break;
    }
  }
  // stable_ms is based on unchanged raw Q, never the whole-cm display above 1 m.
  // It is a settling heuristic, not proof of standstill or target capture.
  void tick(uint32_t now, bool fresh, uint32_t stable_ms, bool released) {
    if (inspecting_recovery() && (!fresh || stable_ms < 1500 || !released)) {
      fail_("Preset recovery interrupted; backup retained", now); return;
    }
    if (!fresh && (phase_ == Phase::CurrentRaw || phase_ == Phase::CheckRaw || phase_ == Phase::ScaleA || phase_ == Phase::ScaleB || phase_ == Phase::Original || phase_ == Phase::Backup || phase_ == Phase::Write || phase_ == Phase::Verify)) {
      fail_("Telemetry lost before recall", now); return;
    }
    if (waiting_() && elapsed_(now) >= 700) { fail_("Preset transaction timeout", now); return; }
    if ((phase_ == Phase::Recall || watching_move_) && !fresh) {
      brake_ = true; fail_("Height telemetry lost during target move", now); return;
    }
    if (watching_move_ && static_cast<uint32_t>(now - move_started_) >= 60000) {
      brake_ = true; fail_("Target move timed out", now); return;
    }
    if (phase_ == Phase::Moving && fresh && released && motion_seen_ && !keep_awake_) {
      early_restore_ = true;
      read_(0x1F, Phase::RestoreRead, now);
    } else if (phase_ == Phase::Monitoring && fresh && released &&
               static_cast<uint32_t>(now - move_started_) >= 3000 && stable_ms >= 1500) {
      watching_move_ = false; phase_ = Phase::Idle;
    } else if (phase_ == Phase::Moving && fresh && released && elapsed_(now) >= 3000 && stable_ms >= 1500) {
      read_(0x1F, Phase::RestoreRead, now);
    } else if (phase_ == Phase::Settle && fresh && released && elapsed_(now) >= 2000 && stable_ms >= 1500) {
      read_(0x1F, Phase::RestoreRead, now);
    }
  }
 private:
  uint32_t elapsed_(uint32_t now) const { return static_cast<uint32_t>(now - started_); }
  bool waiting_() const {
    return phase_ == Phase::CurrentRaw || phase_ == Phase::CheckRaw || phase_ == Phase::ScaleA || phase_ == Phase::ScaleB || phase_ == Phase::Original ||
           phase_ == Phase::Write || phase_ == Phase::Verify || phase_ == Phase::RestoreRead ||
           phase_ == Phase::RestoreWrite || phase_ == Phase::RestoreVerify || inspecting_recovery();
  }
  void read_(uint8_t selector, Phase next, uint32_t now) {
    read_target_parameter(selector, output_); queued_ = true; phase_ = next; started_ = now;
  }
  void write_(uint16_t value, Phase next, uint32_t now) {
    write_preset_raw(3, value, output_); queued_ = true; phase_ = next; started_ = now;
  }
  void fail_(const char *message, uint32_t now) {
    if (early_restore_ && watching_move_) brake_ = true;
    watching_move_ = false;
    failed_ = true; error_ = message; queued_ = false;
    const bool restoring = phase_ == Phase::RestoreRead || phase_ == Phase::RestoreWrite || phase_ == Phase::RestoreVerify || inspecting_recovery();
    phase_ = !dirty_ ? Phase::Idle : restoring ? Phase::Fault : Phase::Settle;
    started_ = now;
  }
  Phase phase_{Phase::Idle};
  Frame output_{};
  uint16_t original_{0}, target_{0}, accepted_raw_{0}, accepted_sequence_{0}, baseline_q_{0xFFFF};
  uint32_t started_{0}, move_started_{0};
  bool early_restore_{false}, watching_move_{false}, motion_seen_{false};
  bool keep_awake_{false}, changed_{false};
  bool queued_{false}, dirty_{false}, failed_{false}, brake_{false};
  const char *error_{""};
};
}  // namespace desk_protocol
