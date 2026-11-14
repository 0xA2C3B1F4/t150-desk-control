#include "desk_uart.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include <cstring>

namespace esphome {
namespace desk_uart {

static const char* const TAG = "desk_uart";

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
  this->yield_panel_lock_read_();
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
  this->yield_panel_lock_read_();
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

}  // namespace desk_uart
}  // namespace esphome
