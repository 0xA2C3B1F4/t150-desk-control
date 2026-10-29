#pragma once

#include "esphome/components/desk_uart/desk_uart.h"
#include "desk_protocol/preset_journal.h"
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

namespace profile_journal_test {
using Journal = desk_protocol::PresetJournal;
using Load = esphome::desk_uart::JournalLoad;

inline void require(bool value, const char *message) {
  if (!value) {
    ESP_LOGE("fixture", "Profile journal assertion failed: %s", message);
    std::abort();
  }
}

struct Disk {
  std::map<uint32_t, std::vector<uint8_t>> records;
  std::map<uint32_t, Load> faults;
  std::vector<uint32_t> writes;
  uint32_t fail_key{0};
  bool persist_before_failure{false};
};

class Wire : public esphome::uart::UARTComponent {
 public:
  std::vector<desk_protocol::Frame> sent;
  void check_logger_conflict() override {}
  void write_array(const uint8_t *data, size_t length) override {
    desk_protocol::Frame frame;
    for (size_t i = 0; i < length; ++i)
      if (parser_.feed(data[i], esphome::millis(), frame) == desk_protocol::ParseResult::FrameReady)
        sent.push_back(frame);
  }
  bool peek_byte(uint8_t *out) override {
    if (incoming_.empty()) return false;
    *out = incoming_.front(); return true;
  }
  bool read_array(uint8_t *out, size_t length) override {
    if (incoming_.size() < length) return false;
    std::memcpy(out, incoming_.data(), length);
    incoming_.erase(incoming_.begin(), incoming_.begin() + length);
    return true;
  }
  size_t available() override { return incoming_.size(); }
  void receive(const desk_protocol::Frame &frame) {
    desk_protocol::Packet packet;
    require(desk_protocol::encode(frame, packet), "encode synthetic incoming frame");
    incoming_.insert(incoming_.end(), packet.bytes, packet.bytes + packet.size);
  }
  esphome::uart::UARTFlushResult flush() override {
    return esphome::uart::UARTFlushResult::UART_FLUSH_RESULT_SUCCESS;
  }
 private:
  desk_protocol::Parser parser_{};
  std::vector<uint8_t> incoming_;
};

class Probe : public esphome::desk_uart::DeskUART {
 public:
  static constexpr uint32_t LEGACY = 1001, CURRENT = 1004;
  explicit Probe(Disk &disk) : disk_(disk) {
    this->preference_key_ = LEGACY;
    this->v4_preference_key_ = CURRENT;
    this->motion_enabled_ = this->target_enabled_ = this->keep_awake_enabled_ = true;
    this->set_uart_parent(&wire_);
    this->set_height_sensor(&height_);
    this->set_command_result(&result_);
    for (uint8_t slot = 1; slot <= 3; ++slot) {
      this->set_preset_height_number(slot, &presets_[slot - 1]);
      this->preset_known_[slot - 1] = true;
    }
    fresh();
  }

  static void run() {
    empty(); legacy_versions(); malformed(); read_errors(); identity_mismatches();
    conflicts(); migration_failures(); acceptance_failures(); resolved_legacy_round_trip();
    resolved_no_legacy(); resolved_legacy_errors(); resolved_migration_failures();
    legacy_clear_failures(); accept_unwritable_legacy(); resolved_recovery_actions(); matching_original_recheck();
    setup_pending(); additional_controls(); borrow_failure(); configuration_identity();
    ESP_LOGI("fixture", "PROFILE_JOURNAL_ALL=passed");
  }

 protected:
  Load read_journal_storage_(uint32_t key, uint8_t *out, size_t length) override {
    const auto fault = disk_.faults.find(key);
    if (fault != disk_.faults.end()) return fault->second;
    const auto it = disk_.records.find(key);
    if (it == disk_.records.end()) return Load::Missing;
    if (it->second.size() != length) return Load::Invalid;
    std::memcpy(out, it->second.data(), length);
    return Load::Found;
  }
  bool write_journal_storage_(uint32_t key, const uint8_t *bytes, size_t length) override {
    disk_.writes.push_back(key);
    // Match the production helper: a malformed or unreadable existing key
    // must not become writable merely because this fixture uses a map.
    std::vector<uint8_t> existing(length);
    const auto before = this->read_journal_storage_(key, existing.data(), length);
    if (before != Load::Missing && before != Load::Found) return false;
    if (key != disk_.fail_key || disk_.persist_before_failure)
      disk_.records[key] = std::vector<uint8_t>(bytes, bytes + length);
    return key != disk_.fail_key;
  }

