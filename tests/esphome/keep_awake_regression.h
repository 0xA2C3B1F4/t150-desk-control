#pragma once

#include "esphome/components/desk_uart/desk_uart.h"
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <functional>
#include <vector>

// Deterministic adapter edge cases alongside the timed PTY scenarios. No UART
// is attached: frames are exchanged directly with the portable transaction.
namespace keep_awake_test {
class Wire : public esphome::uart::UARTComponent {
 public:
  std::function<void()> before_empty;
  unsigned reads{0};
  std::vector<desk_protocol::Frame> sent;
  void check_logger_conflict() override {}
  void write_array(const uint8_t *data, size_t size) override {
    desk_protocol::Frame frame;
    for (size_t i = 0; i < size; ++i)
      if (parser_.feed(data[i], esphome::millis(), frame) == desk_protocol::ParseResult::FrameReady)
        sent.push_back(frame);
  }
  bool peek_byte(uint8_t *) override { ++reads; return false; }
  bool read_array(uint8_t *, size_t) override { ++reads; return false; }
  size_t available() override {
    if (before_empty) { auto callback = before_empty; before_empty = {}; callback(); }
    return 0;
  }
  esphome::uart::UARTFlushResult flush() override {
    return esphome::uart::UARTFlushResult::UART_FLUSH_RESULT_SUCCESS;
  }
 private:
  desk_protocol::Parser parser_{};
};
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
  static void empty_rx_crosses_freshness_boundary() {
    // Cover configurations with and without a preset number. No incoming byte
    // runs the RX-body freshness check, and the initial loop check is fresh.
    for (bool has_number : {false, true}) {
      Probe p;
      Wire wire;
      esphome::sensor::Sensor height;
      esphome::desk_uart::PresetHeightNumber preset;
      p.set_uart_parent(&wire); p.set_height_sensor(&height);
      if (has_number) p.set_preset_height_number(3, &preset);
      p.target_enabled_ = true;
      p.cached_journal_valid_ = true;
      p.cached_journal_ = {0x44534B33, 456, 122};
      const uint32_t now = esphome::millis();
      const uint32_t sample_at = now - 2800;
      auto h = desk_protocol::Frame{};
      h.size = 32; h.bytes[0] = 5; h.bytes[20] = 75;
      require(p.monitor_.observe(h, sample_at), "boundary initial sample");
      p.height_seen_ = p.connected_ = true;
      p.last_height_received_ = sample_at; p.last_height_change_ = sample_at - 2000;
      p.last_subscribe_ = now;
      p.target_move_.recover(456, 122);
      require(p.target_move_.accept_current(now), "boundary explicit accept");
      p.accept_current_active_ = true;
      desk_protocol::Frame f;
      require(p.target_move_.take_frame(f), "boundary initial read already sent");
      bool invalidated_before_publish = false;
      height.add_on_raw_state_callback([&](float state) {
        if (!std::isnan(state)) return;
        // loop clears height_seen immediately before publishing disconnection.
        invalidated_before_publish = p.journal_recheck_pending_ && !p.accept_current_active_;
      });
      wire.before_empty = [&]() {
        require(p.monitor_.fresh(esphome::millis()), "sample must be fresh at zero-RX entry");
        while (p.monitor_.fresh(esphome::millis())) esphome::delay(1);
      };
      p.DeskUART::loop();
      require(wire.reads == 0, "boundary test must receive zero UART bytes");
      require(!p.height_seen_ && !p.connected_, "boundary marks disconnected");
      require(invalidated_before_publish, "late timestamp must invalidate before height state is discarded");
      require(p.journal_recheck_pending_ && !p.accept_current_active_, "late timestamp cancels accept intent");
      require(p.cached_journal_valid_ && p.target_move_.dirty(), "boundary retains original backup");
      require(wire.sent.empty(), "zero-RX gap cannot send a stale recovery command");
    }
    ESP_LOGI("fixture", "EMPTY_RX_BOUNDARY=passed");
  }
  static void stop_cancels_confirmed_restore() {
    using Phase = desk_protocol::TemporaryPreset::Phase;
    for (bool verify : {false, true}) {
      Probe p;
      Wire wire;
      esphome::sensor::Sensor height;
      p.set_uart_parent(&wire); p.set_height_sensor(&height);
      p.target_enabled_ = true;
      p.cached_journal_valid_ = true;
      p.cached_journal_ = {0x44534B33, 456, 122};
      const uint32_t now = esphome::millis();
      auto h = desk_protocol::Frame{};
      h.size = 32; h.bytes[0] = 5; h.bytes[20] = 75;
      require(p.monitor_.observe(h, now), "stop restore height");
      p.height_seen_ = p.connected_ = true;
      p.last_height_received_ = now; p.last_height_change_ = now - 2000;
      p.last_preset_recall_ = now - 4000;
      auto &t = p.target_move_;
      t.recover(456, 122);
      require(p.restore_temporary_preset(), "explicit restore accepted");
      desk_protocol::Frame f;
      require(t.take_frame(f), "restore first read");
      f = value(0x1F, 122); f.bytes[5] = 1; t.accept(f, now);
      require(t.take_frame(f), "restore second read");
      f = value(0x1F, 122); f.bytes[5] = 2; t.accept(f, now);
      require(t.phase() == Phase::RestoreWrite, "confirmed restore has queued write");
      if (verify) {
        require(t.take_frame(f), "write sent before stop");
        desk_protocol::Packet packet;
        require(desk_protocol::encode(f, packet), "encode restore write");
        wire.write_array(packet.bytes, packet.size);
        f = {}; f.size = 5; f.bytes[0] = 0x1A; f.bytes[1] = 0x1F;
        t.accept(f, now);
        require(t.phase() == Phase::RestoreVerify, "restore awaiting readback");
      }
      const size_t sent_before = wire.sent.size();
      require(p.stop(), "stop accepted during explicit restore");
      require(t.dirty(), "stop retains unresolved backup");
      // Deliver late ACK/readback and pass the old two-second cleanup delay.
      f = {}; f.size = 5; f.bytes[0] = 0x1A; f.bytes[1] = 0x1F; t.accept(f, now + 1);
      t.accept(value(0x1F, 122), now + 2);
      f = {}; f.size = 6; f.bytes[0] = 0x18; f.bytes[1] = 1;
      require(p.stop_ack_.accept(f, now + 3), "brake ACK");
      p.stop_acknowledged_ = true;
      h.bytes[31] = 1;
      require(p.monitor_.observe(h, now + 2500), "fresh height after brake settles");
      p.last_height_received_ = now + 2500;
      p.target_loop_(now + 2500);
      // If Stop incorrectly schedules ordinary cleanup, this reply matches
      // its temporary value and would authorize a delayed write of original.
      t.accept(value(0x1F, 122), now + 2501);
      p.target_loop_(now + 2502);
      t.accept(value(0x1F, 456), now + 2503);
      p.target_loop_(now + 2600);
      require(p.cached_journal_valid_ && t.dirty() && t.phase() == Phase::Fault, "stop retains backup pending decision");
      require(!p.restore_requested_ && !t.take_frame(f), "stop leaves no delayed restore frame");
      require(wire.sent.size() == sent_before + 1, "stop sends only one additional frame");
      const auto &brake = wire.sent.back();
      require(brake.size == 4 && brake.bytes[0] == 0x17 && brake.bytes[1] == 1 &&
              brake.bytes[2] == 0 && brake.bytes[3] == 0, "additional frame is brake, not preset recall or write");
    }
    ESP_LOGI("fixture", "STOP_CONFIRMED_RESTORE=passed");
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
