#pragma once
#include "esphome/components/desk_uart/desk_uart.h"
#include "esphome/core/log.h"
#include <cstdio>
#include <cstring>

class ResetSpeedAccess : public esphome::desk_uart::DeskUART {
 public:
  static bool known(esphome::desk_uart::DeskUART* desk) {
    auto member = &ResetSpeedAccess::reset_speed_;
    return (desk->*member).known();
  }
  static uint16_t dt02_guards(esphome::desk_uart::DeskUART* desk) {
    auto member = &ResetSpeedAccess::direct_;
    return (desk->*member).reply().guards;
  }
  static bool dt02_probe_done(esphome::desk_uart::DeskUART* desk) {
    auto member = &ResetSpeedAccess::direct_probe_done_;
    return desk->*member;
  }
  // The peer holds the refresh reply until this observes the real adapter's
  // pending Probe. Cache checks therefore do not depend on status publication.
  // For late-reply checks the peer sends the old nonce, then height q=7 as an
  // ordered RX barrier, and holds the current reply until we inspect the cache.
  static void dt02_refresh_fixture(esphome::desk_uart::DeskUART* desk, const char* test) {
    if (std::strncmp(test, "dt02_", 5) != 0)
      return;
    static unsigned stage = 0;
    static uint16_t retry_nonce = 0;
    static uint32_t action_at = 0;
    static bool stop_clear_observed = false;
    auto direct_member = &ResetSpeedAccess::direct_;
    auto reset_member = &ResetSpeedAccess::reset_speed_;
    auto monitor_member = &ResetSpeedAccess::monitor_;
    auto status_member = &ResetSpeedAccess::control_status_;
    auto stop_latched_member = &ResetSpeedAccess::stop_latched_;
    auto& direct = desk->*direct_member;
    using Phase = desk_protocol::DirectTarget::Phase;
    const bool stop = std::strcmp(test, "dt02_refresh_stop") == 0;
    const bool preset = std::strcmp(test, "dt02_refresh_preset") == 0;
    const bool failure_stop = std::strcmp(test, "dt02_refresh_timeout_stop") == 0;
    const bool timeout = failure_stop || std::strcmp(test, "dt02_refresh_timeout") == 0;
    const bool rejected = std::strcmp(test, "dt02_refresh_rejected") == 0;
    auto* status = desk->*status_member;
    auto finish = [&]() {
      ESP_LOGI("rs_test", "dt02_final_guard=%u dt02_probe_done=%d", direct.reply().guards,
               dt02_probe_done(desk));
      ESP_LOGI("rs_test", "dt02_final_status=%s", status->state.c_str());
      stage = 9;
    };
    if (stage == 0 && direct.phase() == Phase::Probe && (desk->*reset_member).reply().state == 4) {
      ESP_LOGI("rs_test", "dt02_refresh_barrier=1 dt02_cached_guard=%u", direct.reply().guards);
      stage = 4;
      if (stop || preset) {
        action_at = esphome::millis();
        const bool accepted = stop ? desk->stop() : desk->recall_preset(1);
        ESP_LOGI(
            "rs_test",
            "dt02_refresh_action=%d dt02_cancel_guard=%u dt02_cancel_probe_done=%d dt02_cancel_ready=%d",
            accepted, direct.reply().guards, dt02_probe_done(desk),
            direct.phase() == Phase::Ready && direct.known());
        stage = 1;
      }
    }
    if (((stage == 1 && stop) || stage == 5) && !stop_clear_observed &&
        !(desk->*stop_latched_member)) {
      stop_clear_observed = true;
      ESP_LOGI("rs_test", "dt02_stop_cleared=1");
      if (stage == 5)
        stage = 6;
    }
    if (stage == 1 && direct.phase() == Phase::Probe) {
      retry_nonce = direct.pending_nonce();
      ESP_LOGI("rs_test", "dt02_retry_barrier=1 dt02_retry_guard=%u dt02_retry_elapsed_ms=%u",
               direct.reply().guards, static_cast<unsigned>(esphome::millis() - action_at));
      stage = 2;
    }
    if (stage == 2 && (desk->*monitor_member).height().q == 7) {
      const bool ignored = direct.phase() == Phase::Probe &&
                           direct.pending_nonce() == retry_nonce && direct.reply().guards == 1;
      ESP_LOGI("rs_test", "dt02_late_ignored=%d dt02_late_guard=%u", ignored,
               direct.reply().guards);
      stage = 3;
    }
    if (stage == 4 && (timeout || rejected) && direct.phase() == Phase::Fault && status &&
        status->state == (rejected
                              ? "Controller rejected the height control status request"
                              : "Controller did not reply to the height control support check")) {
      if (timeout) {
        ESP_LOGI("rs_test", "dt02_fault_barrier=1 dt02_fault_guard=%u", direct.reply().guards);
        stage = 7;
      } else
        finish();
    }
    if (stage == 7 && (desk->*monitor_member).height().q == 7) {
      const bool ignored =
          direct.phase() == Phase::Fault && !direct.known() && direct.reply().guards == 1 &&
          direct.error() &&
          std::strcmp(direct.error(),
                      "Controller did not reply to the height control support check") == 0;
      ESP_LOGI("rs_test", "dt02_late_fault_ignored=%d dt02_late_fault_guard=%u", ignored,
               direct.reply().guards);
      if (failure_stop) {
        ESP_LOGI("rs_test", "dt02_failure_visible_before_stop=%d",
                 status && status->state ==
                               "Controller did not reply to the height control support check");
        ESP_LOGI("rs_test", "dt02_failure_stop=%d", desk->stop());
        stage = 5;
      } else
        stage = 8;
    }
    const char* expected_ready =
        std::strcmp(test, "dt02_refresh_guarded") == 0
            ? "Controller is blocking movement; select the target again to recheck"
            : "Ready";
    if (status && (stage == 3 || (stage == 4 && !timeout && !rejected)) &&
        direct.phase() == Phase::Ready && status->state == expected_ready)
      finish();
    if (status && stage == 6 && direct.phase() == Phase::Unknown && !direct.known() &&
        status->state == "Direct height control support not confirmed")
      finish();
    if (status && stage == 8 && direct.phase() == Phase::Fault &&
        status->state == "Controller did not reply to the height control support check")
      finish();
    // Host stdout is a file, so explicitly release barrier records to the peer.
    std::fflush(stdout);
  }
  // Give the quiet-wait case settled pre-loss history, so the existing
  // generic height-settling gate cannot hide a missing lease-silence check.
  static void settled_history(esphome::desk_uart::DeskUART* desk) {
    auto changed = &ResetSpeedAccess::last_height_change_;
    auto received = &ResetSpeedAccess::last_height_received_;
    desk->*changed = (desk->*received) - 2000;
  }
};