 private:
  Disk &disk_;
  Wire wire_;
  esphome::sensor::Sensor height_;
  esphome::text_sensor::TextSensor result_;
  esphome::desk_uart::PresetHeightNumber presets_[3];

  static void passed(const char *name) { ESP_LOGI("fixture", "PROFILE_JOURNAL_CASE=%s", name); }
  static Journal retained(uint16_t original = 456, uint16_t temporary = 122) {
    Journal j{};
    j.state = Journal::State::Retained;
    j.profile = desk_protocol::HeightProfile::known_t150().identity();
    j.original = original; j.temporary = temporary;
    return j;
  }
  static void put(Disk &disk, const Journal &j) {
    const auto encoded = j.encode();
    disk.records[CURRENT] = {encoded.begin(), encoded.end()};
  }
  static void put_legacy(Disk &disk, uint32_t magic = 0x44534B33, uint16_t original = 456,
                         uint16_t temporary = 122) {
    disk.records[LEGACY] = {uint8_t(magic), uint8_t(magic >> 8), uint8_t(magic >> 16), uint8_t(magic >> 24),
                           uint8_t(original), uint8_t(original >> 8), uint8_t(temporary), uint8_t(temporary >> 8)};
  }
  static Journal persisted(const Disk &disk) {
    const auto it = disk.records.find(CURRENT);
    require(it != disk.records.end(), "v4 record exists");
    Journal result{};
    require(Journal::decode(it->second.data(), it->second.size(), result), "v4 record decodes");
    return result;
  }
  static desk_protocol::Frame value(uint8_t selector, uint16_t raw, uint16_t sequence) {
    desk_protocol::Frame f{};
    f.size = 6; f.bytes[0] = 0x16; f.bytes[1] = selector;
    f.bytes[2] = raw >> 8; f.bytes[3] = raw;
    f.bytes[4] = sequence >> 8; f.bytes[5] = sequence;
    return f;
  }
  void fresh() {
    const auto now = esphome::millis();
    desk_protocol::Frame sample{};
    sample.size = 32; sample.bytes[0] = 5; sample.bytes[20] = 75;
    // A new monitor accepts the sample regardless of the previous sequence.
    this->monitor_ = {};
    require(this->monitor_.observe(sample, now), "fresh synthetic height");
    this->height_seen_ = this->connected_ = true;
    this->last_height_received_ = now;
    this->last_height_change_ = now - 2000;
    this->last_preset_recall_ = now - 4000;
    this->last_q_ = 75;
  }
  void blocked_controls() {
    fresh();
    require(this->journal_error_() != nullptr, "blocked state has a reason");
    require(!this->recall_preset(1), "blocked preset recall");
    require(!this->move_to_height(80), "blocked target move");
    require(!this->step_height(true), "blocked relative move");
    for (uint8_t slot = 1; slot <= 3; ++slot)
      require(!this->set_preset_height(slot, 80), "blocked preset edit");
    require(!this->restore_temporary_preset(), "blocked Restore");
    require(!this->accept_current_preset(), "blocked Accept");
    require(wire_.sent.empty(), "blocked actions send no UART frame");
    this->last_keep_awake_ = esphome::millis() - 31000;
    this->keep_awake_loop_(esphome::millis());
    this->target_loop_(esphome::millis());
    require(wire_.sent.empty(), "blocked automatic borrowing sends no frame");
    desk_protocol::Frame sample{};
    sample.size = 32; sample.bytes[0] = 5; sample.bytes[20] = 76; sample.bytes[31] = 1;
    wire_.receive(sample);
    this->loop();
    require(this->monitor_.fresh(esphome::millis()) && this->monitor_.height().q == 76 &&
            height_.has_state() && height_.state > 80.0f, "passive UART height still updates while blocked");
    require(wire_.sent.empty(), "passive blocked loop sends no controls");
    require(this->stop(), "Stop remains available");
    require(wire_.sent.size() == 1 && wire_.sent[0].bytes[0] == 0x17 &&
            wire_.sent[0].bytes[1] == 1 && wire_.sent[0].bytes[2] == 0, "only brake sent");
    auto ack = value(1, 0, 2); ack.bytes[0] = 0x18;
    wire_.receive(ack); this->loop();
    require(this->stop_acknowledged_, "Stop ACK still processed while blocked");
    fresh(); this->last_stop_ = esphome::millis() - 2000;
    require(this->clear_stop_latch(), "acknowledged settled Stop can complete");
    require(result_.state.find("preset backup still blocks controls") != std::string::npos,
            "Stop completion does not announce controls ready");
    require(this->journal_error_() && !this->recall_preset(1), "Stop completion preserves journal block");
    require(wire_.sent.size() == 1, "Stop completion does not replay movement");
  }
  void accept_twice(uint16_t current = 555) {
    fresh();
    wire_.sent.clear();
    const auto writes_before = disk_.writes;
    const auto now = esphome::millis();
    require(this->accept_current_preset(), "explicit Accept starts");
    this->target_loop_(now);
    require(wire_.sent.size() == 1 && wire_.sent.back().bytes[0] == 0x15, "first acceptance read");
    this->target_move_.accept(value(0x1F, current, 1), now);
    this->target_loop_(now);
    require(wire_.sent.size() == 2 && wire_.sent.back().bytes[0] == 0x15, "second acceptance read");
    require(disk_.writes == writes_before, "one read cannot delete backup");
    this->target_move_.accept(value(0x1F, current, 2), now);
    this->target_loop_(now);
    require(wire_.sent.size() == 2, "Accept writes no UART setting");
  }
  void reach_backup(uint16_t original = 456) {
    const auto now = esphome::millis();
    require(this->target_move_.begin(800, now), "start synthetic target transaction");
    desk_protocol::Frame f;
    for (const auto &reply : {value(0x0B, 24, 1), value(0x0C, 39, 2), value(0x1F, original, 3)}) {
      require(this->target_move_.take_frame(f) && f.bytes[0] == 0x15, "parameter query before backup");
      this->target_move_.accept(reply, now);
    }
    require(this->target_move_.phase() == desk_protocol::TemporaryPreset::Phase::Backup, "backup before write");
  }

