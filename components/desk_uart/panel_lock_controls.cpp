#include "desk_uart.h"
#include "esphome/core/controller_registry.h"
#include "esphome/core/hal.h"

namespace esphome {
namespace desk_uart {

PanelLockSwitch* PanelLockSwitch::first_ = nullptr;
PanelLockSwitch::PanelLockSwitch() : next_(first_) {
  first_ = this;
}
bool PanelLockSwitch::is_instance(const switch_::Switch* candidate) {
  for (auto* item = first_; item != nullptr; item = item->next_)
    if (item == candidate)
      return true;
  return false;
}

void PanelLockSwitch::invalidate() {
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

void DeskUART::invalidate_panel_lock_(const char* reason, bool publish, bool report_failure) {
  if (!this->panel_lock_enabled_())
    return;
  this->panel_lock_.disconnect(reason, report_failure);
  this->panel_lock_detection_.reset();
  this->panel_lock_dt02_ = false;
  this->panel_lock_probed_ = false;
  this->last_panel_lock_query_ = millis();
  if (this->panel_lock_switch_)
    this->panel_lock_switch_->invalidate();
  if (const char* failure = this->panel_lock_.take_failure())
    this->result_(false, failure);
  if (publish)
    this->publish_panel_lock_(millis());
}

void DeskUART::publish_panel_lock_(uint32_t now) {
  if (!this->panel_lock_enabled_())
    return;
  const bool fresh = !this->firmware_export_active_ && this->height_seen_ &&
                     this->monitor_.fresh(now) &&
                     static_cast<uint32_t>(now - this->last_height_received_) <= 750;
  if (this->panel_lock_switch_) {
    if (!fresh || !this->panel_lock_.state_known())
      this->panel_lock_switch_->invalidate();
    else if (!this->panel_lock_switch_->has_state() ||
             this->panel_lock_switch_->state != this->panel_lock_.reply().locked)
      this->panel_lock_switch_->publish_confirmed(this->panel_lock_.reply().locked);
  }
  // Switch callbacks can synchronously start another SET or maintenance session.
  const bool available = this->panel_lock_.state_known() && !this->reset_speed_.blocking() &&
                         (!this->panel_lock_busy_() || (!this->panel_lock_detection_.busy() &&
                                                        this->panel_lock_.operation() == 0)) &&
                         this->panel_lock_access_allowed_(millis(), false, true);
  if (available != this->last_panel_lock_available_) {
    this->last_panel_lock_available_ = available;
    if (this->panel_lock_available_)
      this->panel_lock_available_->publish_state(available);
  }
  const char* status = this->panel_lock_status_text_(now);
  if (status != this->last_panel_lock_status_) {
    this->last_panel_lock_status_ = status;
    if (this->panel_lock_status_)
      this->panel_lock_status_->publish_state(status);
  }
}
const char* DeskUART::panel_lock_status_text_(uint32_t now) const {
  const bool fresh = !this->firmware_export_active_ && this->height_seen_ &&
                     this->monitor_.fresh(now) &&
                     static_cast<uint32_t>(now - this->last_height_received_) <= 750;
  const char* status = this->panel_lock_detection_.busy()
                           ? "Checking whether the controller supports panel lock"
                           : this->panel_lock_.error();
  if (!status) {
    if (!fresh || this->firmware_export_active_)
      status = "Panel lock unavailable; lock state unknown";
    else if (this->panel_lock_.busy() &&
             (this->panel_lock_.operation() != 0 || !this->panel_lock_.state_known()))
      status = this->panel_lock_.operation() == 0 ? "Checking panel-lock state"
               : this->panel_lock_.operation() == 1
                   ? "Lock requested; waiting for controller confirmation"
                   : "Unlock requested; waiting for controller confirmation";
    else if (!this->panel_lock_.supported())
      status = "Panel-lock state unknown; support must be checked";
    else if (this->panel_lock_.reply().storage_fault)
      status = "Panel-lock saving failed; locked now, lock state after power loss uncertain";
    else if (this->panel_lock_.reply().locked)
      status = "Locked";
    else
      status =
          this->panel_lock_.reply().physical_gate ? "Unlocked; panel buttons in use" : "Unlocked";
  }
  return status;
}
bool DeskUART::set_panel_lock(bool locked) {
  this->yield_panel_lock_read_();
  if (this->reset_speed_blocking_())
    return this->result_(false, this->control_reason_(millis()));
  const uint32_t now = millis();
  if (!this->panel_lock_switch_)
    return this->result_(false, "Panel lock switch not configured");
  if (!this->panel_lock_.supported())
    return this->result_(false, "Panel-lock support not confirmed; lock control unavailable");
  if (this->panel_lock_busy_() || !this->panel_lock_access_allowed_(now))
    return this->result_(
        false, "Panel lock blocked: wait for fresh settled height and other operations to finish");
  if (!this->panel_lock_.set(locked, now))
    return false;
  this->last_keep_awake_ = this->last_panel_lock_query_ = now;
  desk_protocol::Frame frame;
  if (this->panel_lock_.take_frame(frame)) {
    this->panel_lock_sending_ = true;
    this->send_(frame);
    this->panel_lock_sending_ = false;
  }
  this->result_(true, "Panel lock requested; awaiting controller acknowledgement, no retry");
  // Publishing availability may synchronously invoke Stop. Its result must
  // remain the last result, rather than being overwritten by this request.
  this->publish_panel_lock_(millis());
  return true;
}
void DeskUART::panel_lock_loop_(uint32_t now) {
  if (!this->panel_lock_enabled_())
    return;
  const bool fresh = this->height_seen_ && this->monitor_.fresh(now) &&
                     static_cast<uint32_t>(now - this->last_height_received_) <= 750;
  const bool panel_polling = this->panel_lock_switch_ || this->panel_lock_status_ ||
                             this->panel_lock_available_ || this->reset_speed_controls_enabled_();
  if (!fresh)
    this->invalidate_panel_lock_("No recent controller reply; panel-lock state unknown");
  this->panel_lock_.tick(now, fresh);
  this->panel_lock_result_();
  if (this->panel_lock_busy_() &&
      !this->panel_lock_access_allowed_(
          now, this->panel_lock_detection_.busy() || this->panel_lock_.operation() == 0))
    this->invalidate_panel_lock_(
        "Panel-lock request interrupted by changed movement conditions; lock state unknown, request not retried");
  // Reuse auto detection, including a conclusive stock/preset result.
  // Polling a validated DT02 prerequisite never implies CL01 support.
  if (panel_polling && !this->panel_lock_dt02_ && this->panel_lock_probed_ &&
      this->panel_lock_detection_.error() &&
      static_cast<uint32_t>(now - this->last_panel_lock_query_) >= 5000) {
    // A lost prerequisite reply must not strand an installed, locked CL01.
    // Retry the complete read-only identity/profile transaction after backoff.
    this->panel_lock_detection_.reset();
    this->panel_lock_probed_ = false;
  }
  if (fresh && !this->panel_lock_dt02_ && !this->panel_lock_probed_ &&
      this->panel_lock_access_allowed_(now, true)) {
    // A configured direct/auto backend may already have validated DT02. With
    // preset/monitor-only configurations, verify the exact identity, scale and
    // DT02 capability first. Fresh height alone never authorizes D3 probing.
    if (this->auto_target_ready_ &&
        this->detection_.backend() == desk_protocol::TargetDetection::Backend::Preset) {
      this->panel_lock_probed_ = true;
      this->panel_lock_.disconnect(
          "Required height control support not confirmed; panel lock unavailable, lock state unknown");
    } else if (this->direct_.known() ||
               (this->auto_target_ready_ &&
                this->detection_.backend() == desk_protocol::TargetDetection::Backend::Direct)) {
      this->panel_lock_dt02_ = true;
    } else {
      if (this->panel_lock_detection_.idle() && !this->available() &&
          static_cast<uint32_t>(now - this->last_panel_lock_query_) >= 800) {
        this->panel_lock_.disconnect(nullptr);
        this->panel_lock_detection_.seed(this->direct_.next_nonce());
        this->panel_lock_detection_.start(now, this->monitor_.height().sequence);
      }
      this->panel_lock_detection_.tick(now);
      desk_protocol::Frame request;
      if (!this->available() && this->panel_lock_detection_.take_frame(request)) {
        this->direct_.seed(this->panel_lock_detection_.next_nonce());
        this->panel_lock_sending_ = true;
        this->send_(request);
        this->panel_lock_sending_ = false;
      }
      if (this->panel_lock_detection_.backend() == desk_protocol::TargetDetection::Backend::Direct)
        this->panel_lock_dt02_ = true;
      else if (this->panel_lock_detection_.error() ||
               this->panel_lock_detection_.backend() ==
                   desk_protocol::TargetDetection::Backend::Preset) {
        this->panel_lock_probed_ = true;
        this->last_panel_lock_query_ = now;
        this->panel_lock_.disconnect(
            "Required height control support not confirmed; panel lock unavailable, lock state unknown");
      }
    }
  }
  if ((panel_polling || !this->panel_lock_probed_) && fresh && this->panel_lock_dt02_ &&
      !this->panel_lock_busy_() && this->panel_lock_access_allowed_(now, true) &&
      !this->available() &&
      ((!this->panel_lock_probed_ &&
        static_cast<uint32_t>(now - this->last_panel_lock_query_) >= 800) ||
       static_cast<uint32_t>(now - this->last_panel_lock_query_) >= 5000)) {
    // A lost/invalid GET or SET reply recovers through a new nonce-bound GET
    // after backoff. This refreshes runtime state and never replays a SET.
    this->panel_lock_probed_ = true;
    this->last_panel_lock_query_ = now;
    this->panel_lock_.probe(now);
    desk_protocol::Frame frame;
    if (this->panel_lock_.take_frame(frame)) {
      this->panel_lock_sending_ = true;
      this->send_(frame);
      this->panel_lock_sending_ = false;
    }
  }
  this->publish_panel_lock_(millis());
}

void DeskUART::panel_lock_result_() {
  // Consume terminal events and snapshot their data before invoking any
  // synchronous automation that may start a new transaction or Stop.
  const char* failure = this->panel_lock_.take_failure();
  const bool completed_set = this->panel_lock_.take_completed() && this->panel_lock_.operation();
  const bool success = this->panel_lock_.reply().status == 0 && this->panel_lock_.state_known();
  const char* message =
      success ? (this->panel_lock_.reply().locked
                     ? "Panel locked; controller confirmed the lock setting was saved"
                     : "Panel unlocked; controller confirmed the lock setting was saved")
              : this->panel_lock_.error();
  if (failure)
    this->result_(false, failure);
  else if (completed_set)
    this->result_(success, message);
}

}  // namespace desk_uart
}  // namespace esphome
