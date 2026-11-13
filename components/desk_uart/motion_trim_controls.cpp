#include "desk_uart.h"
#include "esphome/core/controller_registry.h"
#include "esphome/core/hal.h"
#include <cmath>

namespace esphome {
namespace desk_uart {

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
  this->yield_panel_lock_read_();
  if (!this->motion_trim_access_allowed_(millis()) || !this->panel_lock_.state_known() ||
      this->panel_lock_.reply().storage_fault) {
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
  desk_protocol::PanelLockReply lock;
  desk_protocol::DirectReply direct;
  if (desk_protocol::reset_speed_reply(frame, reset)) {
    this->motion_trim_.observe_sequence(reset.sequence);
  } else if (desk_protocol::panel_lock_reply(frame, lock)) {
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
        this->motion_trim_dt_attempts_ < 3 && this->panel_lock_dt02_ &&
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

}  // namespace desk_uart
}  // namespace esphome