  void inspect_recovery(uint16_t current) {
    fresh(); wire_.sent.clear();
    const auto writes_before = disk_.writes;
    const auto now = esphome::millis();
    this->keep_awake_loop_(now);
    this->target_loop_(now);
    require(wire_.sent.size() == 1 && wire_.sent[0].bytes[0] == 0x15,
            "boot recovery reads preset without motion");
    this->target_move_.accept(value(0x1F, current, 1), now);
    this->target_loop_(now);
    require(disk_.writes == writes_before && wire_.sent.size() == 1,
            "automatic recovery cannot clear storage or write UART");
  }
  void restore_twice(uint16_t current) {
    fresh(); wire_.sent.clear();
    const auto records_before = disk_.records;
    const auto writes_before = disk_.writes;
    const auto now = esphome::millis();
    require(this->restore_temporary_preset(), "explicit Restore starts");
    this->target_loop_(now);
    require(wire_.sent.size() == 1 && wire_.sent[0].bytes[0] == 0x15, "first restoration read");
    this->target_move_.accept(value(0x1F, current, 11), now);
    this->target_loop_(now);
    require(wire_.sent.size() == 2 && wire_.sent[1].bytes[0] == 0x15,
            "one read cannot authorize restoration");
    this->target_move_.accept(value(0x1F, current, 12), now);
    this->target_loop_(now);
    require(wire_.sent.size() == 3 && wire_.sent[2].bytes[0] == 0x19 &&
            wire_.sent[2].bytes[1] == 0x1F &&
            desk_protocol::be16(wire_.sent[2].bytes + 2) == cached_journal_.original,
            "confirmed Restore writes only the retained original to RAM");
    desk_protocol::Frame ack{};
    ack.size = 5; ack.bytes[0] = 0x1A; ack.bytes[1] = 0x1F;
    this->target_move_.accept(ack, now); this->target_loop_(now);
    require(wire_.sent.size() == 4 && wire_.sent[3].bytes[0] == 0x15, "restoration reads RAM back");
    this->target_move_.accept(value(0x1F, cached_journal_.original, 14), now);
    this->target_loop_(now);
    require(!this->target_move_.dirty() && this->cached_journal_valid_, "RAM restoration retains backup");
    require(disk_.records == records_before && disk_.writes == writes_before,
            "Restore never deletes either backup");
    for (const auto &f : wire_.sent)
      require(f.bytes[0] != 0x17 && f.bytes[0] != 0x1B, "Restore sends neither recall nor EEPROM save");
  }

