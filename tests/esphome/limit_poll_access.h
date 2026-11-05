#pragma once

#include "esphome/components/desk_uart/desk_uart.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include <cstdlib>
#include <cstring>

class LimitPollAccess : public esphome::desk_uart::DeskUART {
 public:
  static void run(esphome::desk_uart::DeskUART* configured, esphome::number::Number* upward,
                  esphome::number::Number* downward, esphome::desk_uart::DisplayModeSelect* mode,
                  esphome::text_sensor::TextSensor* status,
                  esphome::desk_uart::MotionTrimNumber* trim, const char* scenario) {
    LimitPollAccess test;
    test.set_uart_parent(configured->firmware_export_uart());
    test.set_load_limit_number(0, upward);
    test.set_load_limit_number(1, downward);
    test.set_display_mode_select(mode);
    test.set_display_status(status);
    test.set_motion_trim_number(0, trim);
    test.check(scenario);
  }

 private:
  static void require(bool value, const char* message) {
    if (!value) {
      ESP_LOGE("limit_poll_test", "FAILED: %s", message);
      std::abort();
    }
  }

  desk_protocol::Frame display_request(uint32_t now) {
    desk_protocol::Frame request{};
    require(display_.pending_frame(request), "display frame queued");
    display_loop_(now);
    require(display_.busy(), "display waiting for reply");
    return request;
  }

  void display_reply(const desk_protocol::Frame& request, uint32_t now) {
    desk_protocol::Frame reply{};
    reply.size = 10;
    const uint8_t body[] = {0xd8, 4, request.bytes[2], request.bytes[3], request.bytes[4], 0, 3, 1,
                            0,    60};
    std::memcpy(reply.bytes, body, sizeof(body));
    display_frame_(reply, now);
    require(!display_.busy() && display_.state_known(), "display reply accepted");
    require(display_message_ == nullptr, "display status recovered");
  }

  void check(const char* scenario) {
    require(scenario != nullptr, "scenario provided");
    const uint32_t now = esphome::millis();
    desk_protocol::Frame height{};
    height.size = 32;
    height.bytes[0] = 5;
    height.bytes[20] = 6;
    height.bytes[31] = 1;
    require(monitor_.observe(height, now), "fresh height accepted");
    height_seen_ = true;
    last_height_received_ = now;
    original_known_[0] = original_known_[1] = true;
    original_limits_[0] = 2700;
    original_limits_[1] = 1900;
    // Force the existing five-second background poll due at this exact
    // request boundary instead of depending on host scheduling coincidences.
    last_limit_poll_ = now - 5000;

    const bool trim = std::strcmp(scenario, "trim_get") == 0;
    desk_protocol::Frame pending{};
    if (trim) {
      require(motion_trim_.probe() == desk_protocol::MotionTrim::RequestStatus::Started,
              "trim GET started");
      require(motion_trim_.pending_frame(pending), "trim frame queued");
      motion_trim_sending_ = true;
      require(send_(pending), "trim GET transmitted");
      motion_trim_sending_ = false;
      require(motion_trim_.mark_transmitted(now), "trim waiting for reply");
    } else {
      require(display_.probe() == desk_protocol::DisplaySettings::RequestStatus::Started,
              "display GET started");
      pending = display_request(now);
      if (std::strcmp(scenario, "display_set") == 0) {
        display_reply(pending, now);
        // Match the already saved Auto/60 setting used by the hardware
        // reproducer, without exercising preference writes in this test.
        display_have_saved_ = true;
        display_saved_ = DisplaySaved{60, 0, 1};
        require(display_.edit(display_.confirmed().settings) ==
                    desk_protocol::DisplaySettings::RequestStatus::Started,
                "same-setting display SET started");
        pending = display_request(now);
      }
    }

    limit_loop_(now);
    require(limit_phase_ == LimitPhase::Idle, "background limit poll must defer");
    require(trim ? motion_trim_.busy() : display_.busy(), "pending request preserved");
    if (trim) {
      desk_protocol::Frame reply{};
      reply.size = 16;
      reply.bytes[0] = 0xda;
      reply.bytes[1] = 1;
      reply.bytes[2] = 3;
      reply.bytes[8] = pending.bytes[3];
      reply.bytes[9] = pending.bytes[4];
      reply.bytes[11] = 1;
      require(motion_trim_.accept(reply, now), "trim reply accepted");
      require(!motion_trim_.busy(), "trim completed");
    } else {
      display_reply(pending, now);
    }

    limit_loop_(now);
    require(limit_phase_ == LimitPhase::Read, "limit polling resumes after reply");
    desk_protocol::Frame limit_reply{};
    limit_reply.size = 6;
    const uint8_t body[] = {0x16, 0x0e, 0x0a, 0x8c, 0, 2};
    std::memcpy(limit_reply.bytes, body, sizeof(body));
    limit_frame_(limit_reply, now);
    require(limit_phase_ == LimitPhase::Idle && limit_numbers_[0]->state == 2700,
            "active limit reply completes normally");
    last_limit_poll_ = now - 5000;
    limit_loop_(now);
    require(limit_phase_ == LimitPhase::Read, "next limit read started");
    limit_loop_(now + 700);
    require(limit_phase_ == LimitPhase::Idle, "active limit timeout still completes");
    ESP_LOGI("limit_poll_test", "case=%s passed", scenario);
  }
};
