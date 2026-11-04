#include "desk_uart.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"
#include "esphome/core/controller_registry.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cinttypes>

// Native test consumers may compile the adapter without ESPHome code generation.
#ifndef DESK_SOURCE_VERSION
#define DESK_SOURCE_VERSION "unknown"
#define DESK_SOURCE_REVISION "unknown"
#define DESK_SOURCE_STATE "unknown"
#endif

namespace esphome {
namespace desk_uart {
static const char* const TAG = "desk_uart";

MotionTrimNumber* MotionTrimNumber::first_ = nullptr;
MotionTrimNumber::MotionTrimNumber() : next_(first_) {
  first_ = this;
}
bool MotionTrimNumber::is_instance(const number::Number* candidate) {
  for (auto* item = first_; item != nullptr; item = item->next_) {
    if (item == candidate) {
      return true;
    }
  }
  return false;
}
void MotionTrimNumber::invalidate() {
  if (!this->has_state()) {
    return;
  }
  this->set_has_state(false);
  this->state = NAN;
#if defined(USE_CONTROLLER_REGISTRY) && defined(USE_NUMBER)
  ControllerRegistry::notify_number_update(this);
#endif
}

ChildLockSwitch* ChildLockSwitch::first_ = nullptr;
ChildLockSwitch::ChildLockSwitch() : next_(first_) {
  first_ = this;
}
bool ChildLockSwitch::is_instance(const switch_::Switch* candidate) {
  for (auto* item = first_; item != nullptr; item = item->next_)
    if (item == candidate)
      return true;
  return false;
}

void ChildLockSwitch::invalidate() {
#if defined(USE_CONTROLLER_REGISTRY) && defined(USE_SWITCH)
  const bool changed = this->has_state();
#endif
  this->set_has_state(false);
  this->publish_dedup_.next_unknown();
#if defined(USE_CONTROLLER_REGISTRY) && defined(USE_SWITCH)
  // Notify the web server even though there is no new boolean state. Its local
  // renderer uses has_state() to clear the last confirmed color. The native API
  // still needs the companion status/availability entities for unknown states.
  if (changed)
    ControllerRegistry::notify_switch_update(this);
#endif
}
// Interruptions caused by a successful Stop are logged, not published as
// command results, so they cannot replace that Stop's own result.
MotionSpeedSelect* MotionSpeedSelect::first_ = nullptr;
MotionSpeedSelect::MotionSpeedSelect() : next_(first_) {
  first_ = this;
}
bool MotionSpeedSelect::is_instance(const select::Select* candidate) {
  for (auto* item = first_; item != nullptr; item = item->next_)
    if (item == candidate)
      return true;
  return false;
}
void MotionSpeedSelect::invalidate() {
#if defined(USE_CONTROLLER_REGISTRY) && defined(USE_SELECT)
  const bool changed = this->has_state();
#endif
  this->set_has_state(false);
#if defined(USE_CONTROLLER_REGISTRY) && defined(USE_SELECT)
  if (changed)
    ControllerRegistry::notify_select_update(this);
#endif
}
static const char* const STOP_PRESET_REASON =
    "Preset setting interrupted by Stop; readback required";
static const char* reset_continuation_blocked(const desk_protocol::ResetSpeedReply& reply) {
  if (reply.admission_diagnostics) {
    switch (reply.resume_result) {
      case 4:
        return "Reset paused; release physical keys to continue";
      case 5:
        return "Reset paused; panel lock blocks continuation";
      case 6:
        return "Reset paused; controller is blocking movement";
    }
  }
  if (reply.child_lock)
    return "Reset paused; panel lock blocks continuation";
  return "Reset paused; continuation unavailable; inspect the desk";
}

void DeskUART::invalidate_load_raw_() {
  if (!this->load_raw_valid_)
    return;
  this->load_raw_valid_ = false;
  if (this->load_raw_sensor_)
    this->load_raw_sensor_->publish_state(NAN);
}

bool DeskUART::begin_firmware_export(bool motor_isolated) {
  const uint32_t now = millis();
  this->cancel_motion_trim_("Trim change cancelled by firmware maintenance");
  this->yield_child_lock_read_();
  const bool reset_link_stale = !this->height_seen_ || !this->monitor_.fresh(now) ||
                                static_cast<uint32_t>(now - this->last_height_received_) > 750;
  // motor_isolated is the caller's explicit physical recovery precondition.
  // Host time proves only that this adapter has stopped sending lease grants;
  // it cannot prove MCU tick progress or that a hung controller cut outputs.
  const bool isolated_reset_recovery =
      motor_isolated && reset_link_stale && !this->reset_speed_.owned() &&
      !this->reset_speed_.busy() && static_cast<uint32_t>(now - this->last_reset_lease_tx_) >= 1500;
  if (this->custom_write_pending_())
    return this->result_(false, "Custom timer/buzzer transaction pending");
  if (!this->reset_speed_.maintenance_safe(now) && !isolated_reset_recovery)
    return false;
  if (this->firmware_export_active_ ||
      (!motor_isolated &&
       (this->direct_.busy() || this->direct_.set_sent() || !this->preset_access_allowed_(now) ||
        this->preset_settings_.busy() || this->target_move_.dirty() || this->keep_awake_active_ ||
        this->keep_awake_fault_ || this->recovery_check_active_ || this->accept_current_active_ ||
        this->restore_requested_ || this->identity_pending_ != 255)))
    return false;
  if (motor_isolated && (this->pulse_.active() || this->timer_.busy() ||
                         this->release_ack_.pending() || this->stop_ack_.pending()))
    return false;
  if (motor_isolated && this->monitor_.fresh(now) &&
      (this->stable_ms_(now) < 1500 || this->direct_.busy() || this->direct_.set_sent() ||
       this->target_move_.busy() || this->pulse_.active() || this->timer_.busy() ||
       this->release_ack_.pending() || this->stop_ack_.pending() || this->preset_settings_.busy()))
    return false;
  // Isolated maintenance may start while the application is absent. Drop
  // queued work without transmitting anything; never replay it on exit.
  this->firmware_export_active_ = true;
  this->cancel_motion_trim_("Trim settings unavailable during firmware maintenance", true);
  this->display_disconnect_();
  this->reset_speed_confirmed_ = false;
  if (motor_isolated) {
    this->discard_pending_preset_("Pending preset cancelled by firmware maintenance");
    this->target_move_.connection_lost(now);
    this->preset_settings_.cancel("Firmware maintenance; refresh presets later");
    this->timer_ = {};
    this->pulse_ = {};
    this->release_ack_ = {};
    this->stop_ack_ = {};
    this->keep_awake_active_ = false;
    this->keep_awake_recovery_ = false;
    this->restore_requested_ = this->accept_current_active_ = this->recovery_check_active_ = false;
    this->identity_pending_ = 255;
    this->limit_phase_ = LimitPhase::Idle;
  }
  this->invalidate_reset_speed_("Reset and speed controls unavailable during firmware maintenance");
  this->invalidate_journal_session_();
  this->uart_observation_.handoff();
  this->invalidate_detection_();
  this->invalidate_child_lock_(
      "Panel lock unavailable during firmware maintenance; lock state unknown");
  this->direct_.disconnect();
  this->direct_probe_done_ = false;
  this->firmware_export_active_ = true;
  this->parser_.reset();
  this->monitor_ = {};
  this->height_seen_ = this->connected_ = false;
  if (this->connected_sensor_)
    this->connected_sensor_->publish_state(false);
  this->height_sensor_->publish_state(NAN);
  this->invalidate_load_raw_();
  this->publish_motion_trim_(millis());
  if (this->control_status_)
    this->control_status_->publish_state("Firmware read active; desk controls paused");
  this->last_control_status_ = nullptr;
  this->custom_.disconnect();
  this->custom_available_published_ = false;
  if (this->custom_available_sensor_)
    this->custom_available_sensor_->publish_state(false);
  return true;
}
void DeskUART::end_firmware_export() {
  if (!this->firmware_export_active_)
    return;
  this->invalidate_detection_();
  this->invalidate_child_lock_(
      "Panel lock needs to be checked after firmware maintenance; lock state unknown");
  this->invalidate_journal_session_();
  this->parser_.reset();
  this->monitor_ = {};
  this->height_seen_ = this->connected_ = false;
  this->invalidate_load_raw_();
  this->identity_received_ = 0;
  this->identity_pending_ = 255;
  std::memset(this->identity_attempts_, 0, sizeof(this->identity_attempts_));
  std::memset(this->identity_values_, 0, sizeof(this->identity_values_));
  for (auto* sensor : this->identity_sensors_)
    if (sensor)
      sensor->publish_state("Unavailable");
  this->last_keep_awake_ = millis();
  this->last_subscribe_ = millis() - 5000;
  this->last_control_status_ = nullptr;
  this->firmware_export_active_ = false;
}

bool DeskUART::send_(const desk_protocol::Frame& frame) {
  if (this->firmware_export_active_)
    return false;
  if (this->display_.busy() && !this->display_sending_) {
    this->display_.cancel();
    this->display_message_ = "Display request interrupted; not retried";
    this->publish_display_();
  }
  if (this->motion_trim_busy_() && !this->motion_trim_sending_) {
    this->cancel_motion_trim_("Trim change cancelled by another control");
  }
  // Any competing transaction preempts the D7 reply wait. Never hold Stop or
  // a button release behind it; its final state becomes unknown, with no retry.
  this->custom_.observe_outgoing(frame);
  // Any other command (including Stop/native keys) cancels the exclusive probe.
  if (this->detection_.busy() && !this->detection_sending_)
    this->invalidate_detection_();
  if (this->child_lock_busy_() && !this->child_lock_sending_)
    this->invalidate_child_lock_(
        "Panel-lock request interrupted; lock state unknown, request not retried");
  if (this->reset_speed_.busy() && !this->reset_speed_sending_ &&
      this->reset_speed_.operation() == desk_protocol::ResetSpeed::Get)
    this->reset_speed_.yield_read();
  if (this->firmware_export_active_)
    return false;
  desk_protocol::Packet wire;
  if (desk_protocol::encode_resync(frame, wire)) {
    if (frame.size == 10 && frame.bytes[0] == 0xD5 &&
        frame.bytes[2] >= desk_protocol::ResetSpeed::Start &&
        frame.bytes[2] <= desk_protocol::ResetSpeed::Renew)
      this->last_reset_lease_tx_ = millis();
    this->uart_observation_.transmitted();
    this->write_array(wire.bytes, wire.size);
    return true;
  }
  return false;
}
// Optional, bounded background reads. No identity query participates in motion
// interlocks, ACK trackers, preset recovery or the keep-awake mechanism.
static const uint8_t IDENTITY_SELECTORS[6] = {3, 4, 5, 6, 7, 9};
static const uint8_t IDENTITY_ENTITIES[6] = {0, 1, 2, 2, 2, 3};
void DeskUART::identity_loop_(uint32_t now) {
  if (this->identity_pending_ != 255) {
    if (static_cast<uint32_t>(now - this->identity_sent_at_) < 700)
      return;
    this->identity_pending_ = 255;
  }
  if (static_cast<uint32_t>(now - this->identity_sent_at_) < 1000 ||
      !this->preset_access_allowed_(now) || this->preset_settings_.busy() ||
      this->target_move_.dirty())
    return;
  for (uint8_t offset = 0; offset < 6; ++offset) {
    const uint8_t index = (this->identity_next_ + offset) % 6;
    if (!this->identity_sensors_[IDENTITY_ENTITIES[index]] ||
        (this->identity_received_ & (1U << index)) || this->identity_attempts_[index] >= 3)
      continue;
    desk_protocol::Frame query;
    if (!desk_protocol::read_identity(IDENTITY_SELECTORS[index], query))
      return;
    this->identity_pending_ = index;
    this->identity_next_ = (index + 1) % 6;
    ++this->identity_attempts_[index];
    this->identity_sent_at_ = now;
    this->send_(query);
    return;
  }
}
void DeskUART::identity_frame_(const desk_protocol::Frame& frame, uint32_t now) {
  if (this->identity_pending_ == 255 || static_cast<uint32_t>(now - this->identity_sent_at_) >= 700)
    return;
  desk_protocol::IdentityText value;
  const uint8_t index = this->identity_pending_;
  if (!desk_protocol::decode_identity(frame, value) || value.selector != IDENTITY_SELECTORS[index])
    return;
  this->identity_pending_ = 255;
  this->identity_received_ |= 1U << index;
  if (index >= 2 && index <= 4) {
    std::memcpy(this->identity_values_[index - 2], value.text, sizeof(value.text));
    if ((this->identity_received_ & 0x1C) != 0x1C)
      return;
    char identifiers[80];
    std::snprintf(identifiers, sizeof(identifiers), "05: %s; 06: %s; 07: %s",
                  this->identity_values_[0], this->identity_values_[1], this->identity_values_[2]);
    this->identity_sensors_[2]->publish_state(identifiers);
  } else {
    this->identity_sensors_[IDENTITY_ENTITIES[index]]->publish_state(value.text);
  }
}
void DeskUART::subscribe_(uint32_t now) {
  desk_protocol::Frame frame;
  desk_protocol::telemetry(2, 500, frame);
  this->send_(frame);
  this->last_subscribe_ = now;
}
void DeskUART::setup() {
  if (this->source_version_)
    this->source_version_->publish_state(DESK_SOURCE_VERSION);
  if (this->source_revision_)
    this->source_revision_->publish_state(DESK_SOURCE_REVISION " (" DESK_SOURCE_STATE ")");
  const uint32_t boot = random_uint32();
  this->uart_observation_.reset(boot ? boot : 1, millis());
  this->direct_.seed(static_cast<uint16_t>(boot));
  this->detection_.seed(static_cast<uint16_t>(boot));
  this->child_lock_.seed(static_cast<uint16_t>(boot >> 16));
  this->reset_speed_.seed(static_cast<uint16_t>(boot ^ (boot >> 16)));
  this->custom_.seed(static_cast<uint16_t>(boot ^ (boot >> 16)));
  this->display_setup_(boot);
  this->motion_trim_.seed(static_cast<uint16_t>((boot >> 8) ^ boot));
  this->motion_trim_dt_probe_.seed(static_cast<uint16_t>(boot >> 8));
  this->publish_motion_trim_(millis());
  if (this->custom_available_sensor_)
    this->custom_available_sensor_->publish_state(false);
  if (this->custom_result_)
    this->custom_result_->publish_state("No custom command requested");
  this->last_reset_lease_tx_ = millis();
  if (this->motion_speed_select_)
    this->motion_speed_select_->invalidate();
  if (this->reset_available_)
    this->reset_available_->publish_state(false);
  if (this->motion_speed_available_)
    this->motion_speed_available_->publish_state(false);
  if (this->reset_status_)
    this->reset_status_->publish_state("Reset and speed support must be checked");
  this->child_lock_detection_.seed(static_cast<uint16_t>(boot));
  if (this->child_lock_switch_)
    this->child_lock_switch_->invalidate();
  if (this->child_lock_available_)
    this->child_lock_available_->publish_state(false);
  if (this->child_lock_status_)
    this->child_lock_status_->publish_state("Panel-lock state unknown; support must be checked");
  this->height_sensor_->publish_state(NAN);
  this->load_raw_valid_ = false;
  if (this->load_raw_sensor_)
    this->load_raw_sensor_->publish_state(NAN);
  for (auto* n : this->preset_numbers_)
    if (n)
      n->publish_state(NAN);
  if (this->connected_sensor_)
    this->connected_sensor_->publish_state(false);
  if (this->frame_errors_sensor_)
    this->frame_errors_sensor_->publish_state(0);
  if (this->sample_gap_sensor_)
    this->sample_gap_sensor_->publish_state(NAN);
  this->gap_window_start_ = millis();
  // Only explicit preset mode activates recovery at boot. Auto must identify
  // the controller first; direct mode never imports the legacy workaround.
  if (this->legacy_backend_ready_())
    this->load_journal_();
  if (this->target_number_)
    this->target_number_->publish_state(NAN);
  this->publish_restore_pending_();
  if (this->timer_pending_sensor_)
    this->timer_pending_sensor_->publish_state(false);
  if (this->stop_locked_sensor_)
    this->stop_locked_sensor_->publish_state(false);
  this->last_keep_awake_ = millis();
  this->subscribe_(millis());
}
void DeskUART::release_(const desk_protocol::Frame& frame, uint32_t now) {
  this->release_ack_.begin(0x18, frame.bytes[1], 0, now);
  this->send_(frame);
}
void DeskUART::loop() {
  this->uart_observation_.tick(millis());
  if (this->firmware_export_active_) {
    this->custom_.disconnect();
    this->custom_beep_deferral_.expire(millis());
    return;
  }
  const uint32_t loop_start = millis();
  if (this->height_seen_ && !this->monitor_.fresh(loop_start))
    this->invalidate_journal_session_();
  desk_protocol::Frame frame;
  // Release has priority even if network/telemetry has gone away.
  if (this->pulse_.poll(loop_start, frame))
    this->release_(frame, loop_start);
  this->timer_.poll_release(loop_start);
  if (this->timer_.take_frame(frame))
    this->send_(frame);
  if (this->parser_.expire(loop_start)) {
    ++this->frame_errors_;
    this->uart_observation_.frame_error();
  }
  // Bound work per loop so an RX flood cannot starve the pulse timer.
  for (unsigned budget = 0; budget < 512 && this->available(); ++budget) {
    uint8_t byte;
    if (!this->read_byte(&byte))
      break;
    const uint32_t now = millis();
    if (this->height_seen_ && static_cast<uint32_t>(now - this->last_height_received_) > 750) {
      this->invalidate_reset_speed_(
          "No recent controller reply; new reset and speed status required");
      this->invalidate_child_lock_("No recent controller reply; panel-lock state unknown");
      this->cancel_motion_trim_("No recent controller reply; trim settings unknown", true);
      this->invalidate_detection_();
      if (this->firmware_export_active_)
        return;
      if (this->direct_mode_) {
        this->direct_.disconnect();
        this->direct_probe_done_ = false;
      }
    }
    if (this->height_seen_ && !this->monitor_.fresh(now))
      this->invalidate_journal_session_();
    if (this->parser_.expire(now)) {
      ++this->frame_errors_;
      this->uart_observation_.frame_error();
    }
    const auto parsed = this->parser_.feed(byte, now, frame);
    if (parsed == desk_protocol::ParseResult::BadChecksum ||
        parsed == desk_protocol::ParseResult::BadLength ||
        parsed == desk_protocol::ParseResult::Overflow) {
      ++this->frame_errors_;
      this->uart_observation_.frame_error();
    }
    if (parsed != desk_protocol::ParseResult::FrameReady)
      continue;
    this->display_frame_(frame, now);
    this->motion_trim_frame_(frame, now);
    if (this->firmware_export_active_) {
      return;
    }
    if (this->auto_target_mode_)
      this->detection_.accept(frame, now);
    if (this->direct_mode_)
      this->direct_.accept(frame, now);
    if (this->child_lock_enabled_()) {
      this->child_lock_detection_.accept(frame, now);
      this->child_lock_.accept(frame, now);
      // Report this reply before a later RX height callback can issue a newer
      // command. Never let an older terminal result overwrite that command.
      this->child_lock_result_();
      if (this->firmware_export_active_)
        return;
    }
    if (this->reset_speed_enabled_()) {
      this->reset_speed_.accept(frame, now);
      this->reset_speed_result_();
      if (this->firmware_export_active_)
        return;
    }
    if (this->custom_.accept(frame, now))
      this->custom_event_();
    if (this->firmware_export_active_)
      return;
    this->identity_frame_(frame, now);
    this->limit_frame_(frame, now);
    this->preset_settings_.accept(frame, now);
    if (this->target_move_.inspecting_recovery() && !this->recovery_access_allowed_(now))
      this->target_move_.abort_recovery(now);
    this->target_move_.accept(frame, now);
    this->timer_.accept(frame, now);
    this->release_ack_.accept(frame, now);
    if (this->stop_ack_.accept(frame, now)) {
      this->stop_acknowledged_ = true;
      ESP_LOGI(TAG, "Brake request acknowledged; physical standstill not confirmed");
    }
    const uint16_t previous_sequence = this->monitor_.height().sequence;
    if (this->monitor_.observe(frame, now)) {
      this->display_observe_height_(this->monitor_.height().sequence);
      if (this->height_seen_ &&
          static_cast<uint16_t>(this->monitor_.height().sequence - previous_sequence) >= 0x8000) {
        this->custom_.disconnect();
        this->invalidate_reset_speed_(
            "Controller status changed; new reset and speed status required");
        this->invalidate_child_lock_("Controller status changed; panel lock must be checked again");
        this->cancel_motion_trim_("Controller restarted; trim settings must be checked", true);
        if (this->firmware_export_active_)
          return;
      }
      this->child_lock_.observe_sequence(this->monitor_.height().sequence);
      this->reset_speed_.observe_sequence(this->monitor_.height().sequence);
      this->motion_trim_.observe_sequence(this->monitor_.height().sequence);
      const uint16_t q = this->monitor_.height().q;
      this->uart_observation_.observe_height(millis(), q, this->monitor_.height().sequence);
      if (this->height_seen_ && static_cast<uint32_t>(now - this->last_height_received_) > 750)
        this->discard_pending_preset_("Pending preset cancelled by telemetry gap; press again");
      if (this->height_seen_ &&
          static_cast<uint32_t>(now - this->last_height_received_) > this->max_sample_gap_)
        this->max_sample_gap_ = now - this->last_height_received_;
      if (!this->height_seen_ || q != this->last_q_) {
        this->last_height_change_ = now;
        this->last_keep_awake_ = now;
        this->discard_pending_preset_("Pending preset cancelled by height change; press again");
      }
      this->last_height_received_ = now;
      this->height_seen_ = true;
      this->last_q_ = q;
      this->target_move_.observe_height(q);
      if (this->direct_mode_)
        this->direct_.observe_q(q);
      const auto value = this->height_profile_.display(q);
      if (this->target_number_ && !this->target_initialized_) {
        this->target_initialized_ = true;
        this->target_number_->publish_state(
            this->display_height(value.before_rounding_tenths_cm / 10.0f));
      }
      // Convert before the metric panel's whole-centimetre truncation above 100 cm.
      const auto tenths =
          this->display_inches_ ? value.before_rounding_tenths_cm : value.displayed_tenths_cm;
      this->height_sensor_->publish_state(this->display_height(tenths / 10.0f));
      // Sensor callbacks can hand the UART to the firmware reader or delay it.
      if (this->firmware_export_active_)
        return;
      if (this->load_raw_sensor_ && this->monitor_.fresh(millis())) {
        this->load_raw_valid_ = true;
        this->load_raw_sensor_->publish_state(this->monitor_.height().load_raw);
        if (this->firmware_export_active_)
          return;
      }
    }
  }
  // publish_state() may synchronously invoke an automation that calls stop().
  // Use a new timestamp so an ACK timer started in that callback cannot appear
  // to have wrapped around relative to the beginning of this loop.
  const uint32_t now = millis();
  // RX processing or synchronous callbacks may cross the connection deadline.
  // Freeze recovery before any transaction can send or complete queued work.
  if (this->height_seen_ && !this->monitor_.fresh(now))
    this->invalidate_journal_session_();
  if (this->release_ack_.expired(now)) {
    this->release_fault_ = true;
    if (this->target_move_.busy())
      this->stop();
    this->status_set_warning();
    ESP_LOGE(
        TAG,
        "No release ACK; further remote presets disabled until ESP restart. Check controller locally.");
  }
  if (this->stop_ack_.expired(now)) {
    this->status_set_warning();
    ESP_LOGE(TAG, "No brake ACK; no retry. Remote presets remain locked. Check desk locally.");
  }
  this->reset_speed_loop_(now);
  if (this->firmware_export_active_)
    return;
  this->detection_loop_(now);
  if (this->firmware_export_active_)
    return;
  this->keep_awake_loop_(now);
  this->target_loop_(now);
  this->direct_loop_(now);
  this->pending_preset_loop_(now);
  this->timer_loop_(now);
  this->child_lock_loop_(now);
  this->display_loop_(millis());
  this->motion_trim_loop_(millis());
  if (this->firmware_export_active_)
    return;
  this->preset_settings_loop_(now);
  this->limit_loop_(now);
  this->controls_loop_(now);
  this->identity_loop_(now);
  this->uart_observation_log_();
  const bool fresh = this->monitor_.fresh(now);
  if (!fresh)
    this->invalidate_load_raw_();
  if (fresh != this->connected_) {
    this->connected_ = fresh;
    if (this->connected_sensor_)
      this->connected_sensor_->publish_state(fresh);
    if (!fresh) {
      this->height_seen_ = false;
      this->height_sensor_->publish_state(NAN);
    }
  }
  // Diagnostics only: link quality, never an input to any interlock.
  if (this->frame_errors_sensor_ && this->frame_errors_ != this->published_frame_errors_ &&
      static_cast<uint32_t>(now - this->last_frame_error_publish_) >= 1000) {
    this->published_frame_errors_ = this->frame_errors_;
    this->last_frame_error_publish_ = now;
    this->frame_errors_sensor_->publish_state(this->frame_errors_);
  }
  if (static_cast<uint32_t>(now - this->gap_window_start_) >= 60000) {
    // Largest interval between accepted samples while connected, last 60 s.
    if (this->sample_gap_sensor_)
      this->sample_gap_sensor_->publish_state(
          this->max_sample_gap_ ? static_cast<float>(this->max_sample_gap_) : NAN);
    this->max_sample_gap_ = 0;
    this->gap_window_start_ = now;
  }
  // Retrying a subscription is not a wake guarantee. Never retry motion or
  // queue a requested movement across loss of communication.
  if (!fresh && !this->timer_.busy() && !this->pulse_.active() && !this->release_ack_.pending() &&
      !this->stop_ack_.pending() && static_cast<uint32_t>(now - this->last_subscribe_) >= 5000)
    this->subscribe_(now);
  // Start experimental discovery only after all existing background work.
  this->custom_loop_(now);
}
bool DeskUART::recall_preset(uint8_t slot) {
  this->cancel_motion_trim_("Trim change cancelled by a movement request");
  if (this->auto_target_mode_ && !this->auto_target_ready_)
    return this->result_(false, "Checking height control support; preset recall unavailable");
  this->yield_reset_speed_read_();
  if (this->custom_write_pending_())
    return this->result_(false, "Custom timer/buzzer transaction pending");
  if (this->reset_speed_blocking_())
    return this->result_(false, this->control_reason_(millis()));
  if (this->direct_mode_ && this->direct_.brake_pending())
    return this->result_(false, "Direct safety brake pending; select preset again after settling");
  if (const char* error = this->journal_error_())
    return this->result_(false, error);
  if (this->firmware_export_active_)
    return this->result_(false, "Firmware read active; desk controls paused");
  const uint32_t now = millis();
  if (this->journal_recheck_pending_)
    return this->result_(false, "Preset backup needs a fresh read after connection loss");
  if (!this->motion_enabled_ || slot < 1 || slot > 3)
    return this->result_(false, "Preset controls disabled or invalid preset");
  this->yield_child_lock_read_();
  if (this->direct_mode_ && this->direct_.awaiting_set() && this->direct_.set_sent()) {
    const bool stopped = this->stop();
    return this->result_(
        false,
        stopped
            ? "Target acceptance uncertain; braking, press the preset again after the desk settles"
            : "Target acceptance uncertain; Stop was not sent, check Control status before pressing a preset");
  }
  this->last_keep_awake_ = now;
  if (this->keep_awake_active_ && !this->keep_awake_fault_ && !this->target_move_.failed() &&
      !this->stop_latched_ && !this->release_fault_ && this->monitor_.fresh(now) &&
      static_cast<uint32_t>(now - this->last_height_received_) <= 750 &&
      this->target_move_.yield_keep_awake(now)) {
    // A yielded dirty transaction still needs the stale-link guard through
    // final readback, including after the pending button has been cancelled.
    if (this->target_move_.dirty())
      this->soft_abort_error_ = "Keep-awake yielded to preset button";
    // Latest button wins, but repeated presses never extend the deadline.
    if (!this->pending_preset_) {
      this->pending_preset_started_ = now;
      this->pending_preset_q_ = this->last_q_;
    }
    this->pending_preset_ = slot;
    this->result_(true, "Preset waiting for keep-awake cleanup (maximum 4 seconds)");
    return this->pending_preset_loop_(now);
  }
  if (this->child_lock_busy_() || this->preset_settings_.busy() || this->timer_.busy() ||
      this->target_move_.busy() || this->limit_phase_ != LimitPhase::Idle || this->stop_latched_ ||
      !this->motion_enabled_ || this->release_fault_ || !this->monitor_.fresh(now) ||
      this->release_ack_.pending() || slot < 1 || slot > 3) {
    return this->result_(false, this->control_reason_(now));
  }
  desk_protocol::Frame press;
  if (!this->pulse_.begin(static_cast<desk_protocol::Button>(slot + 5), now, press))
    return this->result_(false, "Preset button pulse already active");
  this->last_preset_recall_ = now;
  if (this->direct_mode_) {
    // Native preset preempts ownership without a second brake. Keep a canceled
    // read-only probe pending for the normal idle arbiter.
    if (this->direct_.phase() == desk_protocol::DirectTarget::Phase::Probe)
      this->direct_probe_done_ = false;
    this->direct_.cancel();
  }
  this->send_(press);
  return this->result_(true, "Preset button sent");
}
void DeskUART::discard_pending_preset_(const char* reason) {
  if (!this->pending_preset_)
    return;
  this->pending_preset_ = 0;
  this->result_(false, reason);
}
bool DeskUART::pending_preset_loop_(uint32_t now) {
  if (!this->pending_preset_)
    return false;
  if (this->journal_error_() || this->journal_recheck_pending_ || !this->motion_enabled_ ||
      this->firmware_export_active_ || this->stop_latched_ || this->release_fault_ ||
      this->keep_awake_fault_ || this->target_move_.failed() || !this->monitor_.fresh(now) ||
      static_cast<uint32_t>(now - this->last_height_received_) > 750 ||
      this->last_q_ != this->pending_preset_q_ ||
      static_cast<uint32_t>(now - this->pending_preset_started_) >= 4000) {
    this->discard_pending_preset_(
        "Pending preset cancelled by fault, lost telemetry or timeout; press again");
    return false;
  }
  if (this->target_move_.busy() || this->target_move_.dirty() || this->pulse_.active() ||
      this->release_ack_.pending() || this->stop_ack_.pending())
    return true;
  const uint8_t slot = this->pending_preset_;
  this->pending_preset_ = 0;
  this->keep_awake_active_ = false;
  return this->recall_preset(slot);
}
bool DeskUART::stop() {
  if (this->firmware_export_active_)
    return this->result_(false, "Firmware read active; desk controls paused");
  this->cancel_motion_trim_("Trim change cancelled by Stop");
  // Reserve a fail-safe reset cancellation before any callback can issue work.
  const bool reset_cancel_requested = this->reset_speed_.cancel(millis());
  // A rejected reply may have ended the transaction earlier in this RX batch.
  // Its deferred failure must never overwrite this newer Stop result.
  this->child_lock_.take_failure();
  this->child_lock_.take_completed();
  // Do not run child-lock sensor automations until the brake owns the UART.
  // A synchronous maintenance handoff here could otherwise swallow send_().
  if (this->child_lock_busy_())
    this->invalidate_child_lock_(
        "Panel-lock request cancelled by Stop; lock state unknown, request not retried", false,
        false);
  if (this->direct_mode_) {
    // Only an interrupted read-only probe is retried after Stop settles.
    if (this->direct_.phase() == desk_protocol::DirectTarget::Phase::Probe)
      this->direct_probe_done_ = false;
    this->direct_.cancel();
  }
  this->discard_pending_preset_("Pending preset cancelled by Stop");
  // A user's Stop (and any fault brake) must never inherit quiet presentation.
  this->keep_awake_active_ = false;
  this->soft_abort_error_ = nullptr;
  this->last_keep_awake_ = millis();
  // Local synchronous transmission: no subscription, ACK wait or motion queue.
  // Repeated detector callbacks must not continually move the braking target.
  this->preset_settings_.cancel(STOP_PRESET_REASON);
  this->sync_target_ = true;
  const uint32_t now = millis();
  desk_protocol::Frame reset_cancel;
  if (this->reset_speed_.take_frame(reset_cancel)) {
    this->reset_speed_sending_ = true;
    this->send_(reset_cancel);
    this->reset_speed_sending_ = false;
  }
  if (this->accept_current_active_ || this->recovery_check_active_ || this->restore_requested_)
    this->target_move_.abort_recovery(now);
  // Suppress a burst of duplicate requests, not all later explicit Stops.
  // A missing ACK or preset conflict must not permanently disable braking.
  if (this->stop_latched_ &&
      (this->stop_ack_.pending() || static_cast<uint32_t>(now - this->last_stop_) < 2000)) {
    this->publish_child_lock_(millis());
    return this->result_(false, this->control_reason_(now));
  }
  const bool target_failed = this->target_move_.failed();
  this->target_move_.cancel(now);
  if (!target_failed && this->target_move_.failed()) {
    this->last_target_error_ = this->target_move_.error();
    ESP_LOGW(TAG, "%s", this->last_target_error_);
  }
  const char* timer_error = this->timer_.error();
  this->timer_.cancel(now);
  if (this->timer_.error() != timer_error) {
    this->last_timer_error_ = this->timer_.error();
    ESP_LOGW(TAG, "%s", this->last_timer_error_);
  }
  desk_protocol::Frame timer_release;
  if (this->timer_.take_frame(timer_release))
    this->send_(timer_release);
  this->stop_latched_ = true;
  this->stop_acknowledged_ = false;
  this->last_stop_ = now;
  this->reset_stop_pending_ = reset_cancel_requested;
  if (reset_cancel_requested)
    this->stop_ack_ = {};
  else
    this->stop_ack_.begin(0x18, 1, 0, now);
  // A stop never waits behind parameter ACK/readback. Do not queue a write retry.
  if (this->limit_phase_ != LimitPhase::Idle) {
    ESP_LOGW(TAG, "Parameter transaction interrupted by stop; current setting needs refresh");
    this->limit_phase_ = LimitPhase::Idle;
    if (this->limit_numbers_[this->limit_index_])
      this->limit_numbers_[this->limit_index_]->publish_state(NAN);
  }
  this->last_limit_poll_ = now;
  desk_protocol::Frame release;
  const bool cancelled_pulse = this->pulse_.cancel(release);
  if (cancelled_pulse)
    this->release_(release, now);
  this->send_(desk_protocol::stop());
  this->publish_child_lock_(millis());
  if (cancelled_pulse) {
    ESP_LOGW(
        TAG,
        "Released pending preset before brake. Controller may still have a delayed button event; check locally.");
  }
  return this->result_(
      true, "Braking; waiting for controller confirmation, stable height and preset restoration");
}
bool DeskUART::clear_stop_latch() {
  if (this->firmware_export_active_)
    return this->result_(false, "Firmware read active; desk controls paused");
  const uint32_t now = millis();
  if (!this->stop_latched_ || !this->stop_acknowledged_ || this->reset_stop_pending_ ||
      this->stop_ack_.pending() || this->timer_.busy() || this->pulse_.active() ||
      this->release_ack_.pending() || this->release_fault_ || !this->monitor_.fresh(now) ||
      static_cast<uint32_t>(now - this->last_height_received_) > 750 ||
      this->stable_ms_(now) < 1500 || this->target_move_.busy() || this->target_move_.dirty() ||
      static_cast<uint32_t>(now - this->last_stop_) < 2000)
    return false;
  // No motion is sent, and no request is replayed by clearing the interlock.
  this->stop_latched_ = false;
  return this->result_(
      true, this->reset_speed_.blocking()
                ? "Stop completed; reset remains incomplete, see Control status"
            : this->journal_error_()
                ? "Stop completed; preset backup still blocks controls, see control status"
                : "Stop completed; remote controls ready");
}
void LoadLimitNumber::control(float value) {
  if (!std::isfinite(value) || value < 1 || value > 65535 || std::floor(value) != value) {
    ESP_LOGW(TAG, "Load limit must be an integer1..65535 raw units");
    return;
  }
  parent_->set_load_limit(
      index_ == 0 ? desk_protocol::LoadLimit::Upward : desk_protocol::LoadLimit::DownwardBase,
      static_cast<uint16_t>(value));
}
void DeskUART::query_limit_(uint8_t index, uint32_t now, bool verify) {
  desk_protocol::Frame f;
  desk_protocol::read_load_limit(
      index == 0 ? desk_protocol::LoadLimit::Upward : desk_protocol::LoadLimit::DownwardBase, f);
  this->limit_index_ = index;
  this->limit_phase_ = verify ? LimitPhase::Verify : LimitPhase::Read;
  this->limit_started_ = now;
  this->last_limit_poll_ = now;
  this->send_(f);
}
bool DeskUART::set_load_limit(desk_protocol::LoadLimit limit, uint16_t raw) {
  if (this->firmware_export_active_)
    return this->result_(false, "Firmware read active; desk controls paused");
  if (!desk_protocol::valid_load_limit(limit) || raw == 0)
    return false;
  const uint8_t index = limit == desk_protocol::LoadLimit::Upward ? 0 : 1;
  const uint32_t now = millis();
  this->last_keep_awake_ = now;
  if (this->limit_numbers_[index] && this->original_known_[index])
    this->yield_child_lock_read_();
  if (!this->limit_numbers_[index] || !this->original_known_[index] ||
      this->preset_settings_.busy() || !this->preset_access_allowed_(now)) {
    return this->result_(
        false,
        (!this->limit_numbers_[index] || !this->original_known_[index])
            ? "Load setting not configured or original not yet read"
            : "Load edit blocked: moving, recent recall, stale telemetry or another operation; see control status");
  }
  this->limit_index_ = index;
  this->expected_limit_ = raw;
  this->limit_phase_ = LimitPhase::WriteAck;
  this->limit_started_ = now;
  this->last_limit_poll_ = now;
  desk_protocol::Frame f;
  desk_protocol::write_load_limit(limit, raw, f);
  this->send_(f);
  // Do not publish a requested value as if it were a confirmed setting.
  this->limit_numbers_[index]->publish_state(NAN);
  return true;
}
bool DeskUART::restore_load_limit(desk_protocol::LoadLimit limit) {
  if (this->firmware_export_active_)
    return this->result_(false, "Firmware read active; desk controls paused");
  if (!desk_protocol::valid_load_limit(limit))
    return false;
  const uint8_t i = limit == desk_protocol::LoadLimit::Upward ? 0 : 1;
  return this->original_known_[i] && this->set_load_limit(limit, this->original_limits_[i]);
}
void DeskUART::limit_frame_(const desk_protocol::Frame& f, uint32_t now) {
  if (this->limit_phase_ == LimitPhase::Idle ||
      static_cast<uint32_t>(now - this->limit_started_) >= 700)
    return;
  const auto selected = this->limit_index_ == 0 ? desk_protocol::LoadLimit::Upward
                                                : desk_protocol::LoadLimit::DownwardBase;
  if (this->limit_phase_ == LimitPhase::WriteAck) {
    desk_protocol::Ack a;
    if (!desk_protocol::decode_ack(f, a) || a.kind != 0x1A ||
        a.first != static_cast<uint8_t>(selected))
      return;
    if (a.second != 0) {
      this->limit_phase_ = LimitPhase::Idle;
      ESP_LOGE(TAG, "Load-limit write rejected by controller; no retry");
      return;
    }
    this->query_limit_(this->limit_index_, now, true);
    return;
  }
  desk_protocol::LoadLimitValue value;
  if (!desk_protocol::decode_load_limit(f, value) || value.limit != selected)
    return;
  if (this->limit_phase_ == LimitPhase::Verify) {
    if (value.raw == this->expected_limit_)
      ESP_LOGI(TAG, "Load limit%u verified: %u", this->limit_index_, value.raw);
    else
      ESP_LOGE(TAG, "Load limit%u readback mismatch: requested%u actual%u; no retry",
               this->limit_index_, this->expected_limit_, value.raw);
  }
  if (!this->original_known_[this->limit_index_]) {
    this->original_limits_[this->limit_index_] = value.raw;
    this->original_known_[this->limit_index_] = true;
  }
  const uint8_t index = this->limit_index_;
  this->limit_phase_ = LimitPhase::Idle;
  this->next_limit_ = 1 - index;
  if (this->limit_numbers_[index])
    this->limit_numbers_[index]->publish_state(value.raw);
}
void DeskUART::limit_loop_(uint32_t now) {
  if (this->limit_phase_ != LimitPhase::Idle) {
    if (static_cast<uint32_t>(now - this->limit_started_) >= 700) {
      ESP_LOGE(TAG, "Load-limit transaction timeout; no write retry");
      this->limit_phase_ = LimitPhase::Idle;
      this->last_limit_poll_ = now;
      if (this->limit_numbers_[this->limit_index_])
        this->limit_numbers_[this->limit_index_]->publish_state(NAN);
    }
    return;
  }
  if (this->custom_.pending() || this->reset_speed_blocking_() || this->child_lock_busy_() ||
      this->detection_.busy() || this->preset_settings_.busy() || this->timer_.busy() ||
      this->target_move_.busy() || this->direct_.busy() || !this->monitor_.fresh(now) ||
      this->pulse_.active() || this->release_ack_.pending() || this->stop_ack_.pending())
    return;
  for (uint8_t offset = 0; offset < 2; ++offset) {
    const uint8_t i = (this->next_limit_ + offset) % 2;
    if (this->limit_numbers_[i] && static_cast<uint32_t>(now - this->last_limit_poll_) >=
                                       (this->original_known_[i] ? 5000U : 1000U)) {
      this->query_limit_(i, now);
      return;
    }
  }
}
bool DeskUART::preset_access_allowed_(uint32_t now) const {
  return !this->display_.busy() && !this->motion_trim_busy_() && !this->custom_.pending() &&
         !this->reset_speed_blocking_() && !this->child_lock_busy_() && !this->detection_.busy() &&
         !this->journal_error_() && !this->journal_recheck_pending_ && !this->pending_preset_ &&
         !this->firmware_export_active_ && this->monitor_.fresh(now) &&
         this->stable_ms_(now) >= 1500 &&
         static_cast<uint32_t>(now - this->last_height_received_) <= 750 &&
         static_cast<uint32_t>(now - this->last_preset_recall_) >= 3000 &&
         !this->target_move_.busy() && !this->direct_.busy() && !this->timer_.busy() &&
         !this->stop_latched_ && !this->release_fault_ && !this->pulse_.active() &&
         !this->release_ack_.pending() && !this->stop_ack_.pending() &&
         this->limit_phase_ == LimitPhase::Idle;
}
bool DeskUART::set_preset_height(uint8_t slot, float cm) {
  if (this->auto_target_mode_ && !this->auto_target_ready_)
    return this->result_(false, "Checking height control support; preset editing unavailable");
  if (const char* error = this->journal_error_())
    return this->result_(false, error);
  if (this->firmware_export_active_)
    return this->result_(false, "Firmware read active; desk controls paused");
  const uint32_t now = millis();
  this->last_keep_awake_ = now;
  if (slot < 1 || slot > 3 || !this->preset_numbers_[slot - 1] || !std::isfinite(cm) ||
      cm < this->min_height_ || cm > this->max_height_)
    return this->result_(false, "Preset height not configured or outside configured range");
  if (!this->preset_known_[slot - 1])
    return this->result_(false, "Read original preset height before editing");
  this->yield_child_lock_read_();
  if (this->preset_settings_.busy() || !this->preset_access_allowed_(now))
    return this->result_(
        false,
        "Preset edit blocked: moving, recent recall, stale telemetry or another operation; see control status");
  if (slot == 3 && this->cached_journal_valid_)
    return this->result_(
        false,
        "Accept current preset first to discard the retained backup before editing preset 3");
  if (!this->preset_settings_.begin_write(slot, static_cast<uint16_t>(std::lround(cm * 10)), now))
    return false;
  this->preset_known_[slot - 1] = false;
  this->preset_numbers_[slot - 1]->publish_state(NAN);
  this->result_(true, "Preset edit accepted; waiting for scale check and readback, no movement");
  this->preset_settings_loop_(now);
  return true;
}
void DeskUART::preset_settings_loop_(uint32_t now) {
  if (!this->monitor_.fresh(now)) {
    for (uint8_t i = 0; i < 3; ++i) {
      if (this->preset_known_[i] && this->preset_numbers_[i])
        this->preset_numbers_[i]->publish_state(NAN);
      this->preset_known_[i] = false;
    }
  }
  if (this->preset_settings_.busy() &&
      ((!this->auto_target_ready_ && this->auto_target_mode_ && this->preset_settings_.writing()) ||
       !this->preset_access_allowed_(now)))
    this->preset_settings_.cancel(
        "Preset setting interrupted by movement, stale telemetry or another operation; no retry");
  this->preset_settings_.tick(now);
  desk_protocol::PresetSettings::Result r{};
  if (this->preset_settings_.take_result(r)) {
    if (r.slot == 3 && r.value_known && !r.write && !r.error)
      this->check_retained_preset_(r.raw);
    const uint8_t i = r.slot - 1;
    uint16_t tenths;
    this->preset_known_[i] = r.value_known;
    const bool mapped = r.value_known && this->height_profile_.preset_height(r.raw, tenths);
    this->next_preset_ = (i + 1) % 3;
    this->last_preset_poll_ = now;
    if (r.error == STOP_PRESET_REASON)
      ESP_LOGW(TAG, "%s", r.error);
    else if (r.error)
      this->result_(false, r.error);
    else if (!mapped)
      this->result_(
          false,
          "Preset raw value outside supported metric range; enter a valid height to replace it");
    else if (r.write)
      this->result_(true, "Preset height written and verified; no movement");
    // Only actual values from the controller are published, never the requested
    // value. A mismatch displays the returned value and retains an error message.
    if (this->preset_numbers_[i])
      this->preset_numbers_[i]->publish_state(mapped ? this->display_height(tenths / 10.0f) : NAN);
  }
  desk_protocol::Frame frame;
  if (this->preset_settings_.take_frame(frame))
    this->send_(frame);
  if (this->preset_settings_.busy() || !this->preset_access_allowed_(now))
    return;
  for (uint8_t offset = 0; offset < 3; ++offset) {
    const uint8_t i = (this->next_preset_ + offset) % 3;
    if (this->preset_numbers_[i] && static_cast<uint32_t>(now - this->last_preset_poll_) >=
                                        (this->preset_known_[i] ? 10000U : 1000U)) {
      this->last_preset_poll_ = now;
      this->preset_settings_.begin_read(i + 1, now);
      if (this->preset_settings_.take_frame(frame))
        this->send_(frame);
      return;
    }
  }
}
const char* DeskUART::journal_error_() const {
  if (this->journal_active_ && this->journal_write_error_)
    return this->journal_write_error_;
  if (this->journal_block_reason_)
    return this->journal_block_reason_;
  if (this->cached_journal_valid_ &&
      this->cached_journal_.profile != this->height_profile_.identity())
    return "Preset backup belongs to a different height profile; controls blocked";
  return nullptr;
}
bool DeskUART::write_journal_(const desk_protocol::PresetJournal& journal) {
  const auto bytes = journal.encode();
  if (this->write_journal_storage_(this->v4_preference_key_, bytes.data(), bytes.size()))
    return true;
  this->journal_write_error_ =
      "Preset backup storage failed; original retained, restart to recheck storage";
  return false;
}
bool DeskUART::clear_legacy_journal_() {
  const uint8_t empty[8]{};
  if (!this->write_journal_storage_(this->preference_key_, empty, sizeof(empty))) {
    this->journal_write_error_ =
        "Legacy backup cleanup failed; restart to check the retained backup again";
    return false;
  }
  return true;
}
void DeskUART::load_journal_() {
  this->journal_active_ = true;
  this->journal_block_reason_ = nullptr;
  using Journal = desk_protocol::PresetJournal;
  Journal::Encoded bytes{};
  const auto current =
      this->read_journal_storage_(this->v4_preference_key_, bytes.data(), bytes.size());
  if (current == JournalLoad::Invalid || current == JournalLoad::Error ||
      (current == JournalLoad::Found && !Journal::decode(bytes, this->cached_journal_))) {
    this->journal_block_reason_ =
        "Preset backup unreadable or invalid; controls blocked, storage preserved";
    return;
  }
  this->cached_journal_valid_ =
      current == JournalLoad::Found && this->cached_journal_.state == Journal::State::Retained;
  uint8_t legacy_bytes[8]{};
  const auto legacy_status =
      this->read_journal_storage_(this->preference_key_, legacy_bytes, sizeof(legacy_bytes));
  desk_protocol::LegacyPresetJournal legacy;
  const bool legacy_active =
      legacy_status == JournalLoad::Found &&
      desk_protocol::LegacyPresetJournal::decode(legacy_bytes, sizeof(legacy_bytes), legacy);
  const uint8_t empty[8]{};
  const bool legacy_cleared =
      legacy_status == JournalLoad::Found && std::memcmp(legacy_bytes, empty, sizeof(empty)) == 0;
  if (legacy_status == JournalLoad::Error ||
      (!this->cached_journal_valid_ &&
       (legacy_status == JournalLoad::Invalid ||
        (legacy_status == JournalLoad::Found && !legacy_active && !legacy_cleared)))) {
    this->journal_block_reason_ = "Legacy preset backup unreadable or invalid; storage preserved";
    return;
  }
  if (this->cached_journal_valid_) {
    if (this->journal_error_())
      return;
    if (legacy_active && legacy.original != this->cached_journal_.original) {
      this->journal_block_reason_ =
          "Stored preset backups disagree; controls blocked, both originals preserved";
      return;
    }
  } else if (legacy_active) {
    // A resolved v4 marker cannot date a legacy record. It may be leftover
    // cleanup or a newer borrow by older firmware, even with identical values.
    // Preserve it as a retained backup and run the usual recovery checks.
    this->cached_journal_.state = Journal::State::Retained;
    this->cached_journal_.original = legacy.original;
    this->cached_journal_.temporary = legacy.temporary;
    this->cached_journal_.profile = desk_protocol::LegacyPresetJournal::profile_identity();
    this->cached_journal_valid_ = true;
    if (this->journal_error_() || !this->write_journal_(this->cached_journal_))
      return;
    // Only explicit acceptance may clear the legacy original.
  }
  if (!this->cached_journal_valid_)
    return;
  this->target_move_.recover(this->cached_journal_.original, this->cached_journal_.temporary);
  this->keep_awake_recovery_ = true;
  this->last_target_error_ = this->target_move_.error();
  ESP_LOGD(TAG, "Checking profile-bound preset backup after startup; EEPROM unverified");
}
bool DeskUART::clear_cached_journal_() {
  if (this->journal_error_())
    return false;
  if (!this->cached_journal_valid_)
    return true;
  // Reject an unreadable or wrong-sized legacy key before replacing the
  // retained v4 record with a resolved marker.
  uint8_t legacy_bytes[8]{};
  const auto legacy =
      this->read_journal_storage_(this->preference_key_, legacy_bytes, sizeof(legacy_bytes));
  if (legacy != JournalLoad::Missing && legacy != JournalLoad::Found) {
    this->journal_block_reason_ = "Legacy preset backup unreadable or invalid; storage preserved";
    return false;
  }
  auto resolved = this->cached_journal_;
  resolved.state = desk_protocol::PresetJournal::State::Resolved;
  if (!this->write_journal_(resolved) || !this->clear_legacy_journal_())
    return false;
  // Keep the RAM original and report failure until both writes are verified.
  // If cleanup was interrupted, startup retains the active legacy record again.
  this->cached_journal_valid_ = false;
  this->cached_journal_ = resolved;
  return true;
}
void DeskUART::invalidate_journal_session_() {
  if (!this->journal_active_ || !this->cached_journal_valid_ || this->journal_recheck_pending_)
    return;
  this->verified_journal_.valid = false;
  this->journal_recheck_pending_ = true;
  this->target_move_.connection_lost(millis());
  this->accept_current_active_ = this->restore_requested_ = this->recovery_check_active_ = false;
  this->keep_awake_recovery_ = false;
  this->discard_pending_preset_("Pending preset cancelled by connection loss; press again");
}
void DeskUART::check_retained_preset_(uint16_t preset3_raw) {
  if (!this->journal_active_ || !this->legacy_backend_ready_() || this->journal_error_())
    return;
  if (!this->cached_journal_valid_ || preset3_raw == this->cached_journal_.original ||
      this->target_move_.busy())
    return;
  if (!this->legacy_ownership_active_())
    this->legacy_transaction_started_ = false;
  // A controller reboot and a legitimate local save are indistinguishable.
  // Keep the baseline and require a decision instead of deleting it.
  this->target_move_.recover(
      this->cached_journal_.original, this->cached_journal_.temporary,
      "Preset 3 differs from retained backup; restore backup or accept current preset");
  this->verified_journal_.valid = false;
  this->keep_awake_recovery_ = false;
  ESP_LOGW(
      TAG,
      "Preset 3 differs from retained backup: current=%u original=%u; restore or accept explicitly",
      preset3_raw, this->cached_journal_.original);
}
bool DeskUART::save_journal_(bool active) {
  if (!this->journal_active_ || this->journal_error_())
    return false;
  this->verified_journal_.valid = false;
  if (!active) {
    // Completing a RAM transaction cannot establish EEPROM persistence.
    // Only explicit, double-read acceptance discards the retained baseline.
    return !this->accept_current_active_ || this->clear_cached_journal_();
  }
  if (this->cached_journal_valid_ &&
      (this->journal_recheck_pending_ ||
       this->cached_journal_.original != this->target_move_.original())) {
    this->target_move_.recover(
        this->cached_journal_.original, this->cached_journal_.temporary,
        "Preset 3 differs from retained backup; restore backup or accept current preset");
    this->keep_awake_recovery_ = false;
    ESP_LOGW(TAG, "Retained preset backup conflicts with new borrow; no write sent");
    return false;
  }
  desk_protocol::PresetJournal journal;
  journal.state = desk_protocol::PresetJournal::State::Retained;
  journal.profile = this->height_profile_.identity();
  journal.original = this->target_move_.original();
  journal.temporary = this->target_move_.target();
  if (this->cached_journal_valid_ && this->cached_journal_.profile == journal.profile &&
      this->cached_journal_.original == journal.original &&
      this->cached_journal_.temporary == journal.temporary)
    return true;
  // Preserve the new original in RAM even if durable storage fails before UART write.
  this->cached_journal_ = journal;
  this->cached_journal_valid_ = true;
  if (!this->write_journal_(journal))
    return false;
  ESP_LOGD(TAG, "Profile-bound preset recovery journal committed");
  return true;
}
void DeskUART::keep_awake_loop_(uint32_t now) {
  // A real borrow retains its stale-height abort and automatic recheck even
  // while auto is Unknown. Dormant journals still wait for classification.
  if (!this->journal_active_ ||
      (!this->legacy_backend_ready_() && !this->legacy_ownership_active_()))
    return;
  if (this->journal_error_())
    return;
  using Phase = desk_protocol::TemporaryPreset::Phase;
  // Wait for release/braking and fresh settled height before inspecting a
  // retained record. Never replace a running motion state during RX handling.
  if (this->journal_recheck_pending_ && this->cached_journal_valid_ &&
      !this->target_move_.brake_pending() && this->recovery_access_allowed_(now) &&
      (!this->target_move_.busy() || this->target_move_.phase() == Phase::Fault ||
       this->target_move_.phase() == Phase::Settle)) {
    this->target_move_.recover(this->cached_journal_.original, this->cached_journal_.temporary);
    this->journal_recheck_pending_ = false;
    this->keep_awake_active_ = false;
    this->soft_abort_error_ = nullptr;
    this->keep_awake_recovery_ = true;
  }
  if (this->keep_awake_recovery_ && this->recovery_access_allowed_(now)) {
    this->keep_awake_recovery_ = false;
    this->recovery_check_active_ = this->target_move_.check_recovery(now);
  }
  if (this->direct_mode_)
    return;  // Direct control never borrows a preset.
  if (this->keep_awake_active_) {
    const Phase phase = this->target_move_.phase();
    const bool before_or_during_recall = phase == Phase::CurrentRaw || phase == Phase::Original ||
                                         phase == Phase::Backup || phase == Phase::Write ||
                                         phase == Phase::Verify || phase == Phase::CheckRaw ||
                                         phase == Phase::Recall || phase == Phase::Moving;
    if (before_or_during_recall &&
        (this->last_q_ != this->keep_awake_q_ || !this->monitor_.fresh(now) ||
         static_cast<uint32_t>(now - this->last_height_received_) > 750)) {
      // The press, not the phase name, decides whether motion may have begun.
      this->target_move_.abort_keep_awake(now, this->keep_awake_pressed_);
    }
    return;
  }
  if (!this->legacy_backend_ready_() || !this->target_detected_() ||
      this->journal_recheck_pending_ || this->pending_preset_ || !this->keep_awake_enabled_ ||
      !this->target_enabled_ || this->keep_awake_fault_ || this->preset_settings_.busy() ||
      !this->preset_access_allowed_(now) || this->custom_beep_deferral_.active(now) ||
      this->custom_alarm_deferral_.active(now) ||
      static_cast<uint32_t>(now - this->last_keep_awake_) < 30000)
    return;
  const auto height =
      this->height_profile_.display(this->last_q_).before_rounding_tenths_cm / 10.0f;
  if (height < this->min_height_ || height > this->max_height_)
    return;
  if (!this->target_move_.begin_keep_awake(now))
    return;
  this->legacy_transaction_started_ = true;
  this->keep_awake_active_ = true;
  this->keep_awake_pressed_ = false;
  this->keep_awake_q_ = this->last_q_;
  this->last_keep_awake_ = now;
  this->last_target_error_ = nullptr;
  ESP_LOGD(TAG, "Keep-awake: reading current raw position and borrowing preset 3");
}
bool DeskUART::move_to_height(float cm) {
  this->cancel_motion_trim_("Trim change cancelled by a movement request");
  this->yield_reset_speed_read_();
  if (this->custom_write_pending_())
    return this->result_(false, "Custom timer/buzzer transaction pending");
  if (this->reset_speed_blocking_())
    return this->result_(false, this->control_reason_(millis()));
  if (const char* error = this->journal_error_())
    return this->result_(false, error);
  if (this->firmware_export_active_)
    return this->result_(false, "Firmware read active; desk controls paused");
  if (this->target_enabled_ && std::isfinite(cm) && cm >= this->min_height_ &&
      cm <= this->max_height_)
    this->yield_child_lock_read_();
  const uint32_t now = millis();
  if (this->auto_target_mode_ &&
      (!this->height_seen_ || static_cast<uint32_t>(now - this->last_height_received_) > 750))
    this->invalidate_detection_();
  if (!this->target_detected_())
    return this->result_(
        false, this->detection_.error()
                   ? this->detection_.error()
                   : "Checking height control support; select the target again when Ready");
  if (this->direct_mode_)
    return this->direct_move_(cm, now);
  this->last_keep_awake_ = now;
  if (!this->target_enabled_ || !std::isfinite(cm) || cm < this->min_height_ ||
      cm > this->max_height_)
    return this->result_(false, "Target outside configured range or target control disabled");
  if (this->journal_recheck_pending_)
    return this->result_(false, "Retained preset backup needs a fresh read after connection loss");
  if (this->keep_awake_active_)
    return this->result_(
        false, "Keep-awake preset transaction in progress; request again after restoration");
  // A different target during an active move is only a Stop request.
  // A new move requires a fresh request after braking and preset restoration.
  if (this->child_lock_busy_() || this->preset_settings_.busy() || this->release_fault_ ||
      !this->monitor_.fresh(now) || this->timer_.busy())
    return this->result_(false, this->control_reason_(now));
  using Phase = desk_protocol::TemporaryPreset::Phase;
  if ((this->target_move_.phase() == Phase::Moving ||
       this->target_move_.phase() == Phase::Monitoring) &&
      !this->stop_latched_) {
    // Identical HA updates must not interrupt an otherwise valid move.
    uint16_t raw;
    if (!this->height_profile_.target_raw(static_cast<uint16_t>(std::lround(cm * 10)), raw))
      return this->result_(false, "Target outside supported height profile");
    if (raw == this->target_move_.target())
      return this->result_(true, "Target already active");
    const bool stopped = this->stop();
    return this->result_(
        false, stopped ? "Stopped; select target again after the desk settles"
                       : "Stop request not sent; inspect Control status before another target");
  }
  if (this->stop_latched_ || this->stable_ms_(now) < 1500 || this->pulse_.active() ||
      this->release_ack_.pending() || this->stop_ack_.pending() ||
      this->limit_phase_ != LimitPhase::Idle || this->target_move_.busy())
    return this->result_(false, this->control_reason_(now));
  const auto tenths = static_cast<uint16_t>(std::lround(cm * 10.0f));
  if (!this->target_move_.begin(tenths, now))
    return this->result_(false, "Target transaction refused");
  this->legacy_transaction_started_ = true;
  this->last_target_error_ = nullptr;
  this->sync_target_ = true;
  ESP_LOGI(TAG, "Requested target %.1f cm; preserving preset 3", tenths / 10.0f);
  this->target_loop_(now);
  return this->result_(true, "Target accepted");
}

void DeskUART::invalidate_detection_() {
  if (!this->auto_target_mode_)
    return;
  this->detection_.reset();
  this->auto_target_ready_ = false;
  this->detection_cancelled_at_ = millis();
  if (!this->legacy_ownership_active_())
    this->unload_journal_();
  this->discard_pending_preset_("Pending preset cancelled; firmware detection required");
  // Keep ownership of the old backend until its brake/restore has completed.
}
const char* DeskUART::target_backend_name_() const {
  if (!this->target_enabled_)
    return "Disabled";
  if (!this->target_detected_())
    return "Unknown";
  return this->direct_mode_ ? "Direct height control" : "Height control using preset 3";
}
void DeskUART::detection_loop_(uint32_t now) {
  if (this->auto_target_mode_) {
    const bool fresh = this->height_seen_ && this->monitor_.fresh(now) &&
                       static_cast<uint32_t>(now - this->last_height_received_) <= 750;
    if (!fresh)
      this->invalidate_detection_();
    else {
      const bool idle = this->discovery_access_allowed_(now) && !this->direct_.busy();
      if (this->detection_.busy() && !idle)
        this->invalidate_detection_();
      if (idle) {
        if (this->detection_.idle() && !this->available() &&
            static_cast<uint32_t>(now - this->detection_cancelled_at_) >= 800) {
          this->detection_.seed(this->direct_.next_nonce());
          this->detection_.start(now, this->monitor_.height().sequence);
        }
        this->detection_.tick(now);
        desk_protocol::Frame request;
        if (!this->available() && this->detection_.take_frame(request)) {
          this->direct_.seed(this->detection_.next_nonce());
          this->detection_sending_ = true;
          this->send_(request);
          this->detection_sending_ = false;
        }
        if (this->detection_.backend() != desk_protocol::TargetDetection::Backend::Unknown) {
          const bool direct =
              this->detection_.backend() == desk_protocol::TargetDetection::Backend::Direct;
          // A real legacy transaction may have retained RAM ownership through
          // invalidation. Drop its now-dormant state only after cleanup ends.
          if (direct) {
            if (this->journal_active_)
              this->unload_journal_();
            // A legacy keep-awake failure remains sticky across Unknown/stock,
            // but cannot gate a newly confirmed direct backend.
            this->keep_awake_fault_ = false;
            this->soft_abort_error_ = this->last_target_error_ = nullptr;
          }
          if (direct != this->direct_mode_) {
            this->direct_mode_ = direct;
            this->direct_.disconnect();
            this->direct_.cancel();
            this->direct_probe_done_ = false;
          }
          this->auto_target_ready_ = true;
          if (!direct && !this->journal_active_)
            this->load_journal_();
          this->publish_restore_pending_();
        }
      }
    }
  }
  const char* name = this->target_backend_name_();
  if (name != this->last_target_backend_) {
    this->last_target_backend_ = name;
    ESP_LOGI(TAG, "Height control method: %s", name);
    if (this->target_backend_sensor_)
      this->target_backend_sensor_->publish_state(name);
  }
}
bool DeskUART::legacy_ownership_active_() const {
  using Phase = desk_protocol::TemporaryPreset::Phase;
  const auto phase = this->target_move_.phase();
  return this->target_move_.brake_pending() ||
         (this->target_move_.busy() && phase != Phase::Fault && phase != Phase::RecoveryRead) ||
         (this->legacy_transaction_started_ && this->target_move_.dirty());
}
void DeskUART::unload_journal_() {
  // This is RAM-only. Never discard an original or resolve storage because a
  // newly identified controller can use direct commands.
  if (this->legacy_ownership_active_())
    return;
  this->journal_active_ = false;
  this->cached_journal_valid_ = false;
  this->cached_journal_ = {};
  this->journal_block_reason_ = nullptr;
  this->verified_journal_.valid = false;
  this->journal_recheck_pending_ = false;
  this->keep_awake_recovery_ = this->keep_awake_active_ = false;
  this->recovery_check_active_ = this->accept_current_active_ = this->restore_requested_ = false;
  this->legacy_transaction_started_ = false;
  if (!this->keep_awake_fault_)
    this->soft_abort_error_ = this->last_target_error_ = nullptr;
  this->target_move_ = desk_protocol::TemporaryPreset(this->height_profile_);
}
void DeskUART::publish_restore_pending_() {
  if (this->auto_target_mode_ && !this->auto_target_ready_ && !this->legacy_ownership_active_()) {
    if (this->restore_pending_sensor_ && this->restore_pending_sensor_->has_state())
      this->restore_pending_sensor_->invalidate_state();
    return;
  }
  const bool quiet_restore = this->keep_awake_active_ && this->target_move_.keep_awake() &&
                             !this->target_move_.failed() && !this->keep_awake_fault_ &&
                             !this->release_fault_ && !this->stop_latched_ &&
                             this->monitor_.fresh(millis());
  this->last_restore_pending_ =
      this->journal_error_() || (this->target_move_.dirty() && !quiet_restore);
  if (this->restore_pending_sensor_ &&
      (!this->restore_pending_sensor_->has_state() ||
       this->restore_pending_sensor_->state != this->last_restore_pending_))
    this->restore_pending_sensor_->publish_state(this->last_restore_pending_);
}

bool DeskUART::discovery_access_allowed_(uint32_t now) const {
  // Read-only capability discovery must not depend on a dormant journal, its
  // load errors, or whether a target-height number is exposed. A real borrow
  // still owns the bus until its write/brake/cleanup is resolved.
  return !this->display_.busy() && !this->motion_trim_busy_() && !this->custom_.pending() &&
         !this->reset_speed_blocking_() && !this->child_lock_busy_() &&
         !this->legacy_ownership_active_() && !this->keep_awake_active_ &&
         !this->firmware_export_active_ && this->monitor_.fresh(now) && this->height_seen_ &&
         static_cast<uint32_t>(now - this->last_height_received_) <= 750 &&
         this->stable_ms_(now) >= 1500 && !this->stop_latched_ && !this->release_fault_ &&
         !this->timer_.busy() && !this->preset_settings_.busy() && !this->pulse_.active() &&
         !this->release_ack_.pending() && !this->stop_ack_.pending() && !this->pending_preset_ &&
         this->limit_phase_ == LimitPhase::Idle && this->identity_pending_ == 255 &&
         static_cast<uint32_t>(now - this->last_preset_recall_) >= 3000;
}
bool DeskUART::direct_access_allowed_(uint32_t now) const {
  return !this->display_.busy() && !this->motion_trim_busy_() && !this->custom_.pending() &&
         !this->reset_speed_blocking_() && !this->child_lock_busy_() && this->target_enabled_ &&
         !this->journal_error_() && !this->journal_recheck_pending_ &&
         !this->accept_current_active_ && !this->recovery_check_active_ &&
         !this->restore_requested_ && !this->target_move_.dirty() && !this->target_move_.busy() &&
         !this->firmware_export_active_ && this->monitor_.fresh(now) && this->height_seen_ &&
         static_cast<uint32_t>(now - this->last_height_received_) <= 750 &&
         this->stable_ms_(now) >= 1500 && !this->stop_latched_ && !this->release_fault_ &&
         !this->timer_.busy() && !this->preset_settings_.busy() && !this->pulse_.active() &&
         !this->release_ack_.pending() && !this->stop_ack_.pending() && !this->pending_preset_ &&
         this->limit_phase_ == LimitPhase::Idle && this->identity_pending_ == 255 &&
         static_cast<uint32_t>(now - this->last_preset_recall_) >= 3000;
}
bool DeskUART::direct_move_(float cm, uint32_t now) {
  uint16_t raw;
  if (!this->target_enabled_ || !std::isfinite(cm) || cm < this->min_height_ ||
      cm > this->max_height_ ||
      !this->height_profile_.target_raw(static_cast<uint16_t>(std::lround(cm * 10)), raw))
    return this->result_(false, "Target outside configured range or direct control disabled");
  if (this->direct_.phase() == desk_protocol::DirectTarget::Phase::Probe)
    return this->result_(
        false, "Checking direct height control support; select the target again when Ready");
  if (this->direct_.active()) {
    if (raw == this->direct_.requested())
      return this->result_(true, "Direct target already active");
    const bool stopped = this->stop();
    return this->result_(
        false, stopped ? "Stopped; select target again after the desk settles"
                       : "Stop request not sent; inspect Control status before another target");
  }
  if (this->direct_.waiting()) {
    const bool uncertain = this->direct_.set_sent();
    const bool stopped = uncertain && this->stop();
    if (!uncertain)
      this->direct_.cancel();
    return this->result_(
        false,
        uncertain
            ? (stopped
                   ? "Target acceptance uncertain; braking, select the target again after the desk settles"
                   : "Target acceptance uncertain; check Stop status before selecting another target")
            : "Pending direct request cancelled; select target again");
  }
  if (!this->direct_access_allowed_(now))
    return this->result_(false, this->control_reason_(now));
  int8_t up_trim = 0;
  int8_t down_trim = 0;
  if (this->motion_trim_.supported()) {
    if (!this->motion_trim_.state_known() || this->motion_trim_.reply().flags ||
        !this->reset_speed_.fresh(now)) {
      return this->result_(false, "Trim settings or motion speed unknown; target not sent");
    }
    const uint8_t index = 2 * this->reset_speed_.reply().speed;
    up_trim = this->motion_trim_.reply().active.trim[index];
    down_trim = this->motion_trim_.reply().active.trim[index + 1];
  }
  if (!this->direct_.begin(raw, this->last_q_, now, up_trim, down_trim))
    return this->result_(false, "Direct request blocked");
  this->direct_probe_done_ = true;
  this->last_direct_error_ = nullptr;
  this->direct_loop_(now);
  return this->result_(
      true,
      "Target requested; checking direct height control support and whether the desk is idle");
}

bool DeskUART::reset_speed_access_allowed_(uint32_t now, bool reset, bool read,
                                           bool presentation) const {
  const bool recovery_read = read && this->reset_stop_pending_;
  // Only availability publication skips read-only bus occupancy. Command
  // admission and polling always use the default, fully serialized path.
  return !this->display_.busy() && !this->motion_trim_busy_() && !this->custom_write_pending_() &&
         (!this->custom_.pending() || read || presentation) && !this->firmware_export_active_ &&
         (!this->reset_speed_.busy() ||
          (presentation && this->reset_speed_.operation() == desk_protocol::ResetSpeed::Get)) &&
         !this->detection_.busy() &&
         (!this->child_lock_busy_() ||
          (presentation && !this->child_lock_detection_.busy() &&
           this->child_lock_.operation() == 0 && this->child_lock_.state_known())) &&
         (recovery_read ||
          (!this->journal_error_() && !this->journal_recheck_pending_ &&
           !this->keep_awake_active_ && !this->keep_awake_recovery_ && !this->keep_awake_fault_ &&
           !this->accept_current_active_ && !this->recovery_check_active_ &&
           !this->restore_requested_ && !this->target_move_.dirty())) &&
         !this->target_move_.busy() && !this->direct_.busy() && this->height_seen_ &&
         this->monitor_.fresh(now) &&
         static_cast<uint32_t>(now - this->last_height_received_) <= 750 &&
         // The fresh D6 carries all CL01 inhibit bits. A background D4 read may
         // yield here, so its older cached state cannot veto this newer proof.
         (!reset || (this->reset_speed_.known() && !this->reset_speed_.reply().child_lock)) &&
         this->stable_ms_(now) >= 1500 && (!this->stop_latched_ || recovery_read) &&
         !this->release_fault_ && !this->timer_.busy() &&
         (!this->preset_settings_.busy() || (presentation && !this->preset_settings_.writing())) &&
         !this->pulse_.active() && !this->release_ack_.pending() && !this->stop_ack_.pending() &&
         !this->pending_preset_ &&
         (this->limit_phase_ == LimitPhase::Idle ||
          (presentation && this->limit_phase_ == LimitPhase::Read)) &&
         (this->identity_pending_ == 255 || presentation) &&
         static_cast<uint32_t>(now - this->last_preset_recall_) >= 3000;
}
void DeskUART::yield_reset_speed_read_() {
  this->yield_display_read_();
  this->yield_custom_probe_();
  if (this->reset_speed_.busy() &&
      this->reset_speed_.operation() == desk_protocol::ResetSpeed::Get) {
    this->reset_speed_.yield_read();
    this->last_reset_speed_query_ = millis();
  }
}
void DeskUART::invalidate_reset_speed_(const char* reason) {
  if (!this->reset_speed_enabled_())
    return;
  this->reset_speed_.disconnect(reason);
  this->last_reset_speed_query_ = millis();
  if (this->motion_speed_select_)
    this->motion_speed_select_->invalidate();
}
bool DeskUART::reset_desk(bool resume) {
  this->cancel_motion_trim_("Trim change cancelled by a reset request");
  this->yield_child_lock_read_();
  const uint32_t now = millis();
  // The same user command resumes a paused reset. Cached status cannot choose
  // an operation, and begin() independently checks fresh controller admission.
  const bool continue_reset =
      resume || (this->reset_speed_.fresh(now) && this->reset_speed_.reply().state == 3);
  if (!this->reset_enabled_ || !this->reset_speed_access_allowed_(now, true) ||
      !this->reset_speed_.begin(continue_reset, now)) {
    const auto& reply = this->reset_speed_.reply();
    return this->result_(
        false,
        this->reset_enabled_ && this->reset_speed_.fresh(now) && reply.state == 3 &&
                !(reply.flags & 2)
            ? this->reset_speed_status_(now)
            : "Reset unavailable; controller must confirm it is ready, panel must be unlocked and controls idle");
  }
  this->last_keep_awake_ = now;
  desk_protocol::Frame frame;
  if (this->reset_speed_.take_frame(frame)) {
    this->reset_speed_sending_ = true;
    this->send_(frame);
    this->reset_speed_sending_ = false;
  }
  this->result_(true, continue_reset
                          ? "Reset continuation requested; waiting for controller confirmation"
                          : "Reset requested; waiting for controller confirmation");
  this->publish_reset_speed_(millis());
  return true;
}
bool DeskUART::set_motion_speed(bool slow) {
  this->yield_child_lock_read_();
  const uint32_t now = millis();
  if (!this->motion_speed_select_ || !this->reset_speed_access_allowed_(now) ||
      !this->reset_speed_.set_speed(slow, now))
    return this->result_(
        false, "Speed control unavailable; needs current controller status and idle controls");
  this->last_keep_awake_ = now;
  desk_protocol::Frame frame;
  if (this->reset_speed_.take_frame(frame)) {
    this->reset_speed_sending_ = true;
    this->send_(frame);
    this->reset_speed_sending_ = false;
  }
  this->result_(true, "Motion speed requested; awaiting confirmed controller state");
  this->publish_reset_speed_(millis());
  return true;
}
void DeskUART::reset_speed_result_() {
  const char* failure = this->reset_speed_.take_failure();
  const bool completed = this->reset_speed_.take_completed();
  const auto& reply = this->reset_speed_.reply();
  const uint8_t operation = this->reset_speed_.operation();
  if (completed && operation == desk_protocol::ResetSpeed::Get && !reply.result &&
      this->motion_trim_rs_replies_ < 2) {
    ++this->motion_trim_rs_replies_;
  }
  if (this->motion_trim_token_pending_ && (completed || failure)) {
    this->motion_trim_token_pending_ = false;
    const bool admitted =
        completed && !failure && !reply.result && reply.outputs_off() && !reply.mode &&
        !reply.previous && (reply.state == 0 || reply.state == 4) &&
        this->child_lock_.state_known() && !this->child_lock_.reply().storage_fault;
    if (!admitted || !this->motion_trim_.provide_token(reply.token, reply.sequence)) {
      this->cancel_motion_trim_("Trim change blocked by the controller; try again when idle");
      this->result_(false, "Trim change blocked by the controller; change not queued");
    }
  }
  if (completed) {
    this->reset_speed_confirmed_ = true;
    ESP_LOGD(
        TAG,
        "RS01 reply op=%u result=%u state=%u reason=%u flags=0x%02x mode=%u previous=%u lock=0x%02x "
        "stock_891=%u stock_8b9=%u error=%u stock_88e=%u position=0x%08" PRIx32
        " pwm=%u lease=%u sequence=%u tick=%" PRIu32
        " admission_diagnostics=%d start_result=%u resume_result=%u",
        operation, reply.result, reply.state, reply.reason, reply.flags, reply.mode, reply.previous,
        reply.child_lock, reply.fault_a, reply.fault_b, reply.error, reply.direction,
        reply.position_bits, reply.pwm, reply.lease, reply.sequence, reply.tick,
        reply.admission_diagnostics, reply.start_result, reply.resume_result);
  }
  // A post-Stop GET can independently prove cancellation when its ACK was
  // lost with telemetry. Stop cancelled all older GETs, so this nonce-bound
  // proof is necessarily newer. It clears only the reset-specific ACK wait.
  if (completed && this->reset_stop_pending_ &&
      (operation == desk_protocol::ResetSpeed::Cancel ||
       operation == desk_protocol::ResetSpeed::Get) &&
      !reply.result && reply.outputs_off() && !reply.lease_active() && !reply.active()) {
    this->reset_stop_pending_ = false;
    this->stop_acknowledged_ = true;
    this->stop_ack_ = {};
  }
  if (failure)
    this->result_(false, failure);
  else if (completed && operation == desk_protocol::ResetSpeed::Renew && reply.state == 4 &&
           reply.outputs_off() && !reply.lease_active() && !this->reset_speed_.error()) {
    // Calibration can clear controller inhibits cached by the startup GET.
    // The normal idle arbiter will refresh them with a read-only DT02 probe.
    this->direct_probe_done_ = false;
    this->result_(true, "Reset completed; controller reports outputs off");
  } else if (completed && operation != desk_protocol::ResetSpeed::Get &&
             operation != desk_protocol::ResetSpeed::Renew) {
    if (reply.result)
      this->result_(false, "Reset or speed request rejected; not retried");
    else if (operation == desk_protocol::ResetSpeed::Speed)
      this->result_(true, reply.speed ? "Slow speed confirmed; runtime setting"
                                      : "Normal speed confirmed; runtime setting");
    else if (operation == desk_protocol::ResetSpeed::Cancel)
      this->result_(true,
                    reply.state == 5 ? "Reset cancellation confirmed; power-cycle the controller"
                    : reply.state == 3 && !(reply.flags & 2)
                        ? reset_continuation_blocked(reply)
                        : "Reset cancellation confirmed; see Control status before continuing");
    else
      this->result_(true,
                    "Reset admitted; Stop pauses, completion requires explicit controller state");
  }
}
void DeskUART::publish_reset_speed_(uint32_t now) {
  if (!this->reset_speed_enabled_())
    return;
  const bool link_fresh = !this->firmware_export_active_ && this->height_seen_ &&
                          this->monitor_.fresh(now) &&
                          static_cast<uint32_t>(now - this->last_height_received_) <= 750;
  const bool fresh = link_fresh && this->reset_speed_.fresh(now);
  const auto& reply = this->reset_speed_.reply();
  if (this->motion_speed_select_) {
    // Normal movement suspends RS01 polling but cannot change the confirmed
    // runtime speed. Keep that value while telemetry remains fresh. Admission
    // still requires a fresh RS01 reply; loss or an unconfirmed SET is unknown.
    if (!link_fresh || !this->reset_speed_.known() ||
        (this->reset_speed_.busy() &&
         this->reset_speed_.operation() == desk_protocol::ResetSpeed::Speed))
      this->motion_speed_select_->invalidate();
    else {
      const size_t index = reply.speed;
      if (!this->motion_speed_select_->has_state() ||
          this->motion_speed_select_->active_index().value() != index)
        this->motion_speed_select_->publish_state(index);
    }
  }
  // Publication callbacks may synchronously call Stop or begin maintenance.
  const uint32_t current = millis();
  const uint8_t reset_eligibility = reply.state == 3 ? 2 : 1;
  const bool available = fresh && this->reset_enabled_ && this->reset_speed_.fresh(current) &&
                         this->reset_speed_access_allowed_(current, true, false, true) &&
                         (reply.flags & reset_eligibility) && !reply.child_lock && reply.token;
  const bool speed_available = fresh && this->reset_speed_.fresh(current) &&
                               this->motion_speed_select_ &&
                               this->reset_speed_access_allowed_(current, false, false, true) &&
                               !this->reset_speed_.blocking() && reply.token &&
                               reply.outputs_off() && !reply.mode && !reply.previous;
  if (available != this->last_reset_available_) {
    this->last_reset_available_ = available;
    if (this->reset_available_)
      this->reset_available_->publish_state(available);
  }
  if (speed_available != this->last_speed_available_) {
    this->last_speed_available_ = speed_available;
    if (this->motion_speed_available_)
      this->motion_speed_available_->publish_state(speed_available);
  }
  const char* status = this->reset_speed_status_(now);
  if (status != this->last_reset_status_) {
    this->last_reset_status_ = status;
    if (this->reset_status_)
      this->reset_status_->publish_state(status);
  }
}
const char* DeskUART::reset_speed_status_(uint32_t now) const {
  const bool fresh = !this->firmware_export_active_ && this->height_seen_ &&
                     this->monitor_.fresh(now) &&
                     static_cast<uint32_t>(now - this->last_height_received_) <= 750 &&
                     this->reset_speed_.fresh(now);
  const auto& reply = this->reset_speed_.reply();
  const char* status = this->reset_speed_.error();
  if (fresh && reply.state == 5)
    status = "Reset fault; power-cycle the controller";
  else if (fresh && reply.state == 3 && !(reply.flags & 2))
    status = reset_continuation_blocked(reply);
  if (!status) {
    if (!fresh)
      status = "Reset and speed status unknown; waiting for a new controller reply";
    else if (this->reset_speed_.busy() &&
             this->reset_speed_.operation() != desk_protocol::ResetSpeed::Get &&
             this->reset_speed_.operation() != desk_protocol::ResetSpeed::Renew)
      status = "Waiting for the controller to confirm the reset or speed request";
    else if (reply.state == 1)
      status = this->reset_speed_.owned() ? "Reset descending; Stop pauses"
                                          : "Reset descending; ESPHome control not confirmed";
    else if (reply.state == 2)
      status = this->reset_speed_.owned() ? "Reset rebound; Stop pauses"
                                          : "Reset rebound; ESPHome control not confirmed";
    else if (reply.state == 3)
      status = "Reset paused; select Reset to continue";
    else if (reply.state == 4)
      status = "Reset completed; controller reports outputs off";
    else if (reply.state == 5)
      status = "Reset fault; power-cycle the controller";
    else if (reply.flags & 1)
      status = "Reset available";
    else
      status = "Ready; reset not eligible";
  }
  return status;
}
bool DeskUART::motion_trim_access_allowed_(uint32_t now) const {
  return !this->display_.busy() && !this->firmware_export_active_ && !this->custom_.pending() &&
         !this->reset_speed_.blocking() &&
         (!this->reset_speed_.busy() || this->motion_trim_token_pending_) &&
         !this->child_lock_busy_() && !this->detection_.busy() && !this->direct_.busy() &&
         !this->legacy_ownership_active_() && !this->keep_awake_active_ &&
         !this->target_move_.busy() && !this->target_move_.dirty() &&
         !this->preset_settings_.busy() && this->limit_phase_ == LimitPhase::Idle &&
         this->identity_pending_ == 255 && !this->timer_.busy() && !this->pulse_.active() &&
         !this->release_ack_.pending() && !this->stop_ack_.pending() && !this->pending_preset_ &&
         !this->stop_latched_ && !this->release_fault_ && this->height_seen_ &&
         this->monitor_.fresh(now) &&
         static_cast<uint32_t>(now - this->last_height_received_) <= 750 &&
         this->stable_ms_(now) >= 1500 &&
         static_cast<uint32_t>(now - this->last_preset_recall_) >= 3000;
}

void DeskUART::cancel_motion_trim_(const char* reason, bool disconnect) {
  if (!this->motion_trim_enabled_()) {
    return;
  }
  const bool busy = this->motion_trim_busy_();
  const bool editing = this->motion_trim_edit_active_;
  if (this->motion_trim_token_pending_) {
    this->reset_speed_.yield_read();
    this->motion_trim_token_pending_ = false;
    this->last_reset_speed_query_ = millis();
  }
  this->motion_trim_.cancel();
  this->motion_trim_dt_probe_.cancel();
  this->motion_trim_edit_active_ = false;
  this->motion_trim_busy_refused_ = false;
  this->motion_trim_readback_active_ = false;
  if (editing) {
    this->motion_trim_readback_attempts_ = 0;
  }
  this->motion_trim_readback_pending_ =
      !disconnect && this->motion_trim_.supported() && !this->motion_trim_.state_known() &&
      !this->motion_trim_.reply().flags && this->motion_trim_readback_attempts_ < 3;
  if (disconnect) {
    this->motion_trim_.disconnect();
    this->motion_trim_dt_probe_.disconnect();
    this->motion_trim_attempts_ = this->motion_trim_dt_attempts_ = this->motion_trim_dt_replies_ =
        this->motion_trim_rs_attempts_ = this->motion_trim_rs_replies_ = 0;
    this->motion_trim_readback_attempts_ = 0;
  }
  if (busy || disconnect) {
    this->motion_trim_message_ = reason;
  }
  // Publish later, after the preempting command has acquired the UART.
}

bool DeskUART::redetect_motion_trim() {
  if (!this->motion_trim_enabled_() || this->motion_trim_busy_()) {
    return this->result_(false, "Trim settings busy; detection not restarted");
  }
  this->cancel_motion_trim_("Checking trim support", true);
  this->last_motion_trim_probe_ = this->last_motion_trim_dt_probe_ = millis() - 1000;
  return true;
}

bool DeskUART::set_motion_trim(uint8_t index, float value) {
  if (this->motion_trim_busy_()) {
    this->motion_trim_busy_refused_ = true;
    this->result_(false, "Trim settings busy; change not queued");
    this->publish_motion_trim_(millis());
    return false;
  }
  if (index >= 4 || !this->motion_trim_numbers_[index] || !std::isfinite(value) || value < -3 ||
      value > 3 || value != static_cast<int>(value)) {
    return this->result_(false, "Trim must be a whole number from -3 to 3");
  }
  if (!this->motion_trim_.supported()) {
    return this->result_(false, "Trim support not detected on this connection");
  }
  if (const char* fault = this->motion_trim_fault_status_()) {
    return this->result_(false, fault);
  }
  this->yield_child_lock_read_();
  if (!this->motion_trim_access_allowed_(millis()) || !this->child_lock_.state_known() ||
      this->child_lock_.reply().storage_fault) {
    return this->result_(false, "Trim change blocked; wait for the desk to be idle and try again");
  }
  if (this->motion_trim_.edit(index, value) != desk_protocol::MotionTrim::RequestStatus::Started) {
    return this->result_(false, "Trim change unavailable; check trim status");
  }
  this->motion_trim_edit_active_ = true;
  this->motion_trim_readback_pending_ = this->motion_trim_readback_active_ = false;
  this->motion_trim_readback_attempts_ = 0;
  this->motion_trim_busy_refused_ = false;
  this->motion_trim_message_ = nullptr;
  // Transmit before invoking entity callbacks that may Stop or enter maintenance.
  this->motion_trim_loop_(millis());
  if (this->motion_trim_.busy()) {
    return this->result_(true, "Trim change requested; awaiting confirmation");
  }
  return false;
}

void DeskUART::motion_trim_frame_(const desk_protocol::Frame& frame, uint32_t now) {
  if (!this->motion_trim_enabled_()) {
    return;
  }
  if (this->motion_trim_.accept(frame, now)) {
    this->motion_trim_result_();
  }
  if (this->motion_trim_dt_probe_.busy()) {
    this->motion_trim_dt_probe_.accept(frame, now);
    if (!this->motion_trim_dt_probe_.busy() && this->motion_trim_dt_probe_.known() &&
        !this->motion_trim_dt_probe_.reply().status &&
        this->motion_trim_.observe_sequence(desk_protocol::be16(frame.bytes + 35))) {
      if (this->motion_trim_dt_replies_ < 2) {
        ++this->motion_trim_dt_replies_;
      }
    }
  }
  desk_protocol::ResetSpeedReply reset;
  desk_protocol::ChildLockReply lock;
  desk_protocol::DirectReply direct;
  if (desk_protocol::reset_speed_reply(frame, reset)) {
    this->motion_trim_.observe_sequence(reset.sequence);
  } else if (desk_protocol::child_lock_reply(frame, lock)) {
    this->motion_trim_.observe_sequence(lock.sequence);
  } else if (desk_protocol::direct_reply(frame, direct)) {
    this->motion_trim_.observe_sequence(desk_protocol::be16(frame.bytes + 35));
  }
}

void DeskUART::motion_trim_result_() {
  if (this->motion_trim_.busy()) {
    return;
  }
  using Outcome = desk_protocol::MotionTrim::Outcome;
  const Outcome outcome = this->motion_trim_.outcome();
  if (this->motion_trim_readback_active_) {
    this->motion_trim_readback_active_ = false;
    const bool retryable =
        outcome == Outcome::Timeout ||
        (outcome == Outcome::Rejected &&
         this->motion_trim_.reply().result == desk_protocol::MotionTrimCodec::Busy);
    this->motion_trim_readback_pending_ =
        retryable && !this->motion_trim_.reply().flags && this->motion_trim_readback_attempts_ < 3;
    if (retryable) {
      // Retry only the read. The failed edit's index, tuple and token are gone.
      this->last_motion_trim_probe_ = millis();
      if (this->motion_trim_readback_attempts_ >= 3) {
        this->motion_trim_message_ = "Trim settings unknown; readback attempts exhausted";
      }
    }
    return;
  }
  if (!this->motion_trim_edit_active_) {
    return;
  }
  this->motion_trim_edit_active_ = false;
  this->motion_trim_busy_refused_ = false;
  const bool saved = outcome == Outcome::Saved;
  // Recover current values through a bounded series of reads, never another SET.
  this->motion_trim_readback_pending_ = !saved && !this->motion_trim_.reply().flags;
  this->motion_trim_readback_attempts_ = 0;
  const char* message = saved ? "Trim value confirmed"
                        : outcome == Outcome::Timeout
                            ? "Trim change not confirmed; current settings unknown, no retry"
                        : outcome == Outcome::InvalidReply
                            ? "Trim confirmation did not match; current settings unknown"
                            : "Trim change rejected; no retry";
  this->motion_trim_message_ = saved ? nullptr : message;
  this->result_(saved, message);
}

const char* DeskUART::motion_trim_fault_status_() const {
  if (!this->motion_trim_.supported()) {
    return nullptr;
  }
  if (this->motion_trim_.reply().flags & desk_protocol::MotionTrimCodec::ControlsLocked) {
    return "Trim storage fault; desk controls locked, service required";
  }
  if (this->motion_trim_.reply().flags & desk_protocol::MotionTrimCodec::PlatformFault) {
    return "Trim platform fault; trim controls unavailable";
  }
  return nullptr;
}

void DeskUART::publish_motion_trim_(uint32_t now) {
  if (!this->motion_trim_enabled_()) {
    return;
  }
  const bool exporting_at_entry = this->firmware_export_active_;
  const bool fresh = !exporting_at_entry && this->height_seen_ && this->monitor_.fresh(now) &&
                     static_cast<uint32_t>(now - this->last_height_received_) <= 750;
  const char* fault = this->motion_trim_fault_status_();
  for (size_t i = 0; i < 4; ++i) {
    auto* number = this->motion_trim_numbers_[i];
    if (!number) {
      continue;
    }
    if (!fresh || !this->motion_trim_.supported() || !this->motion_trim_.state_known() || fault) {
      number->invalidate();
    } else {
      const float value = this->motion_trim_.last_confirmed().active.trim[i];
      if (!number->has_state() || number->state != value) {
        number->publish_state(value);
      }
    }
    if (!exporting_at_entry && this->firmware_export_active_) {
      return;
    }
  }
  const char* status = this->motion_trim_message_;
  if (!fresh) {
    status = "Trim settings unavailable; no recent controller reply";
  } else if (fault) {
    status = fault;
  } else if (this->motion_trim_.busy()) {
    status = this->motion_trim_busy_refused_  ? "Trim settings busy; change not queued"
             : this->motion_trim_edit_active_ ? "Updating trim settings; awaiting confirmation"
                                              : "Checking trim settings";
  } else if (!this->motion_trim_.supported()) {
    status = this->motion_trim_attempts_ >= 3 ? "TR01 not detected on this connection"
                                              : "Trim support not detected on this connection";
  } else if (this->motion_trim_.state_known()) {
    switch (this->motion_trim_.last_confirmed().storage) {
      case desk_protocol::MotionTrimCodec::Default:
        status = "Some trims use defaults";
        break;
      case desk_protocol::MotionTrimCodec::Verified:
        status = "Trim settings verified";
        break;
      case desk_protocol::MotionTrimCodec::Fault:
        status = "Trim storage fault; saved settings unavailable";
        break;
      case desk_protocol::MotionTrimCodec::Uncertain:
        status = "Active trims read; saved settings uncertain";
        break;
    }
  }
  if (!status) {
    status = "Current trim settings unknown";
  }
  if (status != this->last_motion_trim_status_) {
    this->last_motion_trim_status_ = status;
    if (this->motion_trim_status_) {
      this->motion_trim_status_->publish_state(status);
    }
  }
}

void DeskUART::motion_trim_loop_(uint32_t now) {
  if (!this->motion_trim_enabled_()) {
    return;
  }
  const bool fresh = !this->firmware_export_active_ && this->height_seen_ &&
                     this->monitor_.fresh(now) &&
                     static_cast<uint32_t>(now - this->last_height_received_) <= 750;
  if (!fresh && (this->motion_trim_.supported() || this->motion_trim_busy_() ||
                 this->motion_trim_attempts_ || this->motion_trim_rs_replies_)) {
    this->cancel_motion_trim_("Controller disconnected; trim settings unknown", true);
  }
  this->motion_trim_.tick(now);
  this->motion_trim_dt_probe_.tick(now, fresh, this->stable_ms_(now));
  this->motion_trim_result_();
  if (this->motion_trim_busy_() && !this->motion_trim_access_allowed_(now)) {
    this->cancel_motion_trim_("Trim change cancelled; desk no longer idle");
  }
  if (this->motion_trim_.awaiting_token() && !this->motion_trim_token_pending_) {
    if (this->reset_speed_.probe(now)) {
      this->motion_trim_token_pending_ = true;
      this->last_reset_speed_query_ = now;
      desk_protocol::Frame request;
      if (this->reset_speed_.take_frame(request)) {
        this->motion_trim_sending_ = this->reset_speed_sending_ = true;
        this->send_(request);
        this->motion_trim_sending_ = this->reset_speed_sending_ = false;
      }
    }
  }
  if (!this->motion_trim_busy_() && this->motion_trim_access_allowed_(now)) {
    if (!this->motion_trim_.supported() && this->motion_trim_dt_replies_ < 2 &&
        this->motion_trim_dt_attempts_ < 3 && this->child_lock_dt02_ &&
        static_cast<uint32_t>(now - this->last_motion_trim_dt_probe_) >= 1000) {
      if (this->motion_trim_dt_probe_.probe(now)) {
        ++this->motion_trim_dt_attempts_;
        this->last_motion_trim_dt_probe_ = now;
        desk_protocol::Frame request;
        if (this->motion_trim_dt_probe_.take_frame(request)) {
          this->motion_trim_sending_ = true;
          this->send_(request);
          this->motion_trim_sending_ = false;
        }
      }
    } else if ((this->motion_trim_readback_pending_ ||
                (!this->motion_trim_.supported() && this->motion_trim_attempts_ < 3 &&
                 this->motion_trim_dt_replies_ >= 2 && this->motion_trim_rs_replies_ >= 2)) &&
               static_cast<uint32_t>(now - this->last_motion_trim_probe_) >= 1000) {
      if (this->motion_trim_.probe() == desk_protocol::MotionTrim::RequestStatus::Started) {
        const bool readback = this->motion_trim_readback_pending_;
        this->motion_trim_readback_pending_ = false;
        this->motion_trim_readback_active_ = readback;
        if (readback) {
          ++this->motion_trim_readback_attempts_;
        } else {
          ++this->motion_trim_attempts_;
        }
        this->last_motion_trim_probe_ = now;
      }
    }
  }
  desk_protocol::Frame request;
  if (this->motion_trim_.pending_frame(request)) {
    this->motion_trim_sending_ = true;
    const uint32_t transmitted = millis();
    const bool sent = this->send_(request);
    this->motion_trim_sending_ = false;
    if (sent && this->motion_trim_.busy()) {
      this->motion_trim_.mark_transmitted(transmitted);
    } else {
      this->cancel_motion_trim_("Trim request could not be sent");
    }
  }
  this->publish_motion_trim_(millis());
}

void DeskUART::reset_speed_loop_(uint32_t now) {
  if (!this->reset_speed_enabled_())
    return;
  const bool fresh = this->height_seen_ && this->monitor_.fresh(now) &&
                     static_cast<uint32_t>(now - this->last_height_received_) <= 750;
  this->reset_speed_.tick(now, fresh);
  if (this->reset_speed_.take_brake()) {
    const char* reason = this->reset_speed_.error();
    this->stop();
    this->result_(false, reason ? reason : "Reset control lost; pausing");
  }
  this->reset_speed_result_();
  // GET discovers status and rotates a challenge. It never acquires or renews
  // an operation. Reconnection only makes an explicit Reset command available.
  const bool trim_discovery = this->motion_trim_enabled_() && this->motion_trim_rs_replies_ < 2 &&
                              this->motion_trim_rs_attempts_ < 3;
  if ((this->reset_speed_controls_enabled_() || trim_discovery) && fresh &&
      !this->reset_speed_.busy() && !this->reset_speed_.owned() && this->child_lock_dt02_ &&
      this->child_lock_.supported() && this->reset_speed_access_allowed_(now, false, true) &&
      !this->available() &&
      static_cast<uint32_t>(now - this->last_reset_speed_query_) >=
          ((this->reset_speed_.supported() || this->reset_stop_pending_) ? 500U : 5000U)) {
    this->last_reset_speed_query_ = now;
    if (this->reset_speed_.probe(now) && this->motion_trim_enabled_()) {
      ++this->motion_trim_rs_attempts_;
    }
  }
  desk_protocol::Frame frame;
  if (this->reset_speed_.take_frame(frame)) {
    this->reset_speed_sending_ = true;
    this->send_(frame);
    this->reset_speed_sending_ = false;
  }
  this->publish_reset_speed_(millis());
}

bool DeskUART::child_lock_access_allowed_(uint32_t now, bool read, bool presentation) const {
  const bool recovery_read = read && this->reset_stop_pending_;
  return !this->display_.busy() && !this->motion_trim_busy_() && !this->custom_write_pending_() &&
         (!this->custom_.pending() || read || presentation) &&
         (!this->reset_speed_blocking_() ||
          (!this->reset_speed_.owned() && !this->reset_speed_.busy()) ||
          (presentation && !this->reset_speed_.blocking() &&
           this->reset_speed_.operation() == desk_protocol::ResetSpeed::Get)) &&
         !this->firmware_export_active_ && !this->detection_.busy() &&
         (recovery_read || (!this->journal_error_() && !this->journal_recheck_pending_ &&
                            !this->keep_awake_recovery_ && !this->keep_awake_active_ &&
                            !this->accept_current_active_ && !this->recovery_check_active_ &&
                            !this->restore_requested_ && !this->target_move_.dirty())) &&
         !this->target_move_.busy() && !this->direct_.busy() && this->monitor_.fresh(now) &&
         this->height_seen_ && static_cast<uint32_t>(now - this->last_height_received_) <= 750 &&
         this->stable_ms_(now) >= 1500 && (!this->stop_latched_ || recovery_read) &&
         !this->release_fault_ && !this->timer_.busy() &&
         (!this->preset_settings_.busy() || (presentation && !this->preset_settings_.writing())) &&
         !this->pulse_.active() && !this->release_ack_.pending() && !this->stop_ack_.pending() &&
         !this->pending_preset_ &&
         (this->limit_phase_ == LimitPhase::Idle ||
          (presentation && this->limit_phase_ == LimitPhase::Read)) &&
         (this->identity_pending_ == 255 || presentation) &&
         static_cast<uint32_t>(now - this->last_preset_recall_) >= 3000;
}
void DeskUART::invalidate_child_lock_(const char* reason, bool publish, bool report_failure) {
  if (!this->child_lock_enabled_())
    return;
  this->child_lock_.disconnect(reason, report_failure);
  this->child_lock_detection_.reset();
  this->child_lock_dt02_ = false;
  this->child_lock_probed_ = false;
  this->last_child_lock_query_ = millis();
  if (this->child_lock_switch_)
    this->child_lock_switch_->invalidate();
  if (const char* failure = this->child_lock_.take_failure())
    this->result_(false, failure);
  if (publish)
    this->publish_child_lock_(millis());
}
void DeskUART::yield_child_lock_read_() {
  this->yield_display_read_();
  this->yield_reset_speed_read_();
  if (this->child_lock_.busy() && this->child_lock_.operation() == 0) {
    this->child_lock_.yield_read();
    this->last_child_lock_query_ = millis();
    return;
  }
  if (!this->child_lock_detection_.busy())
    return;
  // Prerequisite discovery has not confirmed lock state. Cancel its preflight
  // without invoking callbacks until the user control owns its transaction.
  this->child_lock_detection_.reset();
  this->child_lock_.disconnect("Panel-lock check cancelled by another control; lock state unknown",
                               false);
  this->child_lock_probed_ = false;
  this->last_child_lock_query_ = millis();
  if (this->child_lock_switch_)
    this->child_lock_switch_->invalidate();
}
void DeskUART::publish_child_lock_(uint32_t now) {
  if (!this->child_lock_enabled_())
    return;
  const bool fresh = !this->firmware_export_active_ && this->height_seen_ &&
                     this->monitor_.fresh(now) &&
                     static_cast<uint32_t>(now - this->last_height_received_) <= 750;
  if (this->child_lock_switch_) {
    if (!fresh || !this->child_lock_.state_known())
      this->child_lock_switch_->invalidate();
    else if (!this->child_lock_switch_->has_state() ||
             this->child_lock_switch_->state != this->child_lock_.reply().locked)
      this->child_lock_switch_->publish_confirmed(this->child_lock_.reply().locked);
  }
  // Switch callbacks can synchronously start another SET or maintenance session.
  const bool available = this->child_lock_.state_known() && !this->reset_speed_.blocking() &&
                         (!this->child_lock_busy_() || (!this->child_lock_detection_.busy() &&
                                                        this->child_lock_.operation() == 0)) &&
                         this->child_lock_access_allowed_(millis(), false, true);
  if (available != this->last_child_lock_available_) {
    this->last_child_lock_available_ = available;
    if (this->child_lock_available_)
      this->child_lock_available_->publish_state(available);
  }
  const char* status = this->child_lock_status_text_(now);
  if (status != this->last_child_lock_status_) {
    this->last_child_lock_status_ = status;
    if (this->child_lock_status_)
      this->child_lock_status_->publish_state(status);
  }
}
const char* DeskUART::child_lock_status_text_(uint32_t now) const {
  const bool fresh = !this->firmware_export_active_ && this->height_seen_ &&
                     this->monitor_.fresh(now) &&
                     static_cast<uint32_t>(now - this->last_height_received_) <= 750;
  const char* status = this->child_lock_detection_.busy()
                           ? "Checking whether the controller supports panel lock"
                           : this->child_lock_.error();
  if (!status) {
    if (!fresh || this->firmware_export_active_)
      status = "Panel lock unavailable; lock state unknown";
    else if (this->child_lock_.busy() &&
             (this->child_lock_.operation() != 0 || !this->child_lock_.state_known()))
      status = this->child_lock_.operation() == 0 ? "Checking panel-lock state"
               : this->child_lock_.operation() == 1
                   ? "Lock requested; waiting for controller confirmation"
                   : "Unlock requested; waiting for controller confirmation";
    else if (!this->child_lock_.supported())
      status = "Panel-lock state unknown; support must be checked";
    else if (this->child_lock_.reply().storage_fault)
      status = "Panel-lock saving failed; locked now, lock state after power loss uncertain";
    else if (this->child_lock_.reply().locked)
      status = "Locked";
    else
      status = this->child_lock_.reply().physical_gate ? "Unlocked; release all physical keys"
                                                       : "Unlocked";
  }
  return status;
}
bool DeskUART::set_child_lock(bool locked) {
  this->yield_child_lock_read_();
  if (this->reset_speed_blocking_())
    return this->result_(false, this->control_reason_(millis()));
  const uint32_t now = millis();
  if (!this->child_lock_switch_)
    return this->result_(false, "Child lock switch not configured");
  if (!this->child_lock_.supported())
    return this->result_(false, "Panel-lock support not confirmed; lock control unavailable");
  if (this->child_lock_busy_() || !this->child_lock_access_allowed_(now))
    return this->result_(
        false, "Child lock blocked: wait for fresh settled height and other operations to finish");
  if (!this->child_lock_.set(locked, now))
    return false;
  this->last_keep_awake_ = this->last_child_lock_query_ = now;
  desk_protocol::Frame frame;
  if (this->child_lock_.take_frame(frame)) {
    this->child_lock_sending_ = true;
    this->send_(frame);
    this->child_lock_sending_ = false;
  }
  this->result_(true, "Child lock requested; awaiting controller acknowledgement, no retry");
  // Publishing availability may synchronously invoke Stop. Its result must
  // remain the last result, rather than being overwritten by this request.
  this->publish_child_lock_(millis());
  return true;
}
void DeskUART::child_lock_loop_(uint32_t now) {
  if (!this->child_lock_enabled_())
    return;
  const bool fresh = this->height_seen_ && this->monitor_.fresh(now) &&
                     static_cast<uint32_t>(now - this->last_height_received_) <= 750;
  const bool child_polling = this->child_lock_switch_ || this->child_lock_status_ ||
                             this->child_lock_available_ || this->reset_speed_controls_enabled_();
  if (!fresh)
    this->invalidate_child_lock_("No recent controller reply; panel-lock state unknown");
  this->child_lock_.tick(now, fresh);
  this->child_lock_result_();
  if (this->child_lock_busy_() &&
      !this->child_lock_access_allowed_(
          now, this->child_lock_detection_.busy() || this->child_lock_.operation() == 0))
    this->invalidate_child_lock_(
        "Panel-lock request interrupted by changed movement conditions; lock state unknown, request not retried");
  // Reuse auto detection, including a conclusive stock/preset result.
  // Polling a validated DT02 prerequisite never implies CL01 support.
  if (child_polling && !this->child_lock_dt02_ && this->child_lock_probed_ &&
      this->child_lock_detection_.error() &&
      static_cast<uint32_t>(now - this->last_child_lock_query_) >= 5000) {
    // A lost prerequisite reply must not strand an installed, locked CL01.
    // Retry the complete read-only identity/profile transaction after backoff.
    this->child_lock_detection_.reset();
    this->child_lock_probed_ = false;
  }
  if (fresh && !this->child_lock_dt02_ && !this->child_lock_probed_ &&
      this->child_lock_access_allowed_(now, true)) {
    // A configured direct/auto backend may already have validated DT02. With
    // preset/monitor-only configurations, verify the exact identity, scale and
    // DT02 capability first. Fresh height alone never authorizes D3 probing.
    if (this->auto_target_ready_ &&
        this->detection_.backend() == desk_protocol::TargetDetection::Backend::Preset) {
      this->child_lock_probed_ = true;
      this->child_lock_.disconnect(
          "Required height control support not confirmed; panel lock unavailable, lock state unknown");
    } else if (this->direct_.known() ||
               (this->auto_target_ready_ &&
                this->detection_.backend() == desk_protocol::TargetDetection::Backend::Direct)) {
      this->child_lock_dt02_ = true;
    } else {
      if (this->child_lock_detection_.idle() && !this->available() &&
          static_cast<uint32_t>(now - this->last_child_lock_query_) >= 800) {
        this->child_lock_.disconnect(nullptr);
        this->child_lock_detection_.seed(this->direct_.next_nonce());
        this->child_lock_detection_.start(now, this->monitor_.height().sequence);
      }
      this->child_lock_detection_.tick(now);
      desk_protocol::Frame request;
      if (!this->available() && this->child_lock_detection_.take_frame(request)) {
        this->direct_.seed(this->child_lock_detection_.next_nonce());
        this->child_lock_sending_ = true;
        this->send_(request);
        this->child_lock_sending_ = false;
      }
      if (this->child_lock_detection_.backend() == desk_protocol::TargetDetection::Backend::Direct)
        this->child_lock_dt02_ = true;
      else if (this->child_lock_detection_.error() ||
               this->child_lock_detection_.backend() ==
                   desk_protocol::TargetDetection::Backend::Preset) {
        this->child_lock_probed_ = true;
        this->last_child_lock_query_ = now;
        this->child_lock_.disconnect(
            "Required height control support not confirmed; panel lock unavailable, lock state unknown");
      }
    }
  }
  if ((child_polling || !this->child_lock_probed_) && fresh && this->child_lock_dt02_ &&
      !this->child_lock_busy_() && this->child_lock_access_allowed_(now, true) &&
      !this->available() &&
      ((!this->child_lock_probed_ &&
        static_cast<uint32_t>(now - this->last_child_lock_query_) >= 800) ||
       static_cast<uint32_t>(now - this->last_child_lock_query_) >= 5000)) {
    // A lost/invalid GET or SET reply recovers through a new nonce-bound GET
    // after backoff. This refreshes runtime state and never replays a SET.
    this->child_lock_probed_ = true;
    this->last_child_lock_query_ = now;
    this->child_lock_.probe(now);
    desk_protocol::Frame frame;
    if (this->child_lock_.take_frame(frame)) {
      this->child_lock_sending_ = true;
      this->send_(frame);
      this->child_lock_sending_ = false;
    }
  }
  this->publish_child_lock_(millis());
}

void DeskUART::child_lock_result_() {
  // Consume terminal events and snapshot their data before invoking any
  // synchronous automation that may start a new transaction or Stop.
  const char* failure = this->child_lock_.take_failure();
  const bool completed_set = this->child_lock_.take_completed() && this->child_lock_.operation();
  const bool success = this->child_lock_.reply().status == 0 && this->child_lock_.state_known();
  const char* message =
      success ? (this->child_lock_.reply().locked
                     ? "Panel locked; controller confirmed the lock setting was saved"
                     : "Panel unlocked; controller confirmed the lock setting was saved")
              : this->child_lock_.error();
  if (failure)
    this->result_(false, failure);
  else if (completed_set)
    this->result_(success, message);
}

void DeskUART::direct_loop_(uint32_t now) {
  if (!this->direct_mode_)
    return;
  const bool fresh = this->height_seen_ && this->monitor_.fresh(now) &&
                     static_cast<uint32_t>(now - this->last_height_received_) <= 750;
  this->direct_.tick(now, fresh, this->stable_ms_(now));
  if (!fresh)
    this->direct_probe_done_ = false;
  if (this->direct_.take_brake()) {
    const char* error = this->direct_.error();
    this->stop();
    this->result_(false, error ? error : "Direct admission uncertain; braking");
  }
  if (this->direct_.take_accepted())
    this->result_(true,
                  "Controller accepted the target; actual height and standstill not confirmed");
  if (fresh && this->target_detected_() && !this->direct_probe_done_ && !this->direct_.busy() &&
      this->discovery_access_allowed_(now)) {
    this->direct_probe_done_ = this->direct_.probe(now);
  }
  if (this->direct_.awaiting_set() && !this->direct_.set_sent() &&
      !this->direct_access_allowed_(now)) {
    this->direct_.cancel();
    this->result_(false, "Target request cancelled before sending; movement conditions changed");
  }
  desk_protocol::Frame frame;
  if (this->direct_.take_frame(frame))
    this->send_(frame);
  if (this->direct_.error() && this->direct_.error() != this->last_direct_error_) {
    this->last_direct_error_ = this->direct_.error();
    this->result_(false, this->last_direct_error_);
  }
  const auto phase = static_cast<uint8_t>(this->direct_.phase());
  if (phase != this->direct_last_phase_) {
    this->direct_last_phase_ = phase;
    ESP_LOGD(TAG, "Direct DT02 phase=%u verified=%d sleep_wrap=%d guards=0x%04x", unsigned(phase),
             this->direct_.known(), this->direct_.sleep_wrap(), this->direct_.reply().guards);
  }
}

bool DeskUART::step_height(bool up) {
  if (this->firmware_export_active_)
    return this->result_(false, "Firmware read active; desk controls paused");
  const uint32_t now = millis();
  if (!this->monitor_.fresh(now))
    return this->result_(false, this->control_reason_(now));
  const float current =
      this->height_profile_.display(this->monitor_.height().q).before_rounding_tenths_cm / 10.0f;
  float target = current + (up ? 1.0f : -1.0f);
  if (target < this->min_height_)
    target = this->min_height_;
  if (target > this->max_height_)
    target = this->max_height_;
  if ((up ? target <= current : target >= current) || std::fabs(target - current) < 0.05f)
    return this->result_(false, "Step outside configured height range");
  if (!this->move_to_height(target))
    return false;
  if (this->target_number_)
    this->target_number_->publish_state(this->display_height(target));
  return true;
}
bool DeskUART::restore_temporary_preset() {
  if (!this->journal_active_ ||
      (!this->legacy_backend_ready_() && !this->legacy_ownership_active_()))
    return this->result_(false, this->auto_target_mode_ && !this->auto_target_ready_
                                    ? "Checking height control support; preset recovery unavailable"
                                    : "Preset recovery is not used with direct height control");
  if (const char* error = this->journal_error_())
    return this->result_(false, error);
  if (this->firmware_export_active_)
    return this->result_(false, "Firmware read active; desk controls paused");
  const uint32_t now = millis();
  this->yield_child_lock_read_();
  this->last_keep_awake_ = now;
  if (!this->recovery_access_allowed_(now))
    return this->result_(
        false,
        "Preset recovery blocked: wait for fresh settled height and other operations to finish");
  if (this->keep_awake_fault_ && !this->target_move_.busy() && !this->cached_journal_valid_) {
    this->keep_awake_fault_ = false;
    this->last_keep_awake_ = now;
    return this->result_(true, "Keep-awake fault cleared; next attempt after 30 seconds");
  }
  if (this->cached_journal_valid_ && !this->target_move_.busy())
    this->target_move_.recover(this->cached_journal_.original, this->cached_journal_.temporary);
  const bool retried = this->target_move_.restore_backup(now);
  if (retried) {
    this->legacy_transaction_started_ = true;
    this->keep_awake_active_ = false;
    this->keep_awake_recovery_ = false;
    this->restore_requested_ = true;
    this->journal_recheck_pending_ = false;
    this->keep_awake_fault_ = false;
    this->soft_abort_error_ = nullptr;
    this->last_target_error_ = nullptr;
    this->last_keep_awake_ = now;
  }
  return this->result_(retried, retried ? "Preset restoration requested; waiting for readback"
                                        : "No failed preset transaction to restore");
}
bool DeskUART::recovery_access_allowed_(uint32_t now) const {
  return !this->display_.busy() && !this->motion_trim_busy_() && !this->custom_.pending() &&
         !this->reset_speed_blocking_() && !this->child_lock_busy_() && !this->detection_.busy() &&
         !this->journal_error_() && !this->direct_.busy() && !this->firmware_export_active_ &&
         !this->preset_settings_.busy() && !this->timer_.busy() && !this->release_fault_ &&
         this->monitor_.fresh(now) && this->stable_ms_(now) >= 1500 &&
         static_cast<uint32_t>(now - this->last_height_received_) <= 750 &&
         static_cast<uint32_t>(now - this->last_preset_recall_) >= 3000 && !this->pulse_.active() &&
         !this->release_ack_.pending() && !this->stop_ack_.pending() &&
         this->limit_phase_ == LimitPhase::Idle;
}
bool DeskUART::accept_current_preset() {
  if (!this->journal_active_ ||
      (!this->legacy_backend_ready_() && !this->legacy_ownership_active_()))
    return this->result_(false, this->auto_target_mode_ && !this->auto_target_ready_
                                    ? "Checking height control support; preset recovery unavailable"
                                    : "Preset recovery is not used with direct height control");
  if (const char* error = this->journal_error_())
    return this->result_(false, error);
  if (this->firmware_export_active_)
    return this->result_(false, "Firmware read active; desk controls paused");
  const uint32_t now = millis();
  this->yield_child_lock_read_();
  this->last_keep_awake_ = now;
  if (!this->recovery_access_allowed_(now))
    return this->result_(
        false,
        "Preset acceptance blocked: wait for fresh settled height and other operations to finish");
  if (this->cached_journal_valid_ && !this->target_move_.busy())
    this->target_move_.recover(this->cached_journal_.original, this->cached_journal_.temporary);
  if (!this->target_move_.accept_current(now))
    return this->result_(false, "No failed preset backup to discard");
  this->legacy_transaction_started_ = true;
  this->accept_current_active_ = true;
  this->journal_recheck_pending_ = false;
  this->keep_awake_active_ = false;
  this->soft_abort_error_ = nullptr;
  this->keep_awake_recovery_ = false;
  this->last_target_error_ = nullptr;
  return this->result_(true, "Checking current preset twice before discarding backup; no movement");
}
void DeskUART::target_loop_(uint32_t now) {
  if (!this->legacy_backend_ready_() && !this->legacy_ownership_active_()) {
    this->publish_restore_pending_();
    return;
  }
  if (this->reset_speed_blocking_())
    return;
  if (this->journal_error_()) {
    this->publish_restore_pending_();
    return;
  }
  if (!this->target_enabled_ && !this->target_move_.busy())
    return;
  using Phase = desk_protocol::TemporaryPreset::Phase;
  // Any new or unfinished transaction supersedes an earlier verified restore.
  if (this->target_move_.busy())
    this->verified_journal_.valid = false;
  // RX may have advanced RestoreVerify to ClearJournal with an expired height
  // sample. Check before journal_cleared() changes busy() to false; otherwise
  // the successful parameter read hides the lost height link.
  if (this->soft_abort_error_ && this->target_move_.busy() && !this->monitor_.fresh(now)) {
    this->soft_abort_error_ = nullptr;
    this->keep_awake_fault_ = true;
    this->result_(false,
                  "Keep-awake interrupted; telemetry lost before original preset 3 was verified");
  }
  if ((this->target_move_.inspecting_recovery() || this->accept_current_active_ ||
       this->recovery_check_active_ || this->restore_requested_) &&
      !this->recovery_access_allowed_(now))
    this->target_move_.abort_recovery(now);
  this->target_move_.tick(now, this->monitor_.fresh(now), this->stable_ms_(now),
                          !this->pulse_.active() && !this->release_ack_.pending() &&
                              !this->stop_ack_.pending() && !this->release_fault_);
  if (this->target_move_.take_brake()) {
    if (this->keep_awake_active_)
      this->keep_awake_fault_ = true;
    this->stop();
  }
  if (this->target_move_.phase() == Phase::Backup) {
    this->target_move_.commit(this->save_journal_(true), now);
  }
  if (this->target_move_.phase() == Phase::Recall) {
    desk_protocol::Frame press;
    if (this->stop_latched_ || this->release_fault_ || !this->monitor_.fresh(now)) {
      this->target_move_.cancel(now);
    } else if (this->pulse_.begin(desk_protocol::Button::Preset3, now, press)) {
      this->target_move_.recalled(now, this->last_q_);
      if (this->keep_awake_active_)
        this->keep_awake_pressed_ = true;
      this->send_(press);
    }
  }
  if (this->target_move_.phase() == Phase::ClearJournal) {
    this->target_move_.journal_cleared(this->save_journal_(false));
    if (!this->target_move_.busy() && this->keep_awake_active_ && !this->keep_awake_fault_ &&
        this->cached_journal_valid_ &&
        this->cached_journal_.original == this->target_move_.original() &&
        this->cached_journal_.temporary == this->target_move_.target())
      this->verified_journal_ = {true, this->target_move_.original(), this->target_move_.target(),
                                 this->height_profile_.identity()};
    if (!this->target_move_.dirty()) {
      if (this->keep_awake_active_)
        ESP_LOGD(TAG, "Keep-awake: original preset 3 restored and read back");
      else if (this->target_move_.phase() == Phase::Monitoring)
        ESP_LOGI(TAG, "Original preset 3 restored after motion began; monitoring target move");
      else
        ESP_LOGI(TAG, "Original preset 3 restored and read back");
    }
  }
  desk_protocol::Frame frame;
  if (this->target_move_.take_frame(frame))
    this->send_(frame);
  if (this->target_move_.failed() && this->target_move_.error() != this->last_target_error_) {
    this->last_target_error_ = this->target_move_.error();
    // A height change or late sample before the press is not a fault on its
    // own: the original preset is restored and verified first. Lost telemetry,
    // failed restoration and anything after the press remain faults.
    if (this->keep_awake_active_ && !this->keep_awake_pressed_ && !this->soft_abort_error_ &&
        !this->keep_awake_fault_ && this->target_move_.interrupted_by_change() &&
        this->monitor_.fresh(now)) {
      this->soft_abort_error_ = this->last_target_error_;
      ESP_LOGW(TAG, "Keep-awake stopped before preset press: %s; verifying original preset 3",
               this->soft_abort_error_);
    } else {
      if (this->keep_awake_active_) {
        this->keep_awake_fault_ = true;
        this->soft_abort_error_ = nullptr;
      }
      // A requested Stop can finish preset restoration with an interruption result.
      if (!(this->stop_latched_ &&
            std::strcmp(this->last_target_error_, "Target move interrupted") == 0))
        this->result_(false, this->last_target_error_);
    }
  }
  if (this->soft_abort_error_ && !this->target_move_.busy()) {
    // Restored and read back, or never written. The normal interval applies.
    this->soft_abort_error_ = nullptr;
    this->last_keep_awake_ = now;
    ESP_LOGI(TAG, "Keep-awake: original preset 3 confirmed after early stop; next attempt in 30 s");
  }
  if (this->recovery_check_active_ || this->accept_current_active_ || this->restore_requested_) {
    if (!this->target_move_.busy()) {
      this->result_(
          true,
          this->accept_current_active_
              ? "Current preset accepted; backup discarded, value saved across power loss not confirmed"
          : this->recovery_check_active_
              ? "Original preset matches; backup kept, value saved across power loss not confirmed"
              : "Original preset restored; backup kept, value saved across power loss not confirmed");
      if (!this->recovery_check_active_)
        this->keep_awake_fault_ = false;
      this->last_keep_awake_ = now;
      this->last_target_error_ = nullptr;
    }
    if (!this->target_move_.busy() || this->target_move_.phase() == Phase::Fault)
      this->recovery_check_active_ = this->accept_current_active_ = this->restore_requested_ =
          false;
  }
  if (this->keep_awake_active_ && !this->target_move_.busy()) {
    this->keep_awake_active_ = false;
    this->last_keep_awake_ = now;
  }
  if (!this->legacy_ownership_active_())
    this->legacy_transaction_started_ = false;
  this->publish_restore_pending_();
}
bool DeskUART::set_timer(uint16_t minutes) {
  this->yield_custom_probe_();
  if (this->custom_write_pending_())
    return this->result_(false, "Custom timer/buzzer transaction pending");
  this->yield_reset_speed_read_();
  if (this->reset_speed_blocking_())
    return this->result_(false, this->control_reason_(millis()));
  if (this->firmware_export_active_)
    return this->result_(false, "Firmware read active; desk controls paused");
  if (this->timer_enabled_ &&
      (minutes == 0 || minutes == 30 || minutes == 60 || minutes == 90 || minutes == 120))
    this->yield_child_lock_read_();
  const uint32_t now = millis();
  this->last_keep_awake_ = now;
  if (!this->timer_enabled_ || this->child_lock_busy_() || this->detection_.busy() ||
      this->direct_.busy() || this->preset_settings_.busy() || this->stop_latched_ ||
      this->release_fault_ || this->target_move_.busy() || this->limit_phase_ != LimitPhase::Idle ||
      this->pulse_.active() || this->release_ack_.pending() || this->stop_ack_.pending() ||
      !this->monitor_.fresh(now) || this->stable_ms_(now) < 1500 ||
      !this->timer_.begin(minutes, now)) {
    return this->result_(
        false, this->timer_enabled_ ? this->control_reason_(now) : "Timer controls disabled");
  }
  this->last_timer_error_ = nullptr;
  ESP_LOGI(TAG, "Timer request %u min: leave local keys untouched until sequence finishes",
           minutes);
  this->timer_loop_(now);
  return true;
}
void DeskUART::timer_loop_(uint32_t now) {
  this->timer_.tick(now, this->monitor_.fresh(now), this->stable_ms_(now));
  desk_protocol::Frame frame;
  if (this->timer_.take_frame(frame))
    this->send_(frame);
  if (this->timer_.error() && this->timer_.error() != this->last_timer_error_) {
    this->last_timer_error_ = this->timer_.error();
    this->result_(false, this->last_timer_error_);
  }
  if (this->timer_.release_fault()) {
    this->release_fault_ = true;
    this->status_set_warning();
  }
  const bool pending = this->timer_.busy();
  if (pending != this->last_timer_pending_) {
    this->last_timer_pending_ = pending;
    if (!pending && this->timer_.completed())
      ESP_LOGI(TAG, "T-key sequence finished; controller timer state has no readback");
    if (this->timer_pending_sensor_)
      this->timer_pending_sensor_->publish_state(pending);
  }
}
// Experimental extension. A matched D8 acknowledgement reports acceptance
// by the controller, not physical sound or alarm expiry. No writes are retried.
void DeskUART::report_custom_result_(bool accepted, const char* message) {
  if (this->custom_result_)
    this->custom_result_->publish_state(message);
  if (accepted)
    ESP_LOGI(TAG, "Custom timer/buzzer: %s", message);
  else
    ESP_LOGW(TAG, "Custom timer/buzzer: %s", message);
}
bool DeskUART::custom_request_(desk_protocol::CustomTimerBuzzer::Operation op, int a, int b,
                               int count) {
  const uint32_t now = millis();
  if (!this->custom_enabled_) {
    this->report_custom_result_(false, "Experimental controls disabled");
    return false;
  }
  if (a < 0 || a > 65535 || b < 0 || b > 65535 || count < 0 || count > 255 ||
      !desk_protocol::CustomTimerBuzzer::valid(
          op, static_cast<uint16_t>(a), static_cast<uint16_t>(b), static_cast<uint8_t>(count))) {
    this->report_custom_result_(false, "Invalid parameters");
    return false;
  }
  this->yield_custom_probe_();
  if (!this->custom_.available()) {
    this->report_custom_result_(false, "Controller capability not confirmed; no request sent");
    return false;
  }
  this->yield_child_lock_read_();
  if (this->reset_speed_blocking_() || !this->reset_speed_access_allowed_(now) ||
      this->custom_.pending()) {
    const char* reason = this->control_reason_(now);
    this->report_custom_result_(false, std::strcmp(reason, "Ready") == 0
                                           ? "Desk busy or link not ready; no request sent"
                                           : reason);
    return false;
  }
  desk_protocol::Frame frame;
  if (!this->custom_.begin(op, static_cast<uint16_t>(a), static_cast<uint16_t>(b),
                           static_cast<uint8_t>(count), now, frame)) {
    this->report_custom_result_(false, "Request could not be started; no request sent");
    return false;
  }
  this->send_(frame);
  this->last_keep_awake_ = now;
  if (op == desk_protocol::CustomTimerBuzzer::Beep)
    this->custom_beep_deferral_.begin(static_cast<uint16_t>(a), static_cast<uint16_t>(b),
                                      static_cast<uint8_t>(count), now);
  this->report_custom_result_(true, "Request sent; awaiting controller acknowledgement");
  return true;
}
bool DeskUART::custom_set_timer(int minutes) {
  return this->custom_request_(desk_protocol::CustomTimerBuzzer::SetTimer, minutes, 0, 0);
}
bool DeskUART::custom_cancel_timer() {
  if (this->custom_.profiles_available()) {
    return this->custom_profile_request_(desk_protocol::CustomTimerBuzzer::CancelProfileTimer, 0, 0,
                                         0, 0, 0, false);
  }
  return this->custom_request_(desk_protocol::CustomTimerBuzzer::CancelTimer, 0, 0, 0);
}
bool DeskUART::custom_beep(int on_ms, int off_ms, int count) {
  return this->custom_request_(desk_protocol::CustomTimerBuzzer::Beep, on_ms, off_ms, count);
}
bool DeskUART::custom_beep_profile(int on_ms, int off_ms, int count, int volume, bool soften) {
  using Custom = desk_protocol::CustomTimerBuzzer;
  if (!Custom::valid_profile(on_ms, off_ms, count, volume)) {
    this->report_custom_result_(false, "Invalid sound parameters");
    return false;
  }
  if (volume == 0) {
    return this->custom_cancel_buzzer();
  }
  if (!this->custom_.profiles_available() && volume == 100 && !soften && on_ms >= 50 &&
      off_ms >= 50) {
    return this->custom_beep(on_ms, off_ms, count);
  }
  return this->custom_profile_request_(Custom::ProfileBeep, 0, on_ms, off_ms, count, volume,
                                       soften);
}
bool DeskUART::custom_timer_profile(int minutes, int on_ms, int off_ms, int count, int volume,
                                    bool soften) {
  return this->custom_profile_request_(desk_protocol::CustomTimerBuzzer::ProfileTimer, minutes,
                                       on_ms, off_ms, count, volume, soften);
}
bool DeskUART::custom_cancel_buzzer() {
  return this->custom_profile_request_(desk_protocol::CustomTimerBuzzer::CancelBuzzer, 0, 0, 0, 0,
                                       0, false);
}
bool DeskUART::custom_profile_request_(desk_protocol::CustomTimerBuzzer::Operation op, int minutes,
                                       int on_ms, int off_ms, int count, int volume, bool soften) {
  using Custom = desk_protocol::CustomTimerBuzzer;
  const bool profile = op == Custom::ProfileBeep || op == Custom::ProfileTimer;
  if (!this->custom_enabled_) {
    this->report_custom_result_(false, "Experimental controls disabled");
    return false;
  }
  if (profile && (!Custom::valid_profile(on_ms, off_ms, count, volume) ||
                  (op == Custom::ProfileTimer ? minutes < 2 || minutes > 720 : minutes != 0))) {
    this->report_custom_result_(false, "Invalid sound parameters");
    return false;
  }
  this->yield_custom_probe_();
  if (!this->custom_.profiles_available() ||
      (op == Custom::ProfileTimer && !this->custom_.timer_profiles_available())) {
    this->report_custom_result_(
        false, "Sound profiles not supported or not yet confirmed; no request sent");
    return false;
  }
  this->yield_child_lock_read_();
  const uint32_t now = millis();
  if (this->reset_speed_blocking_() || !this->reset_speed_access_allowed_(now) ||
      this->custom_.pending()) {
    const char* reason = this->control_reason_(now);
    this->report_custom_result_(false, std::strcmp(reason, "Ready") == 0
                                           ? "Desk busy or link not ready; no request sent"
                                           : reason);
    return false;
  }
  desk_protocol::Frame frame;
  if (!this->custom_.begin_profile(op, minutes, on_ms, off_ms, count, volume, soften, now, frame)) {
    this->report_custom_result_(false, "Request could not be started; no request sent");
    return false;
  }
  this->send_(frame);
  this->last_keep_awake_ = now;
  if (op == Custom::ProfileBeep) {
    this->custom_beep_deferral_.begin(on_ms, off_ms, count, now);
  } else if (op == Custom::ProfileTimer) {
    this->custom_alarm_deferral_.begin(minutes, on_ms, off_ms, count, now);
  }
  this->report_custom_result_(true, "Request sent; awaiting controller acknowledgement");
  return true;
}
void DeskUART::yield_custom_probe_() {
  if (this->custom_.pending() && this->custom_.is_probe()) {
    this->custom_.yield_probe();
    this->custom_last_probe_ = millis();
  }
}
void DeskUART::custom_event_() {
  using Event = desk_protocol::CustomTimerBuzzer::Event;
  const Event event = this->custom_.take_event();
  if (this->custom_.is_probe()) {
    // Background discovery has no user-write outcome. Preserve the last command
    // result even when a probe succeeds after an unacknowledged write.
    if (event != Event::None)
      ESP_LOGD(TAG, "Custom timer/buzzer capability check event=%u", static_cast<unsigned>(event));
    return;
  }
  switch (event) {
    case Event::Available:
      break;
    case Event::Accepted:
      if (this->custom_.operation() == desk_protocol::CustomTimerBuzzer::CancelProfileTimer ||
          this->custom_.operation() == desk_protocol::CustomTimerBuzzer::CancelTimer) {
        this->custom_alarm_deferral_.clear();
      }
      this->report_custom_result_(true, "Controller accepted request");
      break;
    case Event::Invalid:
      this->report_custom_result_(false, "Controller rejected parameters");
      break;
    case Event::Busy:
      this->report_custom_result_(false, "Controller busy; request not accepted");
      break;
    case Event::BackendError:
      this->report_custom_result_(false, "Controller backend error; result unknown, no retry");
      break;
    case Event::Timeout:
      this->report_custom_result_(false, "No acknowledgement; result unknown, no retry");
      break;
    case Event::LinkLost:
      this->report_custom_result_(false,
                                  "Interrupted or connection lost; result unknown, no retry");
      break;
    default:
      break;
  }
}
void DeskUART::custom_loop_(uint32_t now) {
  if (!this->custom_enabled_)
    return;
  // Earlier synchronous callbacks can start a request after the loop timestamp.
  // Use its current clock before expiring either the hold or acknowledgement.
  now = millis();
  this->custom_beep_deferral_.expire(now);
  this->custom_alarm_deferral_.expire(now);
  if (!this->height_seen_ || !this->monitor_.fresh(now) || !this->reset_speed_.supported())
    this->custom_.disconnect();
  this->custom_.tick(now);
  this->custom_event_();
  const bool available = this->custom_.available() && this->monitor_.fresh(now);
  if (available != this->custom_available_published_) {
    this->custom_available_published_ = available;
    if (this->custom_available_sensor_)
      this->custom_available_sensor_->publish_state(available);
  }
  // Only a read-only probe may be retried. It is serialized with all existing
  // transactions and never initiated while the device is stale or moving.
  if ((!available || !this->custom_.profiles_checked()) && !this->custom_.pending() &&
      static_cast<uint32_t>(now - this->custom_last_probe_) >= 15000 &&
      this->reset_speed_.supported() && !this->reset_speed_blocking_() && !this->available() &&
      this->reset_speed_access_allowed_(now)) {
    desk_protocol::Frame frame;
    const bool started =
        available
            ? this->custom_.begin_profile(desk_protocol::CustomTimerBuzzer::ProbeProfiles, 0, 0, 0,
                                          0, 0, false, now, frame)
            : this->custom_.begin(desk_protocol::CustomTimerBuzzer::Probe, 0, 0, 0, now, frame);
    if (started) {
      this->custom_last_probe_ = now;
      this->send_(frame);
    }
  }
}
bool DeskUART::result_(bool accepted, const char* message) {
  if (this->command_result_)
    this->command_result_->publish_state(message);
  if (accepted)
    ESP_LOGI(TAG, "%s", message);
  else
    ESP_LOGW(TAG, "Command rejected: %s", message);
  return accepted;
}
const char* DeskUART::control_reason_(uint32_t now, bool presentation) const {
  if (const char* error = this->journal_error_())
    return error;
  if (this->custom_write_pending_())
    return "Custom timer/buzzer transaction pending";
  if (this->motion_trim_.busy() && this->motion_trim_edit_active_) {
    return "Updating trim settings; awaiting confirmation";
  }
  if (this->reset_speed_.blocking())
    return this->reset_speed_.busy() &&
                   this->reset_speed_.operation() == desk_protocol::ResetSpeed::Speed
               ? "Updating motion speed; awaiting acknowledgement"
               : this->reset_speed_status_(now);
  if (!presentation && this->reset_speed_.busy())
    return "Reading reset and speed status";
  if (this->firmware_export_active_)
    return "Firmware read active; desk controls paused";
  using Phase = desk_protocol::TemporaryPreset::Phase;
  if (this->release_fault_)
    return "Button release uncertain; inspect desk and restart ESP";
  if (!this->monitor_.fresh(now))
    return "No recent height reading; wake the panel or check the controller connection";
  if (this->target_move_.phase() == Phase::Fault)
    return this->target_move_.error();
  if (this->stop_latched_ && !this->stop_acknowledged_)
    return "Stop not confirmed by controller; movement controls blocked";
  if (this->stop_latched_)
    return "Stop: waiting for settled height and preset cleanup";
  if (this->child_lock_busy_() &&
      (!presentation || this->child_lock_detection_.busy() || this->child_lock_.operation() != 0 ||
       !this->child_lock_.state_known()))
    return presentation ? this->child_lock_status_text_(now) : "Panel-lock request in progress";
  if (this->auto_target_mode_ && !this->target_detected_())
    return this->detection_.error() ? this->detection_.error()
                                    : "Checking height control support; target control unavailable";
  if (this->direct_mode_) {
    if (this->direct_.error())
      return this->direct_.error();
    if (this->direct_.awaiting_set())
      return "Waiting for the controller to confirm the target height";
    if (this->direct_.active())
      return "Target height request active; panel buttons may interrupt";
    if (this->direct_.waiting())
      return "Checking direct height control support and movement restrictions";
    if (!this->direct_.known())
      return "Direct height control support not confirmed";
    if (this->direct_.reply().guards)
      return "Controller is blocking movement; select the target again to recheck";
  }
  if (this->keep_awake_fault_)
    return "Keep-awake failed; inspect last command result, restore preset or restart ESP";
  if (this->timer_.busy())
    return "Timer button sequence active; leave physical keys untouched";
  if (this->preset_settings_.busy() && (!presentation || this->preset_settings_.writing()))
    return "Reading or updating preset height";
  if (this->pending_preset_)
    return "Preset waiting for keep-awake cleanup";
  if (this->keep_awake_active_)
    return "Keep-awake preset transaction in progress";
  if (this->target_move_.phase() == Phase::Moving ||
      this->target_move_.phase() == Phase::Monitoring)
    return "Target move active";
  if (this->target_move_.busy())
    return "Preparing target or restoring original preset";
  if (this->pulse_.active() || this->release_ack_.pending())
    return "Button release pending";
  if (this->stable_ms_(now) < 1500)
    return "Waiting for height to settle";
  if (this->limit_phase_ != LimitPhase::Idle &&
      (!presentation || this->limit_phase_ != LimitPhase::Read))
    return "Load setting read or write in progress";
  // Optional capability discovery must not replace normal desk/lock status on
  // older firmware. Once RS01 has replied, losing that capability is a fault.
  if (presentation && this->reset_speed_enabled_() && this->reset_speed_.error() &&
      (this->reset_speed_confirmed_ ||
       this->reset_speed_.operation() != desk_protocol::ResetSpeed::Get))
    return this->reset_speed_.error();
  if (presentation && this->child_lock_enabled_() &&
      (this->child_lock_.error() || !this->child_lock_.state_known() ||
       (this->child_lock_.reply().physical_gate && !this->child_lock_.reply().locked)))
    return this->child_lock_status_text_(now);
  return "Ready";
}
void DeskUART::controls_loop_(uint32_t now) {
  // ACK alone is insufficient. Require post-brake fresh, unchanged raw height,
  // release completion, and verified original-preset cleanup. This is a
  // settling heuristic, not a safety-rated measurement of standstill.
  if (this->stop_latched_)
    this->clear_stop_latch();
  if (this->sync_target_ && !this->target_move_.busy() && !this->stop_latched_ &&
      this->monitor_.fresh(now) && this->stable_ms_(now) >= 1500) {
    this->sync_target_ = false;
    // Keep UART precision, as at startup; the panel's whole-cm display is not a target.
    if (this->target_number_)
      this->target_number_->publish_state(this->display_height(
          this->height_profile_.display(this->monitor_.height().q).before_rounding_tenths_cm /
          10.0f));
  }
  const char* reason = this->control_reason_(now);
  const auto phase = static_cast<uint8_t>(this->target_move_.phase());
  const bool releasing = this->pulse_.active() || this->release_ack_.pending();
  if (reason != this->last_internal_status_ || phase != this->last_internal_phase_ ||
      releasing != this->last_internal_release_) {
    this->last_internal_phase_ = phase;
    this->last_internal_release_ = releasing;
    this->last_internal_status_ = reason;
    ESP_LOGD(TAG, "Internal control status: %s (automatic=%d, phase=%u, release=%d)", reason,
             this->keep_awake_active_, unsigned(this->target_move_.phase()),
             this->pulse_.active() || this->release_ack_.pending());
  }
  // Presentation only. All interlocks and internal phases continue to run.
  // Periodic reads do not replace the desk's status with transaction chatter.
  // Evaluate the other guards rather than holding a possibly stale Ready label.
  reason = this->control_reason_(now, true);
  if (this->keep_awake_active_ && !this->pending_preset_ && !this->keep_awake_fault_ &&
      !this->target_move_.failed() && !this->release_fault_ && !this->stop_latched_ &&
      this->monitor_.fresh(now))
    reason = "Ready";
  if (!this->last_control_status_ || std::strcmp(reason, this->last_control_status_) != 0) {
    this->last_control_status_ = reason;
    if (this->control_status_)
      this->control_status_->publish_state(reason);
  }
  if (this->last_stop_locked_ != this->stop_latched_) {
    this->last_stop_locked_ = this->stop_latched_;
    if (this->stop_locked_sensor_)
      this->stop_locked_sensor_->publish_state(this->stop_latched_);
  }
}
void DeskUART::uart_observation_log_() {
  const uint32_t now = millis();
  this->uart_observation_.tick(now);
  if (!this->uart_observation_enabled_ ||
      static_cast<uint32_t>(now - this->last_uart_observation_log_) < 5000)
    return;
  this->last_uart_observation_log_ = now;
  const auto& s = this->uart_observation_.snapshot();
  char age[16];
  if (s.seen)
    std::snprintf(age, sizeof(age), "%" PRIu32, s.age);
  else
    std::snprintf(age, sizeof(age), "null");
  ESP_LOGD(TAG,
           "UART observation: {\"v\":1,\"boot\":%" PRIu32 ",\"ms\":%" PRIu64 ",\"samples\":%" PRIu64
           ",\"age\":%s,\"seq\":%u,\"q\":%u,\"gaps\":%" PRIu64 ",\"errors\":%" PRIu64
           ",\"tx\":%" PRIu64 ",\"changes\":%" PRIu64 ",\"handoffs\":%" PRIu64 "}",
           s.boot, s.ms, s.samples, age, unsigned(s.sequence), unsigned(s.q), s.gaps, s.errors,
           s.tx, s.changes, s.handoffs);
}

void DeskUART::on_shutdown() {
  this->cancel_motion_trim_("ESPHome shutting down; trim settings unknown", true);
  if (this->reset_speed_.owned() || this->reset_speed_.blocking())
    this->stop();
  this->invalidate_reset_speed_("ESPHome shutting down; new reset and speed status required");
  this->invalidate_child_lock_("ESPHome shutting down; panel-lock state unknown", false, false);
  if (this->direct_mode_ &&
      (this->direct_.active() || this->direct_.set_sent() || this->direct_.brake_pending()))
    this->stop();
  desk_protocol::Frame frame;
  this->timer_.cancel(millis());
  if (this->timer_.take_frame(frame))
    this->send_(frame);
  if (this->pulse_.cancel(frame))
    this->send_(frame);
  desk_protocol::telemetry(2, 0xFFFF, frame);
  this->send_(frame);
  this->flush();
}
void DeskUART::dump_config() {
  ESP_LOGCONFIG(TAG, "Source: %s, %s (%s)", DESK_SOURCE_VERSION, DESK_SOURCE_REVISION,
                DESK_SOURCE_STATE);
  ESP_LOGCONFIG(TAG, "Desk UART: T150-PJ-V1.5 experimental metric profile");
  ESP_LOGCONFIG(TAG, "Display unit: %s (independent of panel setting)",
                this->display_inches_ ? "in" : "cm");
  ESP_LOGCONFIG(TAG, "  Telemetry: channel 2, 500 ticks; stale after 3000 ms");
  ESP_LOGCONFIG(TAG, "  Target height mode: %s",
                this->auto_target_mode_ ? "auto"
                : this->direct_mode_    ? "direct DT02 (runtime handshake)"
                                        : "preset 3");
  ESP_LOGCONFIG(TAG, "  Keep-awake policy: %s",
                this->keep_awake_auto_ ? "auto (direct: sleep feature; preset: legacy workaround)"
                : this->keep_awake_enabled_ ? "true (legacy preset)"
                                            : "false");
  ESP_LOGCONFIG(TAG, "  Experimental preset buttons: %s",
                this->motion_enabled_ ? "enabled" : "disabled");
  ESP_LOGCONFIG(TAG, "  Experimental custom timer/buzzer: %s",
                this->custom_enabled_ ? "enabled" : "disabled");
  ESP_LOGCONFIG(TAG, "  Experimental T-key timer: %s",
                this->timer_enabled_ ? "enabled" : "disabled");
  ESP_LOGCONFIG(TAG, "  Keep-awake via preset 3 after 30 s of inactivity: %s",
                this->keep_awake_enabled_ ? "enabled" : "disabled");
  LOG_SENSOR("  ", "Height", this->height_sensor_);
  LOG_SENSOR("  ", "Load (raw)", this->load_raw_sensor_);
  LOG_SELECT("  ", "Motion speed", this->motion_speed_select_);
  for (auto* number : this->motion_trim_numbers_) {
    LOG_NUMBER("  ", "Motion trim", number);
  }
  LOG_TEXT_SENSOR("  ", "Motion trim status", this->motion_trim_status_);
  LOG_TEXT_SENSOR("  ", "Reset status", this->reset_status_);
  LOG_BINARY_SENSOR("  ", "Reset available", this->reset_available_);
  LOG_BINARY_SENSOR("  ", "Motion speed available", this->motion_speed_available_);
  LOG_SWITCH("  ", "Physical child lock", this->child_lock_switch_);
  LOG_TEXT_SENSOR("  ", "Child lock status", this->child_lock_status_);
  LOG_BINARY_SENSOR("  ", "Child lock available", this->child_lock_available_);
  LOG_TEXT_SENSOR("  ", "Controller model", this->identity_sensors_[0]);
  LOG_TEXT_SENSOR("  ", "Controller manufacturer", this->identity_sensors_[1]);
  LOG_TEXT_SENSOR("  ", "Controller identifiers", this->identity_sensors_[2]);
  LOG_TEXT_SENSOR("  ", "Controller date", this->identity_sensors_[3]);
  LOG_BINARY_SENSOR("  ", "UART connected", this->connected_sensor_);
  LOG_SENSOR("  ", "UART frame errors", this->frame_errors_sensor_);
  LOG_SENSOR("  ", "Height sample gap", this->sample_gap_sensor_);
  this->check_uart_settings(115200);
}
}  // namespace desk_uart
}  // namespace esphome
