#include "desk_uart.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace esphome { namespace desk_uart {
static const char *const TAG = "desk_uart";
// Interruptions caused by a successful Stop are logged, not published as
// command results, so they cannot replace that Stop's own result.
static const char *const STOP_PRESET_REASON = "Preset setting interrupted by Stop; readback required";

bool DeskUART::begin_firmware_export() {
  const uint32_t now = millis();
  if (this->firmware_export_active_ || !this->preset_access_allowed_(now) ||
      this->preset_settings_.busy() || this->target_move_.dirty() || this->keep_awake_active_ ||
      this->keep_awake_fault_ || this->recovery_check_active_ || this->accept_current_active_ ||
      this->restore_requested_ || this->identity_pending_ != 255) return false;
  this->invalidate_journal_session_();
  this->firmware_export_active_ = true;
  this->parser_.reset();
  this->monitor_ = {};
  this->height_seen_ = this->connected_ = false;
  if (this->connected_sensor_) this->connected_sensor_->publish_state(false);
  this->height_sensor_->publish_state(NAN);
  if (this->control_status_) this->control_status_->publish_state("Firmware read active; desk controls paused");
  this->last_control_status_ = nullptr;
  return true;
}
void DeskUART::end_firmware_export() {
  if (!this->firmware_export_active_) return;
  this->invalidate_journal_session_();
  this->parser_.reset();
  this->monitor_ = {};
  this->height_seen_ = this->connected_ = false;
  this->last_keep_awake_ = millis();
  this->last_subscribe_ = millis() - 5000;
  this->last_control_status_ = nullptr;
  this->firmware_export_active_ = false;
}