  static void empty() {
    Disk d; Probe p(d); p.load_journal_();
    require(!p.cached_journal_valid_ && !p.journal_error_(), "missing storage starts without backup");
    require(d.writes.empty(), "loading missing storage does not create records");
    Disk cleared; cleared.records[LEGACY] = std::vector<uint8_t>(8, 0);
    Probe upgrade(cleared); upgrade.load_journal_();
    require(!upgrade.cached_journal_valid_ && !upgrade.journal_error_(), "zero legacy record is an old explicit clear");
    require(cleared.writes.empty(), "old cleared record needs no migration write");
    passed("empty");
  }
  static void legacy_versions() {
    for (const uint32_t magic : {0x44534B31U, 0x44534B32U, 0x44534B33U}) {
      Disk d; put_legacy(d, magic); const auto legacy = d.records[LEGACY];
      Probe p(d); p.target_enabled_ = p.keep_awake_enabled_ = false; p.load_journal_();
      require(!p.journal_error_() && p.cached_journal_valid_, "legacy record migrated with targets disabled");
      const auto stored = persisted(d);
      require(stored.state == Journal::State::Retained && stored.original == 456 && stored.temporary == 122,
              "legacy raw values copied unchanged");
      require(stored.profile == desk_protocol::LegacyPresetJournal::profile_identity(),
              "migration preserves the frozen historical profile identity");
      require(stored.profile == p.height_profile_.identity(), "current built-in profile matches legacy semantics");
      require(d.records[LEGACY] == legacy, "migration preserves legacy backup");
      require(d.writes == std::vector<uint32_t>{CURRENT}, "migration writes only v4 once");
      require(p.target_move_.dirty() && p.target_move_.original() == 456, "migration enters recovery");
      require(p.wire_.sent.empty(), "migration sends no UART writes");
      d.writes.clear(); Probe again(d); again.load_journal_();
      require(d.writes.empty() && again.cached_journal_valid_, "restart does not rewrite migrated record");
    }
    passed("legacy_versions");
  }
  static void malformed() {
    for (unsigned kind = 0; kind < 5; ++kind) {
      Disk d; put(d, retained()); put_legacy(d);
      if (kind == 0) d.records[CURRENT][60] ^= 1;
      if (kind == 1) d.records[CURRENT].resize(63);
      if (kind == 2) d.records[CURRENT][0] ^= 1;
      if (kind == 3) d.records[CURRENT][6] = 3;
      if (kind == 4) d.records[CURRENT][7] = 1;
      const auto before = d.records;
      Probe p(d); p.load_journal_(); p.blocked_controls();
      require(d.records == before && d.writes.empty(), "malformed v4 cannot be replaced from legacy");
    }
    for (unsigned kind = 0; kind < 2; ++kind) {
      Disk d; put_legacy(d);
      if (kind == 0) d.records[LEGACY].resize(7);
      else d.records[LEGACY][0] = 0x35;
      const auto before = d.records;
      Probe p(d); p.load_journal_(); p.blocked_controls();
      require(d.records == before && d.writes.empty(), "malformed legacy is not absence");
    }
    passed("malformed");
  }
  static void read_errors() {
    for (uint32_t key : {LEGACY, CURRENT}) {
      Disk d; put_legacy(d); d.faults[key] = Load::Error;
      const auto before = d.records;
      Probe p(d); p.load_journal_(); p.blocked_controls();
      require(d.records == before && d.writes.empty(), "read failure cannot overwrite a record");
    }
    passed("read_errors");
  }
  static void identity_mismatches() {
    // One differing byte in each canonical field, with a valid record CRC.
    const size_t fields[] = {0,2,4,8,12,16,20,24,26,28,30,32,34,36,38,40,42,44,46};
    for (size_t offset : fields) {
      Disk d; auto j = retained(); j.profile.bytes[offset] ^= 1; put(d, j);
      const auto before = d.records;
      Probe p(d); p.load_journal_();
      require(p.cached_journal_valid_ && p.cached_journal_.original == 456, "mismatch retains original in memory");
      p.blocked_controls();
      require(d.records == before && d.writes.empty(), "mismatched profile is immutable");
    }
    passed("identity_mismatches");
  }
  static void conflicts() {
    Disk d; put(d, retained()); put_legacy(d, 0x44534B33, 333);
    const auto before = d.records;
    Probe p(d); p.load_journal_(); p.blocked_controls();
    require(p.cached_journal_valid_ && p.cached_journal_.original == 456, "v4 original survives conflict");
    require(d.records == before && d.writes.empty(), "conflict preserves both originals");
    Disk same; put(same, retained()); put_legacy(same, 0x44534B32, 456, 200);
    Probe compatible(same); compatible.load_journal_();
    require(!compatible.journal_error_() && compatible.cached_journal_.temporary == 122,
            "older temporary value does not override v4");
    passed("conflicts");
  }
  static void migration_failures() {
    for (bool persisted_first : {false, true}) {
      Disk d; put_legacy(d); const auto original = d.records[LEGACY];
      d.fail_key = CURRENT; d.persist_before_failure = persisted_first;
      Probe p(d); p.load_journal_();
      require(p.cached_journal_valid_ && p.cached_journal_.original == 456, "failed migration retains RAM original");
      require(d.records[LEGACY] == original, "failed migration retains legacy storage");
      p.blocked_controls();
      d.fail_key = 0; d.writes.clear(); Probe rebooted(d); rebooted.load_journal_();
      require(!rebooted.journal_error_() && rebooted.cached_journal_.original == 456,
              "restart recovers before or after migration commit");
    }
    passed("migration_failures");
  }
  static void acceptance_failures() {
    for (bool persisted_first : {false, true}) {
      Disk d; put(d, retained()); put_legacy(d);
      Probe p(d); p.load_journal_(); d.writes.clear();
      const auto legacy = d.records[LEGACY];
      d.fail_key = CURRENT; d.persist_before_failure = persisted_first;
      p.accept_twice();
      require(p.cached_journal_valid_ && p.cached_journal_.original == 456, "failed acceptance keeps RAM original");
      require(d.records[LEGACY] == legacy, "legacy untouched before tombstone succeeds");
      require(d.writes == std::vector<uint32_t>{CURRENT}, "failed tombstone stops before legacy clear");
      d.fail_key = 0; d.writes.clear(); Probe rebooted(d); rebooted.load_journal_();
      require(rebooted.cached_journal_valid_ && rebooted.cached_journal_.original == 456,
              "active legacy survives either tombstone outcome on restart");
      require(d.records[LEGACY] == legacy && persisted(d).state == Journal::State::Retained,
              "restart reconciles a committed tombstone without discarding legacy");
    }
    passed("acceptance_failures");
  }
  static void resolved_legacy_round_trip() {
    for (uint32_t magic : {0x44534B31U, 0x44534B32U, 0x44534B33U}) {
      for (uint16_t old_original : {uint16_t(333), uint16_t(456)}) {
        for (bool disabled : {false, true}) {
          Disk d; put(d, retained(old_original)); put_legacy(d, magic, old_original);
          Probe before_downgrade(d); before_downgrade.load_journal_(); d.writes.clear();
          before_downgrade.accept_twice(456);
          require(persisted(d).state == Journal::State::Resolved &&
                  d.records[LEGACY] == std::vector<uint8_t>(8, 0), "Accept completes before downgrade");
          // An older version borrows again. Its temporary value 122 may then
          // survive in controller EEPROM. Equal raw values do not date a record.
          put_legacy(d, magic, 456, 122);
          const auto new_legacy = d.records[LEGACY]; d.writes.clear();
          Probe upgraded(d); upgraded.target_enabled_ = upgraded.keep_awake_enabled_ = !disabled;
          upgraded.load_journal_();
          require(!upgraded.journal_error_() && upgraded.cached_journal_valid_ &&
                  upgraded.target_move_.dirty(), "round-trip upgrade imports the active legacy backup");
          require(persisted(d).state == Journal::State::Retained && persisted(d).original == 456 &&
                  persisted(d).temporary == 122, "newer original is retained regardless of tombstone values");
          require(d.writes == std::vector<uint32_t>{CURRENT} && d.records[LEGACY] == new_legacy,
                  "upgrade migrates without clearing the active legacy record");
          require(upgraded.wire_.sent.empty(), "migration itself sends no UART commands");
          upgraded.inspect_recovery(122);
          require(upgraded.target_move_.dirty() && upgraded.target_move_.failed(),
                  "temporary current value requires a fresh recovery decision");
          upgraded.wire_.sent.clear(); const auto writes_before = d.writes;
          require(!upgraded.move_to_height(82) && !upgraded.recall_preset(1),
                  "no movement can bypass the pending legacy recovery");
          require(upgraded.wire_.sent.empty() && d.writes == writes_before && d.records[LEGACY] == new_legacy,
                  "failed new borrow cannot erase the original or send movement");
        }
      }
    }
    passed("resolved_legacy_round_trip");
  }
  static void resolved_no_legacy() {
    for (bool cleared : {false, true}) {
      Disk d; auto j = retained(); j.state = Journal::State::Resolved;
      j.profile.bytes[0] ^= 1; put(d, j);
      if (cleared) d.records[LEGACY] = std::vector<uint8_t>(8, 0);
      const auto records_before = d.records;
      Probe p(d); p.load_journal_();
      require(!p.cached_journal_valid_ && !p.target_move_.dirty() && !p.journal_error_(),
              "resolved record with missing or cleared legacy has no retained profile restriction");
      require(d.records == records_before && d.writes.empty(), "completed resolution needs no startup writes");
      // No speculative legacy cleanup may occur before a new borrow.
      p.reach_backup(); p.target_loop_(esphome::millis());
      require(d.writes == std::vector<uint32_t>{CURRENT} && persisted(d).original == 456,
              "new borrow writes only its v4 backup");
      require(p.wire_.sent.size() == 1 && p.wire_.sent[0].bytes[0] == 0x19,
              "normal UART borrowing follows durable v4 save");
      require(cleared ? d.records[LEGACY] == std::vector<uint8_t>(8, 0) : d.records.count(LEGACY) == 0,
              "borrow leaves the legacy key untouched");
    }
    passed("resolved_no_legacy");
  }
  static void resolved_legacy_errors() {
    for (unsigned kind = 0; kind < 4; ++kind) {
      Disk d; auto j = retained(); j.state = Journal::State::Resolved; put(d, j); put_legacy(d);
      if (kind == 0) d.faults[LEGACY] = Load::Error;
      if (kind == 1) d.faults[LEGACY] = Load::Invalid;
      if (kind == 2) d.records[LEGACY][0] = 0x35;
      if (kind == 3) d.records[LEGACY].resize(7);
      const auto before = d.records;
      Probe p(d); p.load_journal_(); p.blocked_controls();
      require(d.records == before && d.writes.empty(), "resolved marker cannot conceal invalid or unreadable legacy");
    }
    passed("resolved_legacy_errors");
  }
  static void resolved_migration_failures() {
    for (bool persisted_first : {false, true}) {
      Disk d; auto j = retained(333); j.state = Journal::State::Resolved; put(d, j); put_legacy(d);
      const auto legacy = d.records[LEGACY]; d.fail_key = CURRENT; d.persist_before_failure = persisted_first;
      Probe p(d); p.load_journal_();
      require(p.cached_journal_valid_ && p.cached_journal_.original == 456, "failed reconciliation keeps new legacy original in RAM");
      require(d.records[LEGACY] == legacy && d.writes == std::vector<uint32_t>{CURRENT},
              "failed reconciliation never clears legacy");
      p.blocked_controls();
      d.fail_key = 0; d.writes.clear(); Probe again(d); again.load_journal_();
      require(!again.journal_error_() && again.cached_journal_valid_ && persisted(d).original == 456,
              "restart recovers whether migration reached durable storage or not");
      require(d.records[LEGACY] == legacy && again.wire_.sent.empty(), "retry preserves legacy without movement");
    }
    passed("resolved_migration_failures");
  }
  static void legacy_clear_failures() {
    for (bool persisted_first : {false, true}) {
      Disk d; put(d, retained()); put_legacy(d); const auto legacy = d.records[LEGACY];
      Probe p(d); p.load_journal_(); d.writes.clear();
      d.fail_key = LEGACY; d.persist_before_failure = persisted_first;
      p.accept_twice();
      require(persisted(d).state == Journal::State::Resolved, "resolved record commits before legacy cleanup");
      require(d.writes == std::vector<uint32_t>({CURRENT, LEGACY}), "Accept performs the two durable writes in order");
      require(p.cached_journal_valid_ && p.cached_journal_.original == 456 && p.target_move_.dirty() &&
              p.journal_error_(), "uncertain cleanup keeps RAM backup and interlock");
      require(p.result_.state.find("backup discarded") == std::string::npos,
              "failed cleanup must not announce successful backup deletion");
      d.fail_key = 0; d.writes.clear();
      Probe again(d); again.target_enabled_ = again.keep_awake_enabled_ = false; again.load_journal_();
      require(!again.journal_error_(), "restart rechecks storage even with borrowing disabled");
      if (persisted_first) {
        require(!again.cached_journal_valid_ && !again.target_move_.dirty() && d.writes.empty(),
                "completed legacy clear leaves no backup after restart");
      } else {
        require(again.cached_journal_valid_ && again.target_move_.dirty() && d.records[LEGACY] == legacy,
                "interrupted cleanup becomes recoverable with movement disabled");
        d.writes.clear(); again.accept_twice();
        require(!again.cached_journal_valid_ && !again.target_move_.dirty() &&
                d.records[LEGACY] == std::vector<uint8_t>(8, 0), "fresh explicit Accept can finish interrupted cleanup");
        require(d.writes == std::vector<uint32_t>({CURRENT, LEGACY}), "fresh Accept keeps durable order");
      }
    }
    passed("legacy_clear_failures");
  }
  static void accept_unwritable_legacy() {
    for (unsigned kind = 0; kind < 3; ++kind) {
      Disk d; put(d, retained()); put_legacy(d);
      if (kind == 0) d.records[LEGACY].resize(7);
      if (kind == 1) d.faults[LEGACY] = Load::Invalid;
      Probe p(d); p.load_journal_();
      require(p.cached_journal_valid_ && !p.journal_error_(),
              "valid retained v4 remains recoverable with malformed legacy storage");
      if (kind == 2) d.faults[LEGACY] = Load::Error; // The read can fail after startup.
      const auto before = d.records; d.writes.clear();
      p.accept_twice(122);
      require(d.writes.empty() && d.records == before,
              "Accept preflight refuses unwritable legacy before any durable write");
      require(persisted(d).state == Journal::State::Retained && persisted(d).original == 456 &&
              p.cached_journal_valid_ && p.cached_journal_.original == 456 && p.target_move_.dirty(),
              "failed preflight preserves retained original in storage and RAM");
      require(p.journal_error_() && p.result_.state.find("backup discarded") == std::string::npos,
              "unwritable legacy reports failure instead of completed acceptance");
      Probe restarted(d); restarted.load_journal_();
      require(restarted.cached_journal_valid_ && restarted.cached_journal_.original == 456,
              "restart still finds the original v4 backup");
      if (kind == 2) {
        require(restarted.journal_error_(), "continued read failure remains blocked");
        d.faults.erase(LEGACY);
        Probe repaired(d); repaired.load_journal_();
        require(!repaired.journal_error_(), "transient storage failure clears after a successful restart read");
        repaired.restore_twice(122);
      } else {
        require(!restarted.journal_error_(), "malformed legacy cannot erase the valid v4 recovery path");
        restarted.restore_twice(122);
      }
      require(d.records == before && d.writes.empty(), "RAM restoration preserves both storage records");
    }
    passed("accept_unwritable_legacy");
  }
  static void resolved_recovery_actions() {
    for (bool restore : {false, true}) {
      Disk d; auto j = retained(333); j.state = Journal::State::Resolved; put(d, j); put_legacy(d);
      Probe p(d); p.target_enabled_ = p.keep_awake_enabled_ = false; p.load_journal_();
      p.inspect_recovery(122); d.writes.clear();
      if (restore) {
        p.restore_twice(122);
        require(persisted(d).state == Journal::State::Retained && persisted(d).original == 456,
                "explicit Restore keeps the imported baseline");
      } else {
        p.accept_twice(122);
        require(!p.cached_journal_valid_ && !p.target_move_.dirty() &&
                persisted(d).state == Journal::State::Resolved && d.records[LEGACY] == std::vector<uint8_t>(8, 0),
                "explicit Accept resolves both formats without movement");
        require(d.writes == std::vector<uint32_t>({CURRENT, LEGACY}), "Accept commits marker before clearing legacy");
        d.writes.clear(); Probe restarted(d); restarted.load_journal_();
        require(!restarted.cached_journal_valid_ && !restarted.journal_error_() && d.writes.empty(),
                "completed acceptance remains resolved on restart");
      }
    }
    passed("resolved_recovery_actions");
  }
  static void matching_original_recheck() {
    Disk d; auto j = retained(); j.state = Journal::State::Resolved; put(d, j); put_legacy(d);
    Probe p(d); p.load_journal_(); const auto legacy = d.records[LEGACY];
    p.inspect_recovery(456);
    require(!p.target_move_.dirty() && !p.target_move_.busy() && p.cached_journal_valid_,
            "matching current original may resolve RAM check while keeping the backup");
    d.writes.clear(); p.wire_.sent.clear(); p.reach_backup(); p.target_loop_(esphome::millis());
    require(persisted(d).original == 456 && d.records[LEGACY] == legacy,
            "later borrowing never erases the retained legacy original");
    for (uint32_t key : d.writes) require(key == CURRENT, "borrowing cannot clear legacy storage");
    passed("matching_original_recheck");
  }
  static void setup_pending() {
    for (unsigned kind = 0; kind < 4; ++kind) {
      Disk d; auto j = retained();
      if (kind == 0) { put(d, j); d.records[CURRENT][60] ^= 1; }
      if (kind == 1) { j.profile.bytes[0] ^= 1; put(d, j); }
      if (kind >= 2) { j.state = Journal::State::Resolved; put(d, j); put_legacy(d); }
      if (kind == 2) d.faults[LEGACY] = Load::Error;
      esphome::binary_sensor::BinarySensor pending;
      Probe p(d); p.target_enabled_ = p.keep_awake_enabled_ = false;
      p.set_restore_pending_sensor(&pending); p.setup();
      require(pending.has_state() && pending.state, "setup publishes pending for storage, profile or legacy recovery");
      require(p.wire_.sent.size() == 1 && p.wire_.sent[0].bytes[0] != 0x19 &&
              p.wire_.sent[0].bytes[0] != 0x17, "setup only subscribes to passive telemetry");
    }
    passed("setup_pending");
  }
  static void additional_controls() {
    Disk d; auto j = retained(); j.profile.bytes[0] ^= 1; put(d, j);
    Probe p(d); p.load_journal_();
    esphome::desk_uart::LoadLimitNumber limit;
    p.set_load_limit_number(0, &limit); p.original_known_[0] = true; p.original_limits_[0] = 100;
    require(!p.set_load_limit(desk_protocol::LoadLimit::Upward, 101) &&
            !p.restore_load_limit(desk_protocol::LoadLimit::Upward), "profile block covers load-limit edits");
    require(!p.begin_firmware_export() && !p.firmware_export_active(), "profile block prevents firmware-session entry");
    require(p.wire_.sent.empty(), "blocked diagnostic/edit entry points send no UART frames");
    // Timer policy is unchanged: an unreadable journal alone does not block T,
    // but an active recovery transaction still does through the existing gate.
    Disk broken; broken.faults[CURRENT] = Load::Error;
    Probe timer(broken); timer.load_journal_(); timer.timer_enabled_ = true;
    require(timer.set_timer(30) && timer.timer_.busy(), "storage error alone leaves timer controls available");
    require(timer.journal_error_() && broken.writes.empty(), "timer does not bypass the journal interlock or mutate storage");
    for (const auto &f : timer.wire_.sent)
      require(f.bytes[0] != 0x19 && f.bytes[0] != 0x1B, "timer never changes preset RAM or EEPROM");
    passed("additional_controls");
  }
  static void borrow_failure() {
    Disk d; Probe p(d); p.load_journal_(); d.fail_key = CURRENT;
    p.reach_backup(); p.target_loop_(esphome::millis());
    require(!d.writes.empty() && d.writes.back() == CURRENT, "borrow attempts durable save");
    require(p.wire_.sent.empty(), "failed backup never writes preset or recalls");
    require(p.target_move_.failed(), "failed backup aborts transaction");
    passed("borrow_failure");
  }
  static void configuration_identity() {
    Disk d; put(d, retained()); Probe p(d);
    p.set_display_inches(true); p.set_target_range(75, 110); p.load_journal_();
    require(!p.journal_error_() && p.cached_journal_valid_, "units and user range do not change identity");
    require(d.writes.empty() && p.cached_journal_.original == 456, "configuration does not rewrite backup");
    p.invalidate_journal_session_();
    require(p.cached_journal_valid_ && p.journal_recheck_pending_, "connection loss retains profiled backup");
    require(p.cached_journal_.profile == p.height_profile_.identity(), "profile retained across connection loss");
    passed("configuration_identity");
  }
};
}  // namespace profile_journal_test
