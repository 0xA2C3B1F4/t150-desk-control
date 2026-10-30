#pragma once

#include "desk_protocol/protocol.h"
#include "desk_protocol/identity.h"
#include "desk_protocol/target.h"
#include "desk_protocol/direct_target.h"
#include "desk_protocol/uart_observation.h"
#include "desk_protocol/timer.h"
#include "desk_protocol/presets.h"
#include "desk_protocol/preset_journal.h"
#include "journal_storage.h"
#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/button/button.h"
#include "esphome/components/number/number.h"
#include "esphome/components/text_sensor/text_sensor.h"

namespace esphome { namespace desk_uart {

class DeskUART : public Component, public uart::UARTDevice {
 public:
  // Exclusive read-only bootloader session, callable only on the main loop.
  uart::UARTComponent *firmware_export_uart() const { return this->parent_; }
  bool begin_firmware_export();
  void end_firmware_export();
  bool firmware_export_active() const { return firmware_export_active_; }
  void setup() override;
  void loop() override;
  void dump_config() override;
  void on_shutdown() override;
  float get_setup_priority() const override { return setup_priority::DATA; }
  void set_height_sensor(sensor::Sensor *s) { height_sensor_ = s; }
  void set_load_raw_sensor(sensor::Sensor *s) { load_raw_sensor_ = s; }
  void set_display_inches(bool inches) { display_inches_ = inches; }
  float display_height(float cm) const { return display_inches_ ? cm / 2.54f : cm; }
  float input_height_cm(float value) const { return display_inches_ ? value * 2.54f : value; }
  void set_connected_sensor(binary_sensor::BinarySensor *s) { connected_sensor_ = s; }
  void set_frame_errors_sensor(sensor::Sensor *s) { frame_errors_sensor_ = s; }
  void set_sample_gap_sensor(sensor::Sensor *s) { sample_gap_sensor_ = s; }
  void set_motion_enabled(bool enabled) { motion_enabled_ = enabled; }
  // True accepts the button, possibly pending keep-awake cleanup for <= 4 s.
  // No request survives a fault, Stop, telemetry gap or height change.
  bool recall_preset(uint8_t slot);
  bool move_to_height(float cm);
  bool step_height(bool up);
  bool restore_temporary_preset();
  bool accept_current_preset();
  void set_target_number(number::Number *n) { target_number_ = n; }
  void set_target_range(float low, float high) { min_height_ = low; max_height_ = high; }
  void set_keep_awake_enabled(bool enabled) { keep_awake_enabled_ = enabled; }
  void set_direct_mode(bool enabled) { direct_mode_ = enabled; }
  void set_uart_observation_enabled(bool enabled) { uart_observation_enabled_ = enabled; }
  void set_keep_awake_auto(bool enabled) { keep_awake_auto_ = enabled; }
  void set_target_enabled(bool enabled) { target_enabled_ = enabled; }
  void set_preference_key(uint32_t key) { preference_key_ = key; }
  void set_v4_preference_key(uint32_t key) { v4_preference_key_ = key; }
  void set_restore_pending_sensor(binary_sensor::BinarySensor *s) { restore_pending_sensor_ = s; }
  void set_load_limit_number(uint8_t index, number::Number *n) { if (index < 2) limit_numbers_[index] = n; }
  void set_preset_height_number(uint8_t slot, number::Number *n) { if (slot >= 1 && slot <= 3) preset_numbers_[slot - 1] = n; }
  bool set_preset_height(uint8_t slot, float cm);
  bool set_load_limit(desk_protocol::LoadLimit limit, uint16_t raw);
  bool restore_load_limit(desk_protocol::LoadLimit limit);
  // Call on the ESPHome main loop, never from an ISR or another task.
  // True means one request was handed to UART, NOT that motion has stopped.
  bool stop();
  // Compatibility entry point; normal stops clear automatically under the same
  // ACK, observed settling and cleanup checks. Never bypasses a fault.
  bool clear_stop_latch();
  void set_control_status(text_sensor::TextSensor *s) { control_status_ = s; }
  void set_command_result(text_sensor::TextSensor *s) { command_result_ = s; }
  void set_controller_model(text_sensor::TextSensor *s) { identity_sensors_[0] = s; }
  void set_controller_manufacturer(text_sensor::TextSensor *s) { identity_sensors_[1] = s; }
  void set_controller_identifiers(text_sensor::TextSensor *s) { identity_sensors_[2] = s; }
  void set_controller_date(text_sensor::TextSensor *s) { identity_sensors_[3] = s; }
  void set_stop_locked_sensor(binary_sensor::BinarySensor *s) { stop_locked_sensor_ = s; }
  bool is_stop_latched() const { return stop_latched_; }
  void set_timer_enabled(bool enabled) { timer_enabled_ = enabled; }
  void set_timer_pending_sensor(binary_sensor::BinarySensor *s) { timer_pending_sensor_ = s; }
  // minutes=0 requests cancellation using five short T-key clicks.
  bool set_timer(uint16_t minutes);
  bool cancel_timer() { return set_timer(0); }

