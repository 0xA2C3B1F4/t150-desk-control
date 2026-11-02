#pragma once
#include "esphome/components/desk_uart/desk_uart.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

class CustomTimerBuzzerAccess : public esphome::desk_uart::DeskUART {
 public:
  static bool& callback_armed() {
    static bool value = false;
    return value;
  }
  static bool& callback_ran() {
    static bool value = false;
    return value;
  }
  static bool& callback_sent() {
    static bool value = false;
    return value;
  }
  // Run synchronously from a real control-status publication after the parent
  // loop captured its timestamp, then transmit with a measurably newer clock.
  static void status_callback(esphome::desk_uart::DeskUART* desk) {
    const char* test = std::getenv("DESK_CUSTOM_CASE");
    if (!test || std::strcmp(test, "callback_beep") != 0 || !callback_armed())
      return;
    callback_armed() = false;
    callback_ran() = true;
    esphome::delay(5);
    callback_sent() = desk->custom_beep(200, 300, 3);
    ESP_LOGI("custom_regression", "callback_write_sent=%d", callback_sent());
    std::fflush(stdout);
  }
  // Observe real discovery and pending reads in the generated host component.
  // The fixture does not seed capability or replace any production decision.
  static void run(esphome::desk_uart::DeskUART* desk) {
    static bool done = false;
    static bool beep_sent = false;
    static bool short_beep_sent = false;
    static const uint32_t started = esphome::millis();
    if (done)
      return;
    const char* test = std::getenv("DESK_CUSTOM_CASE");
    auto custom_member = &CustomTimerBuzzerAccess::custom_;
    auto reset_member = &CustomTimerBuzzerAccess::reset_speed_;
    auto lock_member = &CustomTimerBuzzerAccess::child_lock_;
    auto direct_member = &CustomTimerBuzzerAccess::direct_;
    auto result_member = &CustomTimerBuzzerAccess::command_result_;
    auto reason_member = &CustomTimerBuzzerAccess::control_reason_;
    auto& custom = desk->*custom_member;
    auto& reset = desk->*reset_member;
    auto& lock = desk->*lock_member;
    auto finish = [&](bool passed) {
      done = true;
      ESP_LOGI("custom_regression", "case=%s passed=%d", test, passed);
      std::fflush(stdout);
    };
    if (std::strcmp(test, "callback_beep") == 0) {
      if (callback_ran()) {
        auto deferral_member = &CustomTimerBuzzerAccess::custom_beep_deferral_;
        finish(callback_sent() && custom.pending() &&
               custom.operation() == desk_protocol::CustomTimerBuzzer::Beep &&
               (desk->*deferral_member).active(esphome::millis()));
      } else if (custom.available() && !callback_armed()) {
        callback_armed() = true;
        auto status_member = &CustomTimerBuzzerAccess::last_control_status_;
        desk->*status_member = nullptr;
      }
      return;
    }
    const bool probe =
        custom.pending() && custom.operation() == desk_protocol::CustomTimerBuzzer::Probe;
    if (probe && std::strcmp(test, "probe_direct") == 0) {
      finish(desk->move_to_height(80) && !custom.pending());
    } else if (probe && std::strcmp(test, "probe_timer") == 0) {
      const bool accepted = desk->set_timer(30);
      finish(accepted && !custom.pending());
      desk->stop();
    } else if (probe && std::strcmp(test, "probe_lock") == 0) {
      finish(desk->set_child_lock(true) && !custom.pending());
    } else if (probe && std::strcmp(test, "probe_maintenance") == 0) {
      finish(desk->begin_firmware_export(false) && !custom.pending());
    } else if (probe && std::strcmp(test, "probe_status") == 0) {
      auto reset_available_member = &CustomTimerBuzzerAccess::reset_available_;
      auto speed_available_member = &CustomTimerBuzzerAccess::motion_speed_available_;
      auto lock_available_member = &CustomTimerBuzzerAccess::child_lock_available_;
      auto publish_reset_member = &CustomTimerBuzzerAccess::publish_reset_speed_;
      auto publish_lock_member = &CustomTimerBuzzerAccess::publish_child_lock_;
      (desk->*publish_reset_member)(esphome::millis());
      (desk->*publish_lock_member)(esphome::millis());
      finish((desk->*reset_available_member)->state && (desk->*speed_available_member)->state &&
             (desk->*lock_available_member)->state);
    } else if (std::strcmp(test, "unsupported") == 0 &&
               static_cast<uint32_t>(esphome::millis() - started) >= 4500 &&
               (desk->*direct_member).known() && !lock.busy()) {
      const bool unsupported = !reset.supported() && !custom.pending() && !custom.available();
      const bool accepted = desk->set_timer(30);
      finish(unsupported && accepted);
      desk->stop();
    } else if (custom.available() && std::strcmp(test, "custom_rs_get") == 0 && reset.busy() &&
               reset.operation() == desk_protocol::ResetSpeed::Get) {
      const bool accepted = desk->custom_set_timer(15);
      auto custom_loop_member = &CustomTimerBuzzerAccess::custom_loop_;
      (desk->*custom_loop_member)(esphome::millis());
      finish(accepted && custom.pending() && !reset.busy() && reset.supported() && reset.known());
    } else if (custom.available() && std::strcmp(test, "custom_cl_get") == 0 && lock.busy() &&
               lock.operation() == 0) {
      const bool accepted = desk->custom_set_timer(15);
      auto switch_member = &CustomTimerBuzzerAccess::child_lock_switch_;
      finish(accepted && custom.pending() && !lock.busy() && lock.state_known() &&
             lock.supported() && (desk->*switch_member)->has_state());
    } else if (custom.available() && std::strcmp(test, "maintenance_rs_get") == 0 && reset.busy() &&
               reset.operation() == desk_protocol::ResetSpeed::Get) {
      finish(desk->begin_firmware_export(false) && !reset.busy());
    } else if (custom.available() && std::strcmp(test, "maintenance_cl_get") == 0 && lock.busy() &&
               lock.operation() == 0) {
      finish(desk->begin_firmware_export(false) && !lock.busy());
    } else if (custom.available() && std::strcmp(test, "lock_cl_get") == 0 && lock.busy() &&
               lock.operation() == 0) {
      finish(desk->set_child_lock(true) && lock.busy() && lock.operation() == 1);
    } else if (custom.available() && std::strcmp(test, "beep_keep_awake") == 0) {
      if (!beep_sent) {
        beep_sent = desk->custom_beep(2000, 5000, 20);
        return;
      }
      if (custom.pending())
        return;
      if (!short_beep_sent) {
        short_beep_sent = desk->custom_beep(50, 50, 1);
        return;
      }
      auto yield_member = &CustomTimerBuzzerAccess::yield_child_lock_read_;
      (desk->*yield_member)();
      auto direct_mode_member = &CustomTimerBuzzerAccess::direct_mode_;
      auto auto_mode_member = &CustomTimerBuzzerAccess::auto_target_mode_;
      auto enabled_member = &CustomTimerBuzzerAccess::keep_awake_enabled_;
      auto last_member = &CustomTimerBuzzerAccess::last_keep_awake_;
      auto deferral_member = &CustomTimerBuzzerAccess::custom_beep_deferral_;
      auto loop_member = &CustomTimerBuzzerAccess::keep_awake_loop_;
      auto active_member = &CustomTimerBuzzerAccess::keep_awake_active_;
      auto target_member = &CustomTimerBuzzerAccess::target_move_;
      desk->*direct_mode_member = false;
      desk->*auto_mode_member = false;
      desk->*enabled_member = true;
      const uint32_t now = esphome::millis();
      const bool refreshed = static_cast<uint32_t>(now - (desk->*last_member)) < 1000;
      const bool long_retained = (desk->*deferral_member).active(now + 31000);
      auto custom_result_member = &CustomTimerBuzzerAccess::custom_result_;
      const bool busy_reported =
          (desk->*custom_result_member)->state == "Controller busy; request not accepted";
      desk->*last_member = now - 31000;
      (desk->*loop_member)(now);
      const bool deferred = !(desk->*active_member) && !(desk->*target_member).busy();
      // Expire only the estimate and keep the same fresh telemetry and aged
      // inactivity timer. This proves the deferral prevented a real cycle.
      desk->*deferral_member = {};
      (desk->*deferral_member).begin(2000, 5000, 20, now - 138500);
      (desk->*loop_member)(now);
      const bool resumed = (desk->*active_member) && (desk->*target_member).busy();
      finish(refreshed && busy_reported && long_retained && deferred && resumed);
      desk->stop();
    } else if (custom.available() && std::strcmp(test, "write_guards") == 0) {
      const bool sent = desk->custom_set_timer(15);
      const bool move_blocked = !desk->move_to_height(80);
      const bool reason =
          (desk->*result_member)->state == "Custom timer/buzzer transaction pending";
      const bool maintenance_blocked = !desk->begin_firmware_export(false);
      const bool preset_blocked = !desk->recall_preset(1);
      desk->stop();
      const bool cleared = std::strcmp((desk->*reason_member)(esphome::millis(), false),
                                       "Custom timer/buzzer transaction pending") != 0;
      finish(sent && move_blocked && reason && maintenance_blocked && preset_blocked && cleared &&
             !custom.pending());
    }
  }
};
