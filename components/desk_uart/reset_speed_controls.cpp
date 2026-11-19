#include "desk_uart.h"
#include "esphome/core/controller_registry.h"
#include "esphome/core/hal.h"

namespace esphome {
namespace desk_uart {

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
  this->yield_panel_lock_read_();
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
  this->yield_panel_lock_read_();
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
                         (reply.flags & reset_eligibility) && !reply.panel_lock && reply.token;
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
      !this->reset_speed_.busy() && !this->reset_speed_.owned() && this->panel_lock_dt02_ &&
      this->panel_lock_.supported() && this->reset_speed_access_allowed_(now, false, true) &&
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

}  // namespace desk_uart
}  // namespace esphome