 protected:
  // Only the existing control profile is configured; research profiles cannot drive UART transactions.
  const desk_protocol::HeightProfile height_profile_{desk_protocol::HeightProfile::known_t150()};
  bool firmware_export_active_{false};
  void send_(const desk_protocol::Frame &frame);
  void identity_loop_(uint32_t now);
  void identity_frame_(const desk_protocol::Frame &frame, uint32_t now);
  text_sensor::TextSensor *identity_sensors_[4]{nullptr, nullptr, nullptr, nullptr};
  char identity_values_[3][18]{};
  uint8_t identity_received_{0}, identity_pending_{255}, identity_next_{0};
  uint8_t identity_attempts_[6]{};
  uint32_t identity_sent_at_{0};
  void preset_settings_loop_(uint32_t now);
  bool preset_access_allowed_(uint32_t now) const;
  desk_protocol::PresetSettings preset_settings_{height_profile_};
  number::Number *preset_numbers_[3]{nullptr, nullptr, nullptr};
  bool preset_known_[3]{false, false, false};
  uint8_t next_preset_{0};
  uint32_t last_preset_poll_{0}, last_preset_recall_{0};
  void controls_loop_(uint32_t now);
  const char *control_reason_(uint32_t now) const;
  bool result_(bool accepted, const char *message);
  void discard_retarget_(const char *reason);
  text_sensor::TextSensor *control_status_{nullptr}, *command_result_{nullptr};
  binary_sensor::BinarySensor *stop_locked_sensor_{nullptr};
  bool retarget_pending_{false}, last_stop_locked_{false}, sync_target_{false};
  float retarget_cm_{0};
  uint32_t retarget_started_{0};
  const char *last_control_status_{nullptr};
  void timer_loop_(uint32_t now);
  desk_protocol::TimerSequence timer_{};
  binary_sensor::BinarySensor *timer_pending_sensor_{nullptr};
  bool timer_enabled_{false}, last_timer_pending_{false};
  const char *last_timer_error_{nullptr};
  void target_loop_(uint32_t now);
  void direct_loop_(uint32_t now);
  void uart_observation_log_();
  desk_protocol::UARTObservation uart_observation_{};
  bool uart_observation_enabled_{false};
  uint32_t last_uart_observation_log_{0};
  bool direct_access_allowed_(uint32_t now) const;
  bool direct_move_(float cm, uint32_t now);
  desk_protocol::DirectTarget direct_{};
  bool direct_mode_{false}, direct_probe_done_{false}, keep_awake_auto_{false};
  uint8_t direct_last_phase_{255};
  const char *last_direct_error_{nullptr};
  void load_journal_();
  const char *journal_error_() const;
  virtual JournalLoad read_journal_storage_(uint32_t key, uint8_t *data, size_t length) {
    return read_journal_storage(key, data, length);
  }
  virtual bool write_journal_storage_(uint32_t key, const uint8_t *data, size_t length) {
    return write_journal_storage(key, data, length);
  }
  bool write_journal_(const desk_protocol::PresetJournal &journal);
  bool clear_legacy_journal_();
  bool save_journal_(bool active);
  bool recovery_access_allowed_(uint32_t now) const;
  bool clear_cached_journal_();
  void keep_awake_loop_(uint32_t now);
  bool pending_preset_loop_(uint32_t now);
  void discard_pending_preset_(const char *reason);
  uint8_t pending_preset_{0};
  uint32_t pending_preset_started_{0};
  uint16_t pending_preset_q_{0};
  bool keep_awake_enabled_{false}, keep_awake_active_{false}, keep_awake_fault_{false};
  bool keep_awake_recovery_{false}, cached_journal_valid_{false}, keep_awake_pressed_{false};
  // Tracks early abort or user-requested yield through verified restore or fault.
  const char *soft_abort_error_{nullptr};
  void check_retained_preset_(uint16_t preset3_raw);
  void invalidate_journal_session_();
  bool journal_recheck_pending_{false};
  bool recovery_check_active_{false}, accept_current_active_{false}, restore_requested_{false};
  uint32_t last_keep_awake_{0};
  uint16_t keep_awake_q_{0};
  const char *last_internal_status_{nullptr};
  uint8_t last_internal_phase_{255};
  bool last_internal_release_{false};
  uint32_t stable_ms_(uint32_t /*now*/) const { return height_seen_ ? static_cast<uint32_t>(last_height_received_ - last_height_change_) : 0; }
  desk_protocol::PresetJournal cached_journal_{};
  const char *journal_block_reason_{nullptr};
  // RAM readback evidence only; never authorizes deleting the durable backup.
  struct VerifiedJournal { bool valid; uint16_t original; uint16_t temporary; desk_protocol::HeightProfileIdentity profile; };
  VerifiedJournal verified_journal_{false, 0, 0, {}};
  desk_protocol::TemporaryPreset target_move_{height_profile_};
  number::Number *target_number_{nullptr};
  binary_sensor::BinarySensor *restore_pending_sensor_{nullptr};
  uint32_t preference_key_{0}, v4_preference_key_{0}, last_height_change_{0}, last_height_received_{0};
  uint16_t last_q_{0};
  float min_height_{height_profile_.control_min_tenths() / 10.0f};
  float max_height_{height_profile_.control_max_tenths() / 10.0f};
  bool target_enabled_{false}, height_seen_{false}, target_initialized_{false};
  bool display_inches_{false};
  bool last_restore_pending_{false};
  const char *last_target_error_{nullptr};
  void limit_frame_(const desk_protocol::Frame &frame, uint32_t now);
  void limit_loop_(uint32_t now);
  void query_limit_(uint8_t index, uint32_t now, bool verify = false);
  enum class LimitPhase { Idle, Read, WriteAck, Verify };
  LimitPhase limit_phase_{LimitPhase::Idle};
  number::Number *limit_numbers_[2]{nullptr, nullptr};
  uint16_t original_limits_[2]{0, 0}, expected_limit_{0};
  bool original_known_[2]{false, false};
  uint8_t limit_index_{0}, next_limit_{0};
  uint32_t limit_started_{0}, last_limit_poll_{0};
  void subscribe_(uint32_t now);
  void release_(const desk_protocol::Frame &frame, uint32_t now);
  desk_protocol::Parser parser_{};
  desk_protocol::HeightMonitor monitor_{};
  desk_protocol::ButtonPulse pulse_{};
  desk_protocol::AckTracker release_ack_{};
  desk_protocol::AckTracker stop_ack_{};
  void invalidate_load_raw_();
  sensor::Sensor *height_sensor_{nullptr}, *load_raw_sensor_{nullptr};
  bool load_raw_valid_{false};
  sensor::Sensor *frame_errors_sensor_{nullptr}, *sample_gap_sensor_{nullptr};
  uint32_t frame_errors_{0}, published_frame_errors_{0}, last_frame_error_publish_{0};
  uint32_t max_sample_gap_{0}, gap_window_start_{0};
  binary_sensor::BinarySensor *connected_sensor_{nullptr};
  uint32_t last_subscribe_{0}, last_stop_{0};
  bool stop_latched_{false}, stop_acknowledged_{false};
  bool connected_{false}, motion_enabled_{false}, release_fault_{false};
};

class PresetHeightNumber : public number::Number {
 public:
  void set_parent(DeskUART *parent) { parent_ = parent; }
  void set_slot(uint8_t slot) { slot_ = slot; }
 protected:
  void control(float value) override {
    if (!parent_->set_preset_height(slot_, parent_->input_height_cm(value))) this->publish_state(this->state);
  }
  DeskUART *parent_{nullptr};
  uint8_t slot_{0};
};

class TargetHeightNumber : public number::Number {
 public:
  void set_parent(DeskUART *parent) { parent_ = parent; }
 protected:
  void control(float value) override {
    if (parent_->move_to_height(parent_->input_height_cm(value))) this->publish_state(value);
    else this->publish_state(this->state);
  }
  DeskUART *parent_{nullptr};
};
class StepHeightButton : public button::Button {
 public:
  void set_parent(DeskUART *parent) { parent_ = parent; }
  void set_up(bool up) { up_ = up; }
 protected:
  void press_action() override { parent_->step_height(up_); }
  DeskUART *parent_{nullptr};
  bool up_{true};
};
class RestorePresetButton : public button::Button {
 public:
  void set_parent(DeskUART *parent) { parent_ = parent; }
 protected:
  void press_action() override { parent_->restore_temporary_preset(); }
  DeskUART *parent_{nullptr};
};
class AcceptCurrentPresetButton : public button::Button {
 public:
  void set_parent(DeskUART *parent) { parent_ = parent; }
 protected:
  void press_action() override { parent_->accept_current_preset(); }
  DeskUART *parent_{nullptr};
};

class LoadLimitNumber : public number::Number {
 public:
  void set_parent(DeskUART *parent) { parent_ = parent; }
  void set_index(uint8_t index) { index_ = index; }
 protected:
  void control(float value) override;
  DeskUART *parent_{nullptr};
  uint8_t index_{0};
};
class RestoreLoadLimitButton : public button::Button {
 public:
  void set_parent(DeskUART *parent) { parent_ = parent; }
  void set_index(uint8_t index) { index_ = index; }
 protected:
  void press_action() override {
    parent_->restore_load_limit(index_ == 0 ? desk_protocol::LoadLimit::Upward : desk_protocol::LoadLimit::DownwardBase);
  }
  DeskUART *parent_{nullptr};
  uint8_t index_{0};
};

class StopButton : public button::Button {
 public:
  void set_parent(DeskUART *parent) { parent_ = parent; }
 protected:
  void press_action() override { parent_->stop(); }
  DeskUART *parent_{nullptr};
};

template<typename... Ts> class StopAction : public Action<Ts...> {
 public:
  explicit StopAction(DeskUART *parent) : parent_(parent) {}
  void play(Ts... x) override { parent_->stop(); }
 protected:
  DeskUART *parent_;
};

class PresetButton : public button::Button {
 public:
  void set_parent(DeskUART *parent) { parent_ = parent; }
  void set_slot(uint8_t slot) { slot_ = slot; }
 protected:
  void press_action() override { parent_->recall_preset(slot_); }
  DeskUART *parent_{nullptr};
  uint8_t slot_{0};
};
class TimerButton : public button::Button {
 public:
  void set_parent(DeskUART *parent) { parent_ = parent; }
  void set_minutes(uint16_t minutes) { minutes_ = minutes; }
 protected:
  void press_action() override { parent_->set_timer(minutes_); }
  DeskUART *parent_{nullptr};
  uint16_t minutes_{0};
};
} }  // namespace esphome::desk_uart
