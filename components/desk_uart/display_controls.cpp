#include "desk_uart.h"
#include "esphome/core/controller_registry.h"
#include "esphome/core/hal.h"
#include <cmath>

namespace esphome {
namespace desk_uart {

DisplayModeSelect* DisplayModeSelect::first_ = nullptr;
DisplayModeSelect::DisplayModeSelect() : next_(first_) {
  first_ = this;
}
bool DisplayModeSelect::is_instance(const select::Select* candidate) {
  for (auto* item = first_; item; item = item->next_) {
    if (item == candidate) {
      return true;
    }
  }
  return false;
}
void DisplayModeSelect::invalidate() {
  if (!this->has_state()) {
    return;
  }
  this->set_has_state(false);
#if defined(USE_CONTROLLER_REGISTRY) && defined(USE_SELECT)
  ControllerRegistry::notify_select_update(this);
#endif
}

DisplayTimeoutNumber* DisplayTimeoutNumber::first_ = nullptr;
DisplayTimeoutNumber::DisplayTimeoutNumber() : next_(first_) {
  first_ = this;
}
bool DisplayTimeoutNumber::is_instance(const number::Number* candidate) {
  for (auto* item = first_; item; item = item->next_) {
    if (item == candidate) {
      return true;
    }
  }
  return false;
}
void DisplayTimeoutNumber::invalidate() {
  if (!this->has_state()) {
    return;
  }
  this->set_has_state(false);
  this->state = NAN;
#if defined(USE_CONTROLLER_REGISTRY) && defined(USE_NUMBER)
  ControllerRegistry::notify_number_update(this);
#endif
}

void DeskUART::display_setup_(uint32_t boot) {
  if (!this->display_enabled_()) {
    return;
  }
  this->display_.seed(static_cast<uint16_t>(boot ^ 0xD704));
  this->display_preference_ =
      global_preferences->make_preference<DisplaySaved>(this->display_preference_key_, true);
  DisplaySaved saved{};
  if (this->display_preference_.load(&saved) && saved.version == 1 && saved.mode <= 1 &&
      saved.seconds) {
    this->display_saved_ = saved;
    this->display_have_saved_ = true;
    this->display_restore_pending_ = true;
  }
  this->publish_display_();
}

void DeskUART::display_observe_height_(uint16_t sequence) {
  if (!this->display_enabled_()) {
    return;
  }
  // HeightMonitor has already rejected fresh duplicate/out-of-order frames.
  // Keep this epoch across a stale link; height_seen_ is cleared on disconnect.
  if (this->display_have_sequence_ &&
      static_cast<uint16_t>(sequence - this->display_last_sequence_) >= 0x8000) {
    this->display_disconnect_();
  }
  this->display_last_sequence_ = sequence;
  this->display_have_sequence_ = true;
}

void DeskUART::yield_display_read_() {
  if (this->display_.busy() && !this->display_.writing()) {
    this->display_.cancel();
    this->display_message_ = nullptr;
  }
}

void DeskUART::display_disconnect_() {
  if (!this->display_enabled_()) {
    return;
  }
  this->display_.disconnect();
  this->display_probe_attempts_ = 0;
  this->display_last_probe_ = millis() - 1000;
  this->display_restore_pending_ = this->display_have_saved_;
  this->display_message_ = nullptr;
  this->publish_display_();
}

void DeskUART::publish_display_() {
  if (!this->display_enabled_()) {
    return;
  }
  const bool known = this->display_.state_known() && !this->firmware_export_active_;
  const auto& response = this->display_.confirmed();
  const char* status = this->display_message_;
  if (!known) {
    if (this->display_mode_select_) {
      this->display_mode_select_->invalidate();
    }
    if (this->display_timeout_number_) {
      this->display_timeout_number_->invalidate();
    }
    if (!status) {
      status = "Display settings unknown";
    }
  } else {
    const char* mode = response.settings.mode == desk_protocol::DisplaySettingsCodec::AutoBlank
                           ? "Auto blank"
                           : "Normal";
    if (this->display_mode_select_ && (!this->display_mode_select_->has_state() ||
                                       this->display_mode_select_->current_option() != mode)) {
      this->display_mode_select_->publish_state(mode);
    }
    if (this->display_timeout_number_ &&
        (!this->display_timeout_number_->has_state() ||
         this->display_timeout_number_->state != response.settings.timeout_seconds)) {
      this->display_timeout_number_->publish_state(response.settings.timeout_seconds);
    }
    if (!status) {
      status = response.flags & desk_protocol::DisplaySettingsCodec::Attention
                   ? "Visible for desk activity"
               : response.flags & desk_protocol::DisplaySettingsCodec::Blank
                   ? "Height display blanked"
                   : "Height display visible";
    }
  }
  if (this->display_status_ &&
      (!this->display_status_->has_state() || this->display_status_->state != status)) {
    this->display_status_->publish_state(status);
  }
}

bool DeskUART::display_edit_(const desk_protocol::DisplaySettingsCodec::Settings& settings) {
  const uint32_t now = millis();
  if (this->display_.busy() || this->motion_trim_busy_() ||
      !this->motion_trim_access_allowed_(now)) {
    return this->result_(false, "Display change refused; another operation is active");
  }
  if (this->display_.edit(settings) != desk_protocol::DisplaySettings::RequestStatus::Started) {
    return this->result_(false, "Display settings unavailable; no request sent");
  }
  this->display_restore_pending_ = false;
  this->display_message_ = "Waiting for display settings";
  this->display_loop_(now);
  return this->result_(true, "Display settings requested");
}

bool DeskUART::set_display_mode(const std::string& mode) {
  if (mode != "Normal" && mode != "Auto blank") {
    return this->result_(false, "Invalid display mode");
  }
  auto settings = this->display_.confirmed().settings;
  settings.mode = mode == "Normal" ? desk_protocol::DisplaySettingsCodec::Normal
                                   : desk_protocol::DisplaySettingsCodec::AutoBlank;
  return this->display_edit_(settings);
}

bool DeskUART::set_display_timeout(float seconds) {
  if (!std::isfinite(seconds) || seconds < 1 || seconds > 65535 ||
      seconds != static_cast<uint16_t>(seconds)) {
    return this->result_(false, "Display timeout must be a whole number from 1 to 65535 seconds");
  }
  auto settings = this->display_.confirmed().settings;
  settings.timeout_seconds = static_cast<uint16_t>(seconds);
  return this->display_edit_(settings);
}

void DeskUART::display_frame_(const desk_protocol::Frame& frame, uint32_t now) {
  if (!this->display_enabled_() || !this->display_.accept(frame, now)) {
    return;
  }
  using Outcome = desk_protocol::DisplaySettings::Outcome;
  if (this->display_.outcome() == Outcome::Applied) {
    const auto& settings = this->display_.confirmed().settings;
    DisplaySaved saved{settings.timeout_seconds, static_cast<uint8_t>(settings.mode), 1};
    if (!this->display_have_saved_ || saved.seconds != this->display_saved_.seconds ||
        saved.mode != this->display_saved_.mode) {
      if (!this->display_preference_.save(&saved)) {
        this->display_message_ = "Applied; could not save display preference";
        this->publish_display_();
        return;
      }
      this->display_saved_ = saved;
      this->display_have_saved_ = true;
    }
    this->display_message_ = nullptr;
  } else if (this->display_.outcome() == Outcome::Read) {
    this->display_message_ = nullptr;
  } else {
    this->display_message_ = "Display request rejected; not retried";
  }
  this->publish_display_();
}

void DeskUART::display_loop_(uint32_t now) {
  if (!this->display_enabled_()) {
    return;
  }
  if (!this->height_seen_ || !this->monitor_.fresh(now) ||
      static_cast<uint32_t>(now - this->last_height_received_) > 750) {
    if (this->display_.supported() || this->display_.busy()) {
      this->display_disconnect_();
    }
    return;
  }
  const bool was_busy = this->display_.busy();
  this->display_.tick(now);
  if (was_busy && !this->display_.busy()) {
    this->display_message_ = "Display reply missing; not retried";
  }
  const bool allowed = !this->motion_trim_busy_() && this->motion_trim_access_allowed_(now);
  if (!this->display_.busy() && allowed && this->panel_lock_dt02_ &&
      this->reset_speed_.supported()) {
    if (this->display_restore_pending_ && this->display_.state_known()) {
      this->display_restore_pending_ = false;
      desk_protocol::DisplaySettingsCodec::Settings saved{};
      saved.mode =
          static_cast<desk_protocol::DisplaySettingsCodec::Mode>(this->display_saved_.mode);
      saved.timeout_seconds = this->display_saved_.seconds;
      if (!(saved == this->display_.confirmed().settings)) {
        this->display_.edit(saved);
      }
    } else if ((!this->display_.supported() && this->display_probe_attempts_ < 3 &&
                static_cast<uint32_t>(now - this->display_last_probe_) >= 1000) ||
               (this->display_.supported() &&
                static_cast<uint32_t>(now - this->display_last_probe_) >= 5000)) {
      this->display_.probe();
      if (this->display_probe_attempts_ < 3) {
        ++this->display_probe_attempts_;
      }
      this->display_last_probe_ = now;
    }
  }
  desk_protocol::Frame frame;
  if (this->display_.pending_frame(frame)) {
    this->display_sending_ = true;
    const uint32_t transmitted = millis();
    const bool sent = this->send_(frame);
    this->display_sending_ = false;
    if (sent && this->display_.busy()) {
      this->display_.mark_transmitted(transmitted);
    } else {
      this->display_.cancel();
    }
  }
  this->publish_display_();
}

}  // namespace desk_uart
}  // namespace esphome