void DeskUART::send_(const desk_protocol::Frame &frame) {
  if (this->firmware_export_active_) return;
  desk_protocol::Packet wire;
  if (desk_protocol::encode(frame, wire)) this->write_array(wire.bytes, wire.size);
}
// Optional, bounded background reads. No identity query participates in motion
// interlocks, ACK trackers, preset recovery or the keep-awake mechanism.
static const uint8_t IDENTITY_SELECTORS[6] = {3, 4, 5, 6, 7, 9};
static const uint8_t IDENTITY_ENTITIES[6] = {0, 1, 2, 2, 2, 3};
void DeskUART::identity_loop_(uint32_t now) {
  if (this->identity_pending_ != 255) {
    if (static_cast<uint32_t>(now - this->identity_sent_at_) < 700) return;
    this->identity_pending_ = 255;
  }
  if (static_cast<uint32_t>(now - this->identity_sent_at_) < 1000 ||
      !this->preset_access_allowed_(now) || this->preset_settings_.busy() || this->target_move_.dirty()) return;
  for (uint8_t offset = 0; offset < 6; ++offset) {
    const uint8_t index = (this->identity_next_ + offset) % 6;
    if (!this->identity_sensors_[IDENTITY_ENTITIES[index]] ||
        (this->identity_received_ & (1U << index)) || this->identity_attempts_[index] >= 3) continue;
    desk_protocol::Frame query;
    if (!desk_protocol::read_identity(IDENTITY_SELECTORS[index], query)) return;
    this->identity_pending_ = index;
    this->identity_next_ = (index + 1) % 6;
    ++this->identity_attempts_[index];
    this->identity_sent_at_ = now;
    this->send_(query);
    return;
  }
}
void DeskUART::identity_frame_(const desk_protocol::Frame &frame, uint32_t now) {
  if (this->identity_pending_ == 255 || static_cast<uint32_t>(now - this->identity_sent_at_) >= 700) return;
  desk_protocol::IdentityText value;
  const uint8_t index = this->identity_pending_;
  if (!desk_protocol::decode_identity(frame, value) || value.selector != IDENTITY_SELECTORS[index]) return;
  this->identity_pending_ = 255;
  this->identity_received_ |= 1U << index;
  if (index >= 2 && index <= 4) {
    std::memcpy(this->identity_values_[index - 2], value.text, sizeof(value.text));
    if ((this->identity_received_ & 0x1C) != 0x1C) return;
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
  this->height_sensor_->publish_state(NAN);
  for (auto *n : this->preset_numbers_) if (n) n->publish_state(NAN);
  if (this->connected_sensor_) this->connected_sensor_->publish_state(false);
  if (this->frame_errors_sensor_) this->frame_errors_sensor_->publish_state(0);
  if (this->sample_gap_sensor_) this->sample_gap_sensor_->publish_state(NAN);
  this->gap_window_start_ = millis();
  {  // Recovery records remain relevant even after target controls are disabled.
    this->preset_preference_ = global_preferences->make_preference<PresetJournal>(this->preference_key_, true);
    PresetJournal journal{};
    if (this->preset_preference_.load(&journal) &&
        (journal.magic == 0x44534B31 || journal.magic == 0x44534B32 || journal.magic == 0x44534B33)) {
      // Same key and eight-byte layout: legacy records remain recoverable.
      this->cached_journal_ = journal;
      this->cached_journal_valid_ = true;
      this->target_move_.recover(journal.original, journal.temporary);
      this->keep_awake_recovery_ = true;
      this->last_target_error_ = this->target_move_.error();
      ESP_LOGD(TAG, "Checking retained preset backup after startup; EEPROM unverified");
    }
    if (this->target_number_) this->target_number_->publish_state(NAN);
  }
  if (this->restore_pending_sensor_) this->restore_pending_sensor_->publish_state(this->target_move_.dirty());
  if (this->timer_pending_sensor_) this->timer_pending_sensor_->publish_state(false);
  if (this->stop_locked_sensor_) this->stop_locked_sensor_->publish_state(false);
  this->last_keep_awake_ = millis();
  this->subscribe_(millis());
}
void DeskUART::release_(const desk_protocol::Frame &frame, uint32_t now) {
  this->release_ack_.begin(0x18, frame.bytes[1], 0, now);
  this->send_(frame);
}
void DeskUART::loop() {
  if (this->firmware_export_active_) return;
  const uint32_t loop_start = millis();
  if (this->height_seen_ && !this->monitor_.fresh(loop_start)) this->invalidate_journal_session_();
  desk_protocol::Frame frame;
  // Release has priority even if network/telemetry has gone away.
  if (this->pulse_.poll(loop_start, frame)) this->release_(frame, loop_start);
  this->timer_.poll_release(loop_start);
  if (this->timer_.take_frame(frame)) this->send_(frame);
  this->parser_.expire(loop_start);
  // Bound work per loop so an RX flood cannot starve the pulse timer.
  for (unsigned budget = 0; budget < 512 && this->available(); ++budget) {
    uint8_t byte;
    if (!this->read_byte(&byte)) break;
    const uint32_t now = millis();
    if (this->height_seen_ && !this->monitor_.fresh(now)) this->invalidate_journal_session_();
    const auto parsed = this->parser_.feed(byte, now, frame);
    if (parsed == desk_protocol::ParseResult::BadChecksum || parsed == desk_protocol::ParseResult::BadLength ||
        parsed == desk_protocol::ParseResult::Overflow)
      ++this->frame_errors_;
    if (parsed != desk_protocol::ParseResult::FrameReady) continue;
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
    if (this->monitor_.observe(frame, now)) {
      const uint16_t q = this->monitor_.height().q;
      if (this->height_seen_ && static_cast<uint32_t>(now - this->last_height_received_) > 750)
        this->discard_pending_preset_("Pending preset cancelled by telemetry gap; press again");
      if (this->height_seen_ && static_cast<uint32_t>(now - this->last_height_received_) > this->max_sample_gap_)
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
      const auto value = desk_protocol::metric_display(q);
      if (this->target_number_ && !this->target_initialized_) {
        this->target_initialized_ = true;
        this->target_number_->publish_state(this->display_height(value.before_rounding_tenths_cm / 10.0f));
      }
      // Convert before the metric panel's whole-centimetre truncation above 100 cm.
      const auto tenths = this->display_inches_ ? value.before_rounding_tenths_cm : value.displayed_tenths_cm;
      this->height_sensor_->publish_state(this->display_height(tenths / 10.0f));
    }
  }
  // publish_state() may synchronously invoke an automation that calls stop().
  // Use a new timestamp so an ACK timer started in that callback cannot appear
  // to have wrapped around relative to the beginning of this loop.
  const uint32_t now = millis();
  // RX processing or synchronous callbacks may cross the connection deadline.
  // Freeze recovery before any transaction can send or complete queued work.
  if (this->height_seen_ && !this->monitor_.fresh(now)) this->invalidate_journal_session_();
  if (this->release_ack_.expired(now)) {
    this->release_fault_ = true;
    if (this->target_move_.busy()) this->stop();
    this->status_set_warning();
    ESP_LOGE(TAG, "No release ACK; further remote presets disabled until ESP restart. Check controller locally.");
  }
  if (this->stop_ack_.expired(now)) {
    this->status_set_warning();
    ESP_LOGE(TAG, "No brake ACK; no retry. Remote presets remain locked. Check desk locally.");
  }
  this->keep_awake_loop_(now);
  this->target_loop_(now);
  this->pending_preset_loop_(now);
  this->timer_loop_(now);
  this->preset_settings_loop_(now);
  this->limit_loop_(now);
  this->controls_loop_(now);
  this->identity_loop_(now);
  const bool fresh = this->monitor_.fresh(now);
  if (fresh != this->connected_) {
    this->connected_ = fresh;
    if (this->connected_sensor_) this->connected_sensor_->publish_state(fresh);
    if (!fresh) { this->height_seen_ = false; this->height_sensor_->publish_state(NAN); }
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
      this->sample_gap_sensor_->publish_state(this->max_sample_gap_ ? static_cast<float>(this->max_sample_gap_) : NAN);
    this->max_sample_gap_ = 0;
    this->gap_window_start_ = now;
  }
  // Retrying a subscription is not a wake guarantee. Never retry motion or
  // queue a requested movement across loss of communication.
  if (!fresh && !this->timer_.busy() && !this->pulse_.active() && !this->release_ack_.pending() && !this->stop_ack_.pending() &&
      static_cast<uint32_t>(now - this->last_subscribe_) >= 5000) this->subscribe_(now);
}
bool DeskUART::recall_preset(uint8_t slot) {
  if (this->firmware_export_active_) return this->result_(false, "Firmware read active; desk controls paused");
  const uint32_t now = millis();
  if (this->journal_recheck_pending_) return this->result_(false, "Preset backup needs a fresh read after connection loss");
  if (!this->motion_enabled_ || slot < 1 || slot > 3) return this->result_(false, "Preset controls disabled or invalid preset");
  this->last_keep_awake_ = now;
  if (this->keep_awake_active_ && !this->keep_awake_fault_ && !this->target_move_.failed() &&
      !this->stop_latched_ && !this->release_fault_ && this->monitor_.fresh(now) &&
      static_cast<uint32_t>(now - this->last_height_received_) <= 750 &&
      this->target_move_.yield_keep_awake(now)) {
    // A yielded dirty transaction still needs the stale-link guard through
    // final readback, including after the pending button has been cancelled.
    if (this->target_move_.dirty()) this->soft_abort_error_ = "Keep-awake yielded to preset button";
    // Latest button wins, but repeated presses never extend the deadline.
    if (!this->pending_preset_) {
      this->pending_preset_started_ = now;
      this->pending_preset_q_ = this->last_q_;
    }
    this->pending_preset_ = slot;
    this->result_(true, "Preset waiting for keep-awake cleanup (maximum 4 seconds)");
    return this->pending_preset_loop_(now);
  }
  if (this->preset_settings_.busy() || this->retarget_pending_ || this->timer_.busy() || this->target_move_.busy() || this->limit_phase_ != LimitPhase::Idle || this->stop_latched_ || !this->motion_enabled_ || this->release_fault_ || !this->monitor_.fresh(now) ||
      this->release_ack_.pending() || slot < 1 || slot > 3) {
    return this->result_(false, this->control_reason_(now));
  }
  desk_protocol::Frame press;
  if (!this->pulse_.begin(static_cast<desk_protocol::Button>(slot + 5), now, press)) return this->result_(false, "Preset button pulse already active");
  this->last_preset_recall_ = now;
  this->send_(press);
  return this->result_(true, "Preset button sent");
}
void DeskUART::discard_pending_preset_(const char *reason) {
  if (!this->pending_preset_) return;
  this->pending_preset_ = 0;
  this->result_(false, reason);
}
bool DeskUART::pending_preset_loop_(uint32_t now) {
  if (!this->pending_preset_) return false;
  if (this->journal_recheck_pending_ || !this->motion_enabled_ || this->firmware_export_active_ || this->stop_latched_ ||
      this->release_fault_ || this->keep_awake_fault_ || this->target_move_.failed() ||
      !this->monitor_.fresh(now) || static_cast<uint32_t>(now - this->last_height_received_) > 750 ||
      this->last_q_ != this->pending_preset_q_ ||
      static_cast<uint32_t>(now - this->pending_preset_started_) >= 4000) {
    this->discard_pending_preset_("Pending preset cancelled by fault, lost telemetry or timeout; press again");
    return false;
  }
  if (this->target_move_.busy() || this->target_move_.dirty() || this->pulse_.active() ||
      this->release_ack_.pending() || this->stop_ack_.pending()) return true;
  const uint8_t slot = this->pending_preset_;
  this->pending_preset_ = 0;
  this->keep_awake_active_ = false;
  return this->recall_preset(slot);
}
bool DeskUART::stop() {
  if (this->firmware_export_active_) return this->result_(false, "Firmware read active; desk controls paused");
  this->discard_pending_preset_("Pending preset cancelled by Stop");
  // A user's Stop (and any fault brake) must never inherit quiet presentation.
  this->keep_awake_active_ = false;
  this->soft_abort_error_ = nullptr;
  this->last_keep_awake_ = millis();
  // Local synchronous transmission: no subscription, ACK wait or motion queue.
  // Repeated detector callbacks must not continually move the braking target.
  this->preset_settings_.cancel(STOP_PRESET_REASON);
  this->discard_retarget_("Pending target cancelled by Stop");
  this->sync_target_ = true;
  const uint32_t now = millis();
  if (this->accept_current_active_ || this->recovery_check_active_ || this->restore_requested_)
    this->target_move_.abort_recovery(now);
  // Suppress a burst of duplicate requests, not all later explicit Stops.
  // A missing ACK or preset conflict must not permanently disable braking.
  if (this->stop_latched_ && (this->stop_ack_.pending() ||
      static_cast<uint32_t>(now - this->last_stop_) < 2000))
    return this->result_(false, this->control_reason_(now));
  const bool target_failed = this->target_move_.failed();
  this->target_move_.cancel(now);
  if (!target_failed && this->target_move_.failed()) {
    this->last_target_error_ = this->target_move_.error();
    ESP_LOGW(TAG, "%s", this->last_target_error_);
  }
  const char *timer_error = this->timer_.error();
  this->timer_.cancel(now);
  if (this->timer_.error() != timer_error) {
    this->last_timer_error_ = this->timer_.error();
    ESP_LOGW(TAG, "%s", this->last_timer_error_);
  }
  desk_protocol::Frame timer_release;
  if (this->timer_.take_frame(timer_release)) this->send_(timer_release);
  this->stop_latched_ = true;
  this->stop_acknowledged_ = false;
  this->last_stop_ = now;
  this->stop_ack_.begin(0x18, 1, 0, now);
  // A stop never waits behind parameter ACK/readback. Do not queue a write retry.
  if (this->limit_phase_ != LimitPhase::Idle) {
    ESP_LOGW(TAG, "Parameter transaction interrupted by stop; current setting needs refresh");
    this->limit_phase_ = LimitPhase::Idle;
    if (this->limit_numbers_[this->limit_index_]) this->limit_numbers_[this->limit_index_]->publish_state(NAN);
  }
  this->last_limit_poll_ = now;
  desk_protocol::Frame release;
  const bool cancelled_pulse = this->pulse_.cancel(release);
  if (cancelled_pulse) this->release_(release, now);
  this->send_(desk_protocol::stop());
  if (cancelled_pulse) {
    ESP_LOGW(TAG, "Released pending preset before brake. Controller may still have a delayed button event; check locally.");
  }
  return this->result_(true, "Braking; waiting for ACK, settled height and preset cleanup");
}
bool DeskUART::clear_stop_latch() {
  if (this->firmware_export_active_) return this->result_(false, "Firmware read active; desk controls paused");
  const uint32_t now = millis();
  if (!this->stop_latched_ || !this->stop_acknowledged_ || this->stop_ack_.pending() ||
      this->timer_.busy() || this->pulse_.active() || this->release_ack_.pending() || this->release_fault_ ||
      !this->monitor_.fresh(now) || static_cast<uint32_t>(now - this->last_height_received_) > 750 || this->stable_ms_(now) < 1500 ||
      this->target_move_.busy() || this->target_move_.dirty() || static_cast<uint32_t>(now - this->last_stop_) < 2000)
    return false;
  // No motion is sent, and no request is replayed by clearing the interlock.
  this->stop_latched_ = false;
  return this->result_(true, "Stop completed; remote controls ready");
}
void LoadLimitNumber::control(float value) {
  if (!std::isfinite(value) || value < 1 || value > 65535 || std::floor(value) != value) {
    ESP_LOGW(TAG, "Load limit must be an integer1..65535 raw units");
    return;
  }
  parent_->set_load_limit(index_ == 0 ? desk_protocol::LoadLimit::Upward : desk_protocol::LoadLimit::DownwardBase,
                          static_cast<uint16_t>(value));
}
void DeskUART::query_limit_(uint8_t index, uint32_t now, bool verify) {
  desk_protocol::Frame f;
  desk_protocol::read_load_limit(index == 0 ? desk_protocol::LoadLimit::Upward : desk_protocol::LoadLimit::DownwardBase, f);
  this->limit_index_ = index;
  this->limit_phase_ = verify ? LimitPhase::Verify : LimitPhase::Read;
  this->limit_started_ = now;
  this->last_limit_poll_ = now;
  this->send_(f);
}
bool DeskUART::set_load_limit(desk_protocol::LoadLimit limit, uint16_t raw) {
  if (this->firmware_export_active_) return this->result_(false, "Firmware read active; desk controls paused");
  if (!desk_protocol::valid_load_limit(limit) || raw == 0) return false;
  const uint8_t index = limit == desk_protocol::LoadLimit::Upward ? 0 : 1;
  const uint32_t now = millis();
  this->last_keep_awake_ = now;
  if (!this->limit_numbers_[index] || !this->original_known_[index] ||
      this->preset_settings_.busy() || !this->preset_access_allowed_(now)) {
    return this->result_(false, (!this->limit_numbers_[index] || !this->original_known_[index]) ? "Load setting not configured or original not yet read" :
      "Load edit blocked: moving, recent recall, stale telemetry or another operation; see control status");
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
  if (this->firmware_export_active_) return this->result_(false, "Firmware read active; desk controls paused");
  if (!desk_protocol::valid_load_limit(limit)) return false;
  const uint8_t i = limit == desk_protocol::LoadLimit::Upward ? 0 : 1;
  return this->original_known_[i] && this->set_load_limit(limit, this->original_limits_[i]);
}
void DeskUART::limit_frame_(const desk_protocol::Frame &f, uint32_t now) {
  if (this->limit_phase_ == LimitPhase::Idle || static_cast<uint32_t>(now - this->limit_started_) >= 700) return;
  const auto selected = this->limit_index_ == 0 ? desk_protocol::LoadLimit::Upward : desk_protocol::LoadLimit::DownwardBase;
  if (this->limit_phase_ == LimitPhase::WriteAck) {
    desk_protocol::Ack a;
    if (!desk_protocol::decode_ack(f, a) || a.kind != 0x1A || a.first != static_cast<uint8_t>(selected)) return;
    if (a.second != 0) {
      this->limit_phase_ = LimitPhase::Idle;
      ESP_LOGE(TAG, "Load-limit write rejected by controller; no retry");
      return;
    }
    this->query_limit_(this->limit_index_, now, true);
    return;
  }
  desk_protocol::LoadLimitValue value;
  if (!desk_protocol::decode_load_limit(f, value) || value.limit != selected) return;
  if (this->limit_phase_ == LimitPhase::Verify) {
    if (value.raw == this->expected_limit_) ESP_LOGI(TAG, "Load limit%u verified: %u", this->limit_index_, value.raw);
    else ESP_LOGE(TAG, "Load limit%u readback mismatch: requested%u actual%u; no retry", this->limit_index_, this->expected_limit_, value.raw);
  }
  if (!this->original_known_[this->limit_index_]) {
    this->original_limits_[this->limit_index_] = value.raw;
    this->original_known_[this->limit_index_] = true;
  }
  const uint8_t index = this->limit_index_;
  this->limit_phase_ = LimitPhase::Idle;
  this->next_limit_ = 1 - index;
  if (this->limit_numbers_[index]) this->limit_numbers_[index]->publish_state(value.raw);
}
void DeskUART::limit_loop_(uint32_t now) {
  if (this->limit_phase_ != LimitPhase::Idle) {
    if (static_cast<uint32_t>(now - this->limit_started_) >= 700) {
      ESP_LOGE(TAG, "Load-limit transaction timeout; no write retry");
      this->limit_phase_ = LimitPhase::Idle;
      this->last_limit_poll_ = now;
      if (this->limit_numbers_[this->limit_index_]) this->limit_numbers_[this->limit_index_]->publish_state(NAN);
    }
    return;
  }
  if (this->preset_settings_.busy() || this->retarget_pending_ || this->timer_.busy() || this->target_move_.busy() || !this->monitor_.fresh(now) || this->pulse_.active() || this->release_ack_.pending() || this->stop_ack_.pending()) return;
  for (uint8_t offset = 0; offset < 2; ++offset) {
    const uint8_t i = (this->next_limit_ + offset) % 2;
    if (this->limit_numbers_[i] && static_cast<uint32_t>(now - this->last_limit_poll_) >= (this->original_known_[i] ? 5000U : 1000U)) {
      this->query_limit_(i, now);
      return;
    }
  }
}
bool DeskUART::preset_access_allowed_(uint32_t now) const {
  return !this->journal_recheck_pending_ && !this->pending_preset_ && !this->firmware_export_active_ && this->monitor_.fresh(now) && this->stable_ms_(now) >= 1500 &&
         static_cast<uint32_t>(now - this->last_height_received_) <= 750 &&
         static_cast<uint32_t>(now - this->last_preset_recall_) >= 3000 &&
         !this->target_move_.busy() && !this->retarget_pending_ && !this->timer_.busy() &&
         !this->stop_latched_ && !this->release_fault_ && !this->pulse_.active() &&
         !this->release_ack_.pending() && !this->stop_ack_.pending() && this->limit_phase_ == LimitPhase::Idle;
}
bool DeskUART::set_preset_height(uint8_t slot, float cm) {
  if (this->firmware_export_active_) return this->result_(false, "Firmware read active; desk controls paused");
  const uint32_t now = millis();
  this->last_keep_awake_ = now;
  if (slot < 1 || slot > 3 || !this->preset_numbers_[slot - 1] || !std::isfinite(cm) ||
      cm < this->min_height_ || cm > this->max_height_)
    return this->result_(false, "Preset height not configured or outside configured range");
  if (!this->preset_known_[slot - 1]) return this->result_(false, "Read original preset height before editing");
  if (this->preset_settings_.busy() || !this->preset_access_allowed_(now))
    return this->result_(false, "Preset edit blocked: moving, recent recall, stale telemetry or another operation; see control status");
  if (slot == 3 && this->cached_journal_valid_)
    return this->result_(false, "Accept current preset first to discard the retained backup before editing preset 3");
  if (!this->preset_settings_.begin_write(slot, static_cast<uint16_t>(std::lround(cm * 10)), now)) return false;
  this->preset_known_[slot - 1] = false;
  this->preset_numbers_[slot - 1]->publish_state(NAN);
  this->result_(true, "Preset edit accepted; waiting for scale check and readback, no movement");
  this->preset_settings_loop_(now);
  return true;
}
void DeskUART::preset_settings_loop_(uint32_t now) {
  if (!this->monitor_.fresh(now)) {
    for (uint8_t i = 0; i < 3; ++i) {
      if (this->preset_known_[i] && this->preset_numbers_[i]) this->preset_numbers_[i]->publish_state(NAN);
      this->preset_known_[i] = false;
    }
  }
  if (this->preset_settings_.busy() && !this->preset_access_allowed_(now))
    this->preset_settings_.cancel("Preset setting interrupted by movement, stale telemetry or another operation; no retry");
  this->preset_settings_.tick(now);
  desk_protocol::PresetSettings::Result r{};
  if (this->preset_settings_.take_result(r)) {
    if (r.slot == 3 && r.value_known && !r.write && !r.error) this->check_retained_preset_(r.raw);
    const uint8_t i = r.slot - 1;
    uint16_t tenths;
    this->preset_known_[i] = r.value_known;
    const bool mapped = r.value_known && desk_protocol::preset_height(r.raw, tenths);
    this->next_preset_ = (i + 1) % 3;
    this->last_preset_poll_ = now;
    if (r.error == STOP_PRESET_REASON) ESP_LOGW(TAG, "%s", r.error);
    else if (r.error) this->result_(false, r.error);
    else if (!mapped) this->result_(false, "Preset raw value outside supported metric range; enter a valid height to replace it");
    else if (r.write) this->result_(true, "Preset height written and verified; no movement");
    // Only actual values from the controller are published, never the requested
    // value. A mismatch displays the returned value and retains an error message.
    if (this->preset_numbers_[i]) this->preset_numbers_[i]->publish_state(mapped ? this->display_height(tenths / 10.0f) : NAN);
  }
  desk_protocol::Frame frame;
  if (this->preset_settings_.take_frame(frame)) this->send_(frame);
  if (this->preset_settings_.busy() || !this->preset_access_allowed_(now)) return;
  for (uint8_t offset = 0; offset < 3; ++offset) {
    const uint8_t i = (this->next_preset_ + offset) % 3;
    if (this->preset_numbers_[i] && static_cast<uint32_t>(now - this->last_preset_poll_) >= (this->preset_known_[i] ? 10000U : 1000U)) {
      this->last_preset_poll_ = now;
      this->preset_settings_.begin_read(i + 1, now);
      if (this->preset_settings_.take_frame(frame)) this->send_(frame);
      return;
    }
  }
}
bool DeskUART::clear_cached_journal_() {
  if (!this->cached_journal_valid_) return true;
  PresetJournal empty{};
  if (!this->preset_preference_.save(&empty) || !global_preferences->sync()) return false;
  this->cached_journal_valid_ = false;
  return true;
}
void DeskUART::invalidate_journal_session_() {
  if (!this->cached_journal_valid_ || this->journal_recheck_pending_) return;
  this->verified_journal_.valid = false;
  this->journal_recheck_pending_ = true;
  this->target_move_.connection_lost(millis());
  this->accept_current_active_ = this->restore_requested_ = this->recovery_check_active_ = false;
  this->keep_awake_recovery_ = false;
  this->discard_pending_preset_("Pending preset cancelled by connection loss; press again");
  this->discard_retarget_("Pending target cancelled by connection loss; request again");
}
void DeskUART::check_retained_preset_(uint16_t preset3_raw) {
  if (!this->cached_journal_valid_ || preset3_raw == this->cached_journal_.original ||
      this->target_move_.busy()) return;
  // A controller reboot and a legitimate local save are indistinguishable.
  // Keep the baseline and require a decision instead of deleting it.
  this->target_move_.recover(this->cached_journal_.original, this->cached_journal_.temporary,
        "Preset 3 differs from retained backup; restore backup or accept current preset");
  this->verified_journal_.valid = false;
  this->keep_awake_recovery_ = false;
  ESP_LOGW(TAG, "Preset 3 differs from retained backup: current=%u original=%u; restore or accept explicitly",
           preset3_raw, this->cached_journal_.original);
}
bool DeskUART::save_journal_(bool active) {
  this->verified_journal_.valid = false;
  if (!active) {
    // Completing a RAM transaction cannot establish EEPROM persistence.
    // Only explicit, double-read acceptance discards the retained baseline.
    return !this->accept_current_active_ || this->clear_cached_journal_();
  }
  if (this->cached_journal_valid_ &&
      (this->journal_recheck_pending_ || this->cached_journal_.original != this->target_move_.original())) {
    this->target_move_.recover(this->cached_journal_.original, this->cached_journal_.temporary,
        "Preset 3 differs from retained backup; restore backup or accept current preset");
    this->keep_awake_recovery_ = false;
    ESP_LOGW(TAG, "Retained preset backup conflicts with new borrow; no write sent");
    return false;
  }
  PresetJournal journal{0x44534B33U, this->target_move_.original(), this->target_move_.target()};
  if (this->cached_journal_valid_ && this->cached_journal_.magic == journal.magic &&
      this->cached_journal_.original == journal.original && this->cached_journal_.temporary == journal.temporary)
    return true;
  if (!this->preset_preference_.save(&journal) || !global_preferences->sync()) return false;
  this->cached_journal_ = journal;
  this->cached_journal_valid_ = true;
  ESP_LOGD(TAG, "Preset recovery journal committed (magic=%08x)", unsigned(journal.magic));
  return true;
}
void DeskUART::keep_awake_loop_(uint32_t now) {
  using Phase = desk_protocol::TemporaryPreset::Phase;
  // Wait for release/braking and fresh settled height before inspecting a
  // retained record. Never replace a running motion state during RX handling.
  if (this->journal_recheck_pending_ && this->cached_journal_valid_ && !this->target_move_.brake_pending() && this->recovery_access_allowed_(now) &&
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
  if (this->keep_awake_active_) {
    const Phase phase = this->target_move_.phase();
    const bool before_or_during_recall = phase == Phase::CurrentRaw || phase == Phase::Original ||
        phase == Phase::Backup || phase == Phase::Write || phase == Phase::Verify ||
        phase == Phase::CheckRaw || phase == Phase::Recall || phase == Phase::Moving;
    if (before_or_during_recall && (this->last_q_ != this->keep_awake_q_ ||
        !this->monitor_.fresh(now) || static_cast<uint32_t>(now - this->last_height_received_) > 750)) {
      // The press, not the phase name, decides whether motion may have begun.
      this->target_move_.abort_keep_awake(now, this->keep_awake_pressed_);
    }
    return;
  }
  if (this->journal_recheck_pending_ || this->pending_preset_ || !this->keep_awake_enabled_ || !this->target_enabled_ || this->keep_awake_fault_ ||
      this->preset_settings_.busy() || !this->preset_access_allowed_(now) ||
      static_cast<uint32_t>(now - this->last_keep_awake_) < 30000) return;
  const auto height = desk_protocol::metric_display(this->last_q_).before_rounding_tenths_cm / 10.0f;
  if (height < this->min_height_ || height > this->max_height_) return;
  if (!this->target_move_.begin_keep_awake(now)) return;
  this->keep_awake_active_ = true;
  this->keep_awake_pressed_ = false;
  this->keep_awake_q_ = this->last_q_;
  this->last_keep_awake_ = now;
  this->last_target_error_ = nullptr;
  ESP_LOGD(TAG, "Keep-awake: reading current raw position and borrowing preset 3");
}
bool DeskUART::move_to_height(float cm) {
  if (this->firmware_export_active_) return this->result_(false, "Firmware read active; desk controls paused");
  const uint32_t now = millis();
  this->last_keep_awake_ = now;
  if (!this->target_enabled_ || !std::isfinite(cm) || cm < this->min_height_ || cm > this->max_height_)
    return this->result_(false, "Target outside configured range or target control disabled");
  if (this->journal_recheck_pending_)
    return this->result_(false, "Retained preset backup needs a fresh read after connection loss");
  if (this->keep_awake_active_)
    return this->result_(false, "Keep-awake preset transaction in progress; request again after restoration");
  // Only an already active slider move may be replaced. Never queue across a
  // stale link, timer operation, boot recovery or a failed transaction.
  if (this->preset_settings_.busy() || this->release_fault_ || !this->monitor_.fresh(now) || this->timer_.busy())
    return this->result_(false, this->control_reason_(now));
  if (this->retarget_pending_) {
    this->retarget_cm_ = cm;  // latest wins; do not extend the original deadline
    return this->result_(true, "Latest target selected; waiting for brake and preset cleanup");
  }
  using Phase = desk_protocol::TemporaryPreset::Phase;
  if ((this->target_move_.phase() == Phase::Moving || this->target_move_.phase() == Phase::Monitoring) && !this->stop_latched_) {
    // Identical HA updates must not interrupt an otherwise valid move.
    uint16_t raw;
    desk_protocol::metric_target_raw(static_cast<uint16_t>(std::lround(cm * 10)), raw);
    if (raw == this->target_move_.target()) return this->result_(true, "Target already active");
    this->stop();
    this->retarget_cm_ = cm;
    this->retarget_started_ = now;
    this->retarget_pending_ = true;
    return this->result_(true, "New target accepted; braking before a separate recall");
  }
  if (this->stop_latched_ || this->stable_ms_(now) < 1500 ||
      this->pulse_.active() || this->release_ack_.pending() || this->stop_ack_.pending() ||
      this->limit_phase_ != LimitPhase::Idle || this->target_move_.busy())
    return this->result_(false, this->control_reason_(now));
  const auto tenths = static_cast<uint16_t>(std::lround(cm * 10.0f));
  if (!this->target_move_.begin(tenths, now)) return this->result_(false, "Target transaction refused");
  this->last_target_error_ = nullptr;
  this->sync_target_ = true;
  ESP_LOGI(TAG, "Requested target %.1f cm; preserving preset 3", tenths / 10.0f);
  this->target_loop_(now);
  return this->result_(true, "Target accepted");
}
bool DeskUART::step_height(bool up) {
  if (this->firmware_export_active_) return this->result_(false, "Firmware read active; desk controls paused");
  const uint32_t now = millis();
  if (!this->monitor_.fresh(now)) return this->result_(false, this->control_reason_(now));
  const float current = desk_protocol::metric_display(this->monitor_.height().q).before_rounding_tenths_cm / 10.0f;
  float target = current + (up ? 1.0f : -1.0f);
  if (target < this->min_height_) target = this->min_height_;
  if (target > this->max_height_) target = this->max_height_;
  if ((up ? target <= current : target >= current) || std::fabs(target - current) < 0.05f)
    return this->result_(false, "Step outside configured height range");
  if (!this->move_to_height(target)) return false;
  if (this->target_number_) this->target_number_->publish_state(this->display_height(target));
  return true;
}
bool DeskUART::restore_temporary_preset() {
  if (this->firmware_export_active_) return this->result_(false, "Firmware read active; desk controls paused");
  const uint32_t now = millis();
  this->last_keep_awake_ = now;
  if (!this->recovery_access_allowed_(now)) return this->result_(false, "Preset recovery blocked: wait for fresh settled height and other operations to finish");
  if (this->keep_awake_fault_ && !this->target_move_.busy() && !this->cached_journal_valid_) {
    this->keep_awake_fault_ = false;
    this->last_keep_awake_ = now;
    return this->result_(true, "Keep-awake fault cleared; next attempt after 30 seconds");
  }
  if (this->cached_journal_valid_ && !this->target_move_.busy())
    this->target_move_.recover(this->cached_journal_.original, this->cached_journal_.temporary);
  const bool retried = this->target_move_.restore_backup(now);
  if (retried) {
    this->keep_awake_active_ = false;
    this->keep_awake_recovery_ = false;
    this->restore_requested_ = true;
    this->journal_recheck_pending_ = false;
    this->keep_awake_fault_ = false;
    this->soft_abort_error_ = nullptr;
    this->last_target_error_ = nullptr;
    this->last_keep_awake_ = now;
  }
  return this->result_(retried, retried ? "Preset restoration requested; waiting for readback" : "No failed preset transaction to restore");
}
bool DeskUART::recovery_access_allowed_(uint32_t now) const {
  return !this->firmware_export_active_ && !this->preset_settings_.busy() && !this->timer_.busy() && !this->retarget_pending_ &&
      !this->release_fault_ && this->monitor_.fresh(now) && this->stable_ms_(now) >= 1500 &&
      static_cast<uint32_t>(now - this->last_height_received_) <= 750 &&
      static_cast<uint32_t>(now - this->last_preset_recall_) >= 3000 && !this->pulse_.active() &&
      !this->release_ack_.pending() && !this->stop_ack_.pending() && this->limit_phase_ == LimitPhase::Idle;
}
bool DeskUART::accept_current_preset() {
  if (this->firmware_export_active_) return this->result_(false, "Firmware read active; desk controls paused");
  const uint32_t now = millis();
  this->last_keep_awake_ = now;
  if (!this->recovery_access_allowed_(now)) return this->result_(false, "Preset acceptance blocked: wait for fresh settled height and other operations to finish");
  if (this->cached_journal_valid_ && !this->target_move_.busy())
    this->target_move_.recover(this->cached_journal_.original, this->cached_journal_.temporary);
  if (!this->target_move_.accept_current(now)) return this->result_(false, "No failed preset backup to discard");
  this->accept_current_active_ = true;
  this->journal_recheck_pending_ = false;
  this->keep_awake_active_ = false;
  this->soft_abort_error_ = nullptr;
  this->keep_awake_recovery_ = false;
  this->last_target_error_ = nullptr;
  return this->result_(true, "Checking current preset twice before discarding backup; no movement");
}
void DeskUART::target_loop_(uint32_t now) {
  if (!this->target_enabled_ && !this->target_move_.busy()) return;
  using Phase = desk_protocol::TemporaryPreset::Phase;
  // Any new or unfinished transaction supersedes an earlier verified restore.
  if (this->target_move_.busy()) this->verified_journal_.valid = false;
  // RX may have advanced RestoreVerify to ClearJournal with an expired height
  // sample. Check before journal_cleared() changes busy() to false; otherwise
  // the successful parameter read hides the lost height link.
  if (this->soft_abort_error_ && this->target_move_.busy() && !this->monitor_.fresh(now)) {
    this->soft_abort_error_ = nullptr;
    this->keep_awake_fault_ = true;
    this->result_(false, "Keep-awake interrupted; telemetry lost before original preset 3 was verified");
  }
  if ((this->target_move_.inspecting_recovery() || this->accept_current_active_ || this->recovery_check_active_ || this->restore_requested_) &&
      !this->recovery_access_allowed_(now))
    this->target_move_.abort_recovery(now);
  this->target_move_.tick(now, this->monitor_.fresh(now), this->stable_ms_(now),
                         !this->pulse_.active() && !this->release_ack_.pending() && !this->stop_ack_.pending() && !this->release_fault_);
  if (this->target_move_.take_brake()) {
    if (this->keep_awake_active_) this->keep_awake_fault_ = true;
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
      if (this->keep_awake_active_) this->keep_awake_pressed_ = true;
      this->send_(press);
    }
  }
  if (this->target_move_.phase() == Phase::ClearJournal) {
    this->target_move_.journal_cleared(this->save_journal_(false));
    if (!this->target_move_.busy() && this->keep_awake_active_ && !this->keep_awake_fault_ && this->cached_journal_valid_ &&
        this->cached_journal_.original == this->target_move_.original() &&
        this->cached_journal_.temporary == this->target_move_.target())
      this->verified_journal_ = {true, this->target_move_.original(), this->target_move_.target()};
    if (!this->target_move_.dirty()) {
      if (this->keep_awake_active_) ESP_LOGD(TAG, "Keep-awake: original preset 3 restored and read back");
      else if (this->target_move_.phase() == Phase::Monitoring)
        ESP_LOGI(TAG, "Original preset 3 restored after motion began; monitoring target move");
      else ESP_LOGI(TAG, "Original preset 3 restored and read back");
    }
  }
  desk_protocol::Frame frame;
  if (this->target_move_.take_frame(frame)) this->send_(frame);
  if (this->target_move_.failed() && this->target_move_.error() != this->last_target_error_) {
    this->last_target_error_ = this->target_move_.error();
    // A height change or late sample before the press is not a fault on its
    // own: the original preset is restored and verified first. Lost telemetry,
    // failed restoration and anything after the press remain faults.
    if (this->keep_awake_active_ && !this->keep_awake_pressed_ && !this->soft_abort_error_ &&
        !this->keep_awake_fault_ && this->target_move_.interrupted_by_change() && this->monitor_.fresh(now)) {
      this->soft_abort_error_ = this->last_target_error_;
      ESP_LOGW(TAG, "Keep-awake stopped before preset press: %s; verifying original preset 3", this->soft_abort_error_);
    } else {
      if (this->keep_awake_active_) {
        this->keep_awake_fault_ = true;
        this->soft_abort_error_ = nullptr;
      }
      // Braking to replace a target is intentional, not a rejected command.
      if (!(this->retarget_pending_ && std::strcmp(this->last_target_error_, "Target move interrupted") == 0))
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
      this->result_(true, this->accept_current_active_ ? "Current preset accepted; backup discarded, EEPROM unverified" :
        this->recovery_check_active_ ? "Original preset RAM checked; backup retained, EEPROM unverified" : "Original preset RAM restored; backup retained, EEPROM unverified");
      if (!this->recovery_check_active_) this->keep_awake_fault_ = false;
      this->last_keep_awake_ = now;
      this->last_target_error_ = nullptr;
    }
    if (!this->target_move_.busy() || this->target_move_.phase() == Phase::Fault)
      this->recovery_check_active_ = this->accept_current_active_ = this->restore_requested_ = false;
  }
  if (this->keep_awake_active_ && !this->target_move_.busy()) {
    this->keep_awake_active_ = false;
    this->last_keep_awake_ = now;
  }
  // Hide only healthy automatic cycles in the HA entity, never the internal
  // journal/interlock. Boot recovery, user moves, Stop and failures stay visible.
  const bool quiet_restore = this->keep_awake_active_ && this->target_move_.keep_awake() &&
      !this->target_move_.failed() && !this->keep_awake_fault_ && !this->release_fault_ &&
      !this->stop_latched_ && this->monitor_.fresh(now);
  const bool restore_pending = this->target_move_.dirty() && !quiet_restore;
  if (restore_pending != this->last_restore_pending_) {
    this->last_restore_pending_ = restore_pending;
    if (this->restore_pending_sensor_) this->restore_pending_sensor_->publish_state(this->last_restore_pending_);
  }
}
bool DeskUART::set_timer(uint16_t minutes) {
  if (this->firmware_export_active_) return this->result_(false, "Firmware read active; desk controls paused");
  const uint32_t now = millis();
  this->last_keep_awake_ = now;
  if (!this->timer_enabled_ || this->preset_settings_.busy() || this->retarget_pending_ || this->stop_latched_ || this->release_fault_ ||
      this->target_move_.busy() || this->limit_phase_ != LimitPhase::Idle ||
      this->pulse_.active() || this->release_ack_.pending() || this->stop_ack_.pending() ||
      !this->monitor_.fresh(now) || this->stable_ms_(now) < 1500 || !this->timer_.begin(minutes, now)) {
    return this->result_(false, this->timer_enabled_ ? this->control_reason_(now) : "Timer controls disabled");
  }
  this->last_timer_error_ = nullptr;
  ESP_LOGI(TAG, "Timer request %u min: leave local keys untouched until sequence finishes", minutes);
  this->timer_loop_(now);
  return true;
}
void DeskUART::timer_loop_(uint32_t now) {
  this->timer_.tick(now, this->monitor_.fresh(now), this->stable_ms_(now));
  desk_protocol::Frame frame;
  if (this->timer_.take_frame(frame)) this->send_(frame);
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
    if (this->timer_pending_sensor_) this->timer_pending_sensor_->publish_state(pending);
  }
}
bool DeskUART::result_(bool accepted, const char *message) {
  if (this->command_result_) this->command_result_->publish_state(message);
  if (accepted) ESP_LOGI(TAG, "%s", message);
  else ESP_LOGW(TAG, "Command rejected: %s", message);
  return accepted;
}
void DeskUART::discard_retarget_(const char *reason) {
  if (!this->retarget_pending_) return;
  this->retarget_pending_ = false;
  this->result_(false, reason);
}
const char *DeskUART::control_reason_(uint32_t now) const {
  if (this->firmware_export_active_) return "Firmware read active; desk controls paused";
  using Phase = desk_protocol::TemporaryPreset::Phase;
  if (this->release_fault_) return "Button release uncertain; inspect desk and restart ESP";
  if (!this->monitor_.fresh(now)) return "No fresh UART height; wake panel or check connection";
  if (this->target_move_.phase() == Phase::Fault) return this->target_move_.error();
  if (this->stop_latched_ && !this->stop_acknowledged_) return "Brake ACK missing or pending; movement blocked";
  if (this->retarget_pending_) return "Changing target: braking, settling and restoring preset";
  if (this->stop_latched_) return "Stop: waiting for settled height and preset cleanup";
  if (this->keep_awake_fault_) return "Keep-awake failed; inspect last command result, restore preset or restart ESP";
  if (this->timer_.busy()) return "Timer button sequence active; leave physical keys untouched";
  if (this->preset_settings_.busy()) return "Reading or updating preset height";
  if (this->pending_preset_) return "Preset waiting for keep-awake cleanup";
  if (this->keep_awake_active_) return "Keep-awake preset transaction in progress";
  if (this->target_move_.phase() == Phase::Moving || this->target_move_.phase() == Phase::Monitoring) return "Target move active";
  if (this->target_move_.busy()) return "Preparing target or restoring original preset";
  if (this->pulse_.active() || this->release_ack_.pending()) return "Button release pending";
  if (this->stable_ms_(now) < 1500) return "Waiting for height to settle";
  if (this->limit_phase_ != LimitPhase::Idle) return "Load setting read or write in progress";
  return "Ready";
}
void DeskUART::controls_loop_(uint32_t now) {
  using Phase = desk_protocol::TemporaryPreset::Phase;
  if (this->retarget_pending_) {
    if (!this->monitor_.fresh(now) || this->release_fault_ ||
        this->target_move_.phase() == Phase::Fault ||
        (this->target_move_.failed() && std::strcmp(this->target_move_.error(), "Target move interrupted") != 0) ||
        (this->stop_latched_ && !this->stop_ack_.pending() && !this->stop_acknowledged_) ||
        static_cast<uint32_t>(now - this->retarget_started_) >= 15000)
      this->discard_retarget_("Target replacement cancelled by fault, lost telemetry or timeout; request again after recovery");
  }
  // ACK alone is insufficient. Require post-brake fresh, unchanged raw height,
  // release completion, and verified original-preset cleanup. This is a
  // settling heuristic, not a safety-rated measurement of standstill.
  if (this->stop_latched_) this->clear_stop_latch();
  if (this->retarget_pending_ && !this->stop_latched_ && !this->target_move_.busy() &&
      this->limit_phase_ == LimitPhase::Idle && this->stable_ms_(now) >= 1500) {
    const float next = this->retarget_cm_;
    this->retarget_pending_ = false;
    if (this->move_to_height(next) && this->target_number_) this->target_number_->publish_state(this->display_height(next));
  }
  if (this->sync_target_ && !this->target_move_.busy() && !this->retarget_pending_ &&
      !this->stop_latched_ && this->monitor_.fresh(now) && this->stable_ms_(now) >= 1500) {
    this->sync_target_ = false;
    // Keep UART precision, as at startup; the panel's whole-cm display is not a target.
    if (this->target_number_) this->target_number_->publish_state(
        this->display_height(desk_protocol::metric_display(this->monitor_.height().q).before_rounding_tenths_cm / 10.0f));
  }
  const char *reason = this->control_reason_(now);
  const auto phase = static_cast<uint8_t>(this->target_move_.phase());
  const bool releasing = this->pulse_.active() || this->release_ack_.pending();
  if (reason != this->last_internal_status_ || phase != this->last_internal_phase_ || releasing != this->last_internal_release_) {
    this->last_internal_phase_ = phase;
    this->last_internal_release_ = releasing;
    this->last_internal_status_ = reason;
    ESP_LOGD(TAG, "Internal control status: %s (automatic=%d, phase=%u, release=%d)", reason,
             this->keep_awake_active_, unsigned(this->target_move_.phase()),
             this->pulse_.active() || this->release_ack_.pending());
  }
  // Presentation only. All interlocks and internal phases continue to run.
  if (this->keep_awake_active_ && !this->pending_preset_ && !this->keep_awake_fault_ && !this->target_move_.failed() &&
      !this->release_fault_ && !this->stop_latched_ && this->monitor_.fresh(now)) reason = "Ready";
  if (!this->last_control_status_ || std::strcmp(reason, this->last_control_status_) != 0) {
    this->last_control_status_ = reason;
    if (this->control_status_) this->control_status_->publish_state(reason);
  }
  if (this->last_stop_locked_ != this->stop_latched_) {
    this->last_stop_locked_ = this->stop_latched_;
    if (this->stop_locked_sensor_) this->stop_locked_sensor_->publish_state(this->stop_latched_);
  }
}
void DeskUART::on_shutdown() {
  desk_protocol::Frame frame;
  this->timer_.cancel(millis());
  if (this->timer_.take_frame(frame)) this->send_(frame);
  if (this->pulse_.cancel(frame)) this->send_(frame);
  desk_protocol::telemetry(2, 0xFFFF, frame);
  this->send_(frame);
  this->flush();
}
void DeskUART::dump_config() {
  ESP_LOGCONFIG(TAG, "Desk UART: T150-PJ-V1.5 experimental metric profile");
  ESP_LOGCONFIG(TAG, "Display unit: %s (independent of panel setting)", this->display_inches_ ? "in" : "cm");
  ESP_LOGCONFIG(TAG, "  Telemetry: channel 2, 500 ticks; stale after 3000 ms");
  ESP_LOGCONFIG(TAG, "  Experimental preset buttons: %s", this->motion_enabled_ ? "enabled" : "disabled");
  ESP_LOGCONFIG(TAG, "  Experimental T-key timer: %s", this->timer_enabled_ ? "enabled" : "disabled");
  ESP_LOGCONFIG(TAG, "  Keep-awake via preset 3 after 30 s of inactivity: %s", this->keep_awake_enabled_ ? "enabled" : "disabled");
  LOG_SENSOR("  ", "Height", this->height_sensor_);
  LOG_TEXT_SENSOR("  ", "Controller model", this->identity_sensors_[0]);
  LOG_TEXT_SENSOR("  ", "Controller manufacturer", this->identity_sensors_[1]);
  LOG_TEXT_SENSOR("  ", "Controller identifiers", this->identity_sensors_[2]);
  LOG_TEXT_SENSOR("  ", "Controller date", this->identity_sensors_[3]);
  LOG_BINARY_SENSOR("  ", "UART connected", this->connected_sensor_);
  LOG_SENSOR("  ", "UART frame errors", this->frame_errors_sensor_);
  LOG_SENSOR("  ", "Height sample gap", this->sample_gap_sensor_);
  this->check_uart_settings(115200);
}
} }  // namespace esphome::desk_uart
