#pragma once

#include "esphome/components/desk_uart/desk_uart.h"
#include <cstdlib>

// Deterministic adapter edge cases alongside the timed PTY scenarios. No UART
// is attached: frames are exchanged directly with the portable transaction.
namespace keep_awake_test {
class Probe : public esphome::desk_uart::DeskUART {
 public:
  void loop() override {
    DeskUART::loop();
    const std::string scenario(getenv("SCENARIO"));
    if (scenario.find("handoff_") != 0) return;
    using Phase = desk_protocol::TemporaryPreset::Phase;
    Phase trigger = Phase::Write;
    if (scenario == "handoff_read") trigger = Phase::CurrentRaw;
    if (scenario == "handoff_moving" || scenario == "handoff_release_failure" ||
        scenario == "handoff_deadline" || scenario == "handoff_latest_deadline") trigger = Phase::Moving;
    if (scenario == "handoff_restoring") trigger = Phase::RestoreVerify;
    if (!requested_ && this->keep_awake_active_ && this->target_move_.phase() == trigger) {
      requested_ = true;
      requested_at_ = esphome::millis();
      ESP_LOGI("fixture", "HANDOFF_REQUEST=%d", this->recall_preset(scenario == "handoff_slot3" ? 3 : 1));
      if (scenario == "handoff_latest") this->recall_preset(2);
      if (scenario == "handoff_stop") this->stop();
    }
    if (scenario == "handoff_latest_deadline" && requested_ && !replaced_ &&
        static_cast<uint32_t>(esphome::millis() - requested_at_) >= 2500) {
      replaced_ = true;
      ESP_LOGI("fixture", "HANDOFF_REPLACE=%d", this->recall_preset(2));
    }
  }
  static void gap_cancels_recovery() {
    using Phase = desk_protocol::TemporaryPreset::Phase;
    for (bool restoring : {false, true}) {
      Probe p;
      p.cached_journal_valid_ = true;
      p.cached_journal_ = {0x44534B33, 456, 122};
      auto &t = p.target_move_;
      t.recover(456, 122);
      require(restoring ? t.restore_backup(1000) : t.accept_current(1000), "begin recovery before gap");
      p.restore_requested_ = restoring;
      p.accept_current_active_ = !restoring;
      desk_protocol::Frame f;
      require(t.take_frame(f), "first recovery read");
      f = value(0x1F, 555); f.bytes[5] = 1;
      t.accept(f, 1001);
      require(t.take_frame(f), "second recovery read");
      if (restoring) {
        f = value(0x1F, 555); f.bytes[5] = 2;
        t.accept(f, 1002); // Leave the resulting RAM write queued.
        require(t.phase() == Phase::RestoreWrite, "queued restore before gap");
      }
      p.invalidate_journal_session_();
      require(p.journal_recheck_pending_ && p.cached_journal_valid_, "gap retains backup");
      require(!p.accept_current_active_ && !p.restore_requested_, "gap cancels explicit intent");
      require(!t.take_frame(f) && t.phase() == Phase::Fault, "gap drops queued UART work");
      // A later original-valued automatic check must not inherit Accept's
      // permission to clear persistent storage.
      require(t.check_recovery(2000), "automatic read-only recheck");
      require(t.take_frame(f), "automatic read");
      t.accept(value(0x1F, 456), 2001);
      require(t.phase() == Phase::ClearJournal, "matching RAM check");
      require(p.save_journal_(false) && p.cached_journal_valid_, "recheck cannot clear backup");
      t.journal_cleared(true);
    }
    Probe p;
    p.cached_journal_valid_ = true;
    p.cached_journal_ = {0x44534B33, 456, 122};
    auto &t = p.target_move_;
    t.recover(456, 122); require(t.retry_restore(0), "prepare fallback restore");
    t.tick(2000, true, 2000, true);
    p.invalidate_journal_session_();
    desk_protocol::Frame f;
    require(!t.take_frame(f), "gap drops pending restore read");
    ESP_LOGI("fixture", "GAP_CANCELS_RECOVERY=passed");
  }
  static void pending_deadline() {
    Probe p;
    p.motion_enabled_ = true;
    p.pending_preset_ = 1; p.pending_preset_started_ = 1000;
    auto f = desk_protocol::Frame{};
    f.size = 32; f.bytes[0] = 5; f.bytes[20] = 6;
    require(p.monitor_.observe(f, 4999), "deadline height");
    p.last_height_received_ = 4999;
    p.pending_preset_q_ = p.last_q_ = 6;
    p.target_move_.begin_keep_awake(1000);
    p.pending_preset_loop_(4999);
    require(p.pending_preset_ == 1, "request valid before deadline");
    p.pending_preset_loop_(5000);
    require(!p.pending_preset_, "deadline cancels request");
    p.target_move_.yield_keep_awake(5001);
    p.pending_preset_loop_(5001); // Must not send even after cleanup completes.
    require(!p.pulse_.active() && !p.pending_preset_, "expired button must not replay");
    ESP_LOGI("fixture", "PENDING_DEADLINE=passed");
  }
  static void yielded_stale_final_readback() {
    Probe p;
    const uint32_t base = esphome::millis();
    p.target_enabled_ = p.keep_awake_active_ = p.motion_enabled_ = true;
    auto &t = p.target_move_;
    desk_protocol::Frame f;
    auto take = [&]() { require(t.take_frame(f), "expected yield frame"); };
    require(t.begin_keep_awake(base), "yield begin"); take();
    t.accept(value(0x17, 123), base); take();
    t.accept(value(0x1F, 456), base);
    t.commit(true, base); take();
    f = {}; f.size = 32; f.bytes[0] = 5; f.bytes[20] = 6;
    require(p.monitor_.observe(f, base), "yield height");
    p.last_height_received_ = base; p.last_q_ = 6;
    require(p.recall_preset(1), "user yield accepted");
    require(p.soft_abort_error_, "yield must track restoration freshness");
    p.cached_journal_valid_ = true;
    p.cached_journal_ = {0x44534B32, 456, 123};
    t.tick(base + 2590, true, 2000, true); take();
    t.accept(value(0x1F, 123), base + 2591); take();
    f = {}; f.size = 5; f.bytes[0] = 0x1A; f.bytes[1] = 0x1F;
    t.accept(f, base + 2592); take();
    t.accept(value(0x1F, 456), base + 3001);
    require(!p.monitor_.fresh(base + 3001), "yield stale final height");
    p.target_loop_(base + 3001);
    p.pending_preset_loop_(base + 3001);
    require(p.keep_awake_fault_ && !p.verified_journal_.valid, "stale yield cannot authorize journal deletion");
    require(!p.pending_preset_ && !p.pulse_.active(), "stale yield cannot replay button");
    ESP_LOGI("fixture", "YIELDED_STALE_FINAL_READBACK=passed");
  }
  static void stale_final_readback() {
    Probe p;
    p.target_enabled_ = p.keep_awake_active_ = true;
    auto &t = p.target_move_;
    desk_protocol::Frame frame;
    auto take = [&]() { require(t.take_frame(frame), "expected transaction frame"); };
    require(t.begin_keep_awake(1000), "begin"); take();
    t.accept(value(0x17, 123), 1001); take();
    t.accept(value(0x1F, 456), 1002);
    t.commit(true, 1003); take();
    t.abort_keep_awake(1004, false);
    p.soft_abort_error_ = p.last_target_error_ = t.error();
    p.cached_journal_valid_ = true;
    p.cached_journal_ = {0x44534B32, 456, 123};

    frame = {};
    frame.size = 32; frame.bytes[0] = 5; frame.bytes[20] = 6;
    require(p.monitor_.observe(frame, 1000), "height sample");
    t.tick(3590, true, 2000, true); take();
    t.accept(value(0x1F, 123), 3591); take();
    frame = {}; frame.size = 5; frame.bytes[0] = 0x1A; frame.bytes[1] = 0x1F;
    t.accept(frame, 3592); take();
    // The final parameter reply is still within its 700 ms timeout, but the
    // height sample expired before this RX drain. It must not become a healthy
    // automatic retry just because ClearJournal finishes in the same loop.
    t.accept(value(0x1F, 456), 4001);
    require(!p.monitor_.fresh(4001), "stale height");
    p.target_loop_(4001);
    require(p.keep_awake_fault_, "stale final readback must latch a fault");
    require(!p.soft_abort_error_ && !t.busy() && !t.dirty(), "verified cleanup");
    require(!p.verified_journal_.valid, "fault must not authorize stale-journal deletion");
    ESP_LOGI("fixture", "STALE_FINAL_READBACK=passed");
  }
 private:
  bool requested_{false}, replaced_{false};
  uint32_t requested_at_{0};
  static desk_protocol::Frame value(uint8_t selector, uint16_t raw) {
    desk_protocol::Frame f;
    f.size = 6; f.bytes[0] = 0x16; f.bytes[1] = selector;
    f.bytes[2] = raw >> 8; f.bytes[3] = raw & 255;
    return f;
  }
  static void require(bool condition, const char *message) {
    if (!condition) { ESP_LOGE("fixture", "%s", message); std::exit(1); }
  }
};
}  // namespace keep_awake_test
