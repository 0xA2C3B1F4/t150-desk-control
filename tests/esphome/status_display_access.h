#pragma once
#include "esphome/components/desk_uart/desk_uart.h"
#include "esphome/core/log.h"
#include <cstdlib>
#include <cstring>

// Deterministic presentation checks on the real adapter. No UART is attached:
// transaction objects are driven directly, and no command is transmitted.
class StatusDisplayAccess : public esphome::desk_uart::DeskUART {
 public:
  static void require(bool ok, const char* what) {
    if (!ok) {
      ESP_LOGE("status_test", "FAILED: %s", what);
      std::abort();
    }
  }
  void idle(uint32_t now) {
    desk_protocol::Frame height{};
    height.size = 32;
    height.bytes[0] = 5;
    height.bytes[20] = 6;
    height.bytes[31] = 1;
    require(monitor_.observe(height, now), "height accepted");
    height_seen_ = true;
    last_height_received_ = now;
    last_height_change_ = now - 2000;
  }
  void expect(uint32_t now, const char* display, const char* internal = nullptr) {
    require(std::strcmp(control_reason_(now, true), display) == 0, display);
    if (internal)
      require(std::strcmp(control_reason_(now), internal) == 0, internal);
  }
  void rs_reply(uint32_t now, uint8_t state = 0, uint8_t speed = 0, uint8_t paused_flags = 6,
                uint8_t resume_result = 3, bool diagnostics = true) {
    desk_protocol::Frame request{}, reply{};
    require(reset_speed_.probe(now), "RS01 probe");
    require(reset_speed_.take_frame(request), "RS01 request");
    reply.size = 48;
    reply.bytes[0] = 0xD6;
    reply.bytes[1] = 1;
    reply.bytes[2] = diagnostics ? 31 : 15;
    reply.bytes[7] = speed;
    reply.bytes[5] = state;
    reply.bytes[8] = state == 1 ? 8 : state == 3 ? paused_flags : state == 5 ? 4 : 5;
    if (state == 1) {
      reply.bytes[18] = 5;
      reply.bytes[19] = 220;
    }
    reply.bytes[12] = request.bytes[3];
    reply.bytes[13] = request.bytes[4];
    reply.bytes[17] = 1;
    reply.bytes[37] = 1;
    if (diagnostics) {
      reply.bytes[46] = (reply.bytes[8] & 1) ? 0 : 4;
      reply.bytes[47] = (reply.bytes[8] & 2) ? 0 : resume_result;
    }
    reset_speed_.accept(reply, now + 1);
    require(reset_speed_.known(), "RS01 reply accepted");
    reset_speed_result_();
  }
  void cl_reply(uint32_t now, bool locked = false, bool storage_fault = false,
                bool physical_gate = false) {
    desk_protocol::Frame request{}, reply{};
    require(panel_lock_.probe(now), "CL01 probe");
    require(panel_lock_.take_frame(request), "CL01 request");
    reply.size = 15;
    reply.bytes[0] = 0xD4;
    reply.bytes[1] = 1;
    reply.bytes[3] = storage_fault ? 3 : 0;
    reply.bytes[4] = locked;
    reply.bytes[5] = locked || physical_gate;
    reply.bytes[6] = storage_fault;
    std::memcpy(reply.bytes + 7, "CL01", 4);
    reply.bytes[11] = request.bytes[3];
    reply.bytes[12] = request.bytes[4];
    reply.bytes[14] = 1;
    panel_lock_.accept(reply, now + 1);
    require(panel_lock_.supported(), "CL01 reply accepted");
  }
  static void availability_checks() {
    const uint32_t now = esphome::millis();
    StatusDisplayAccess d;
    d.idle(now);
    d.last_preset_recall_ = now - 5000;
    d.rs_reply(now - 2);
    d.cl_reply(now - 2);
    d.reset_enabled_ = true;
    static esphome::desk_uart::MotionSpeedSelect speed;
    static esphome::desk_uart::PanelLockSwitch lock;
    speed.traits.set_options({"Normal", "Slow"});
    esphome::binary_sensor::BinarySensor reset_available, speed_available, lock_available;
    d.motion_speed_select_ = &speed;
    d.reset_available_ = &reset_available;
    d.motion_speed_available_ = &speed_available;
    d.panel_lock_available_ = &lock_available;
    d.panel_lock_switch_ = &lock;
    require(!d.reset_desk(true), "legacy continuation cannot start an idle reset");
    desk_protocol::Frame legacy_reset_frame{};
    require(!d.reset_speed_.busy() && !d.reset_speed_.take_frame(legacy_reset_frame),
            "idle legacy continuation queues no frame");
    unsigned changes = 0;
    reset_available.add_on_state_callback([&](bool) {
      ++changes;
    });
    speed_available.add_on_state_callback([&](bool) {
      ++changes;
    });
    lock_available.add_on_state_callback([&](bool) {
      ++changes;
    });
    auto publish = [&]() {
      d.publish_reset_speed_(now);
      d.publish_panel_lock_(now);
    };
    auto all = [&](bool expected) {
      publish();
      require(reset_available.state == expected, "reset availability");
      require(speed_available.state == expected, "speed availability");
      require(lock_available.state == expected, "panel availability");
    };
    all(true);
    const auto initial_changes = changes;
    require(d.reset_speed_.probe(now), "availability RS01 GET");
    all(true);
    require(!d.reset_speed_access_allowed_(now) && !d.panel_lock_access_allowed_(now),
            "RS01 GET admission still blocked");
    d.reset_speed_.yield_read();
    const auto panel_lock = d.panel_lock_;
    require(d.panel_lock_.probe(now), "availability CL01 GET");
    all(true);
    require(!d.reset_speed_access_allowed_(now) && d.panel_lock_busy_(),
            "CL01 GET admission still blocked");
    // The real SET path yields this GET before admission. Check that step
    // without transmitting through this fixture's deliberately absent UART.
    d.yield_panel_lock_read_();
    require(d.panel_lock_.supported() && d.panel_lock_.state_known() && !d.panel_lock_busy_(),
            "yielded CL01 GET retains confirmed state");
    require(d.panel_lock_access_allowed_(now), "CL01 SET admission after yielding its own GET");
    d.panel_lock_ = panel_lock;
    require(d.preset_settings_.begin_read(1, now), "availability preset read");
    all(true);
    require(!d.reset_speed_access_allowed_(now) && !d.panel_lock_access_allowed_(now),
            "preset read admission still blocked");
    d.preset_settings_ = desk_protocol::PresetSettings(d.height_profile_);
    d.limit_phase_ = LimitPhase::Read;
    all(true);
    require(!d.reset_speed_access_allowed_(now) && !d.panel_lock_access_allowed_(now),
            "limit read admission still blocked");
    d.limit_phase_ = LimitPhase::Idle;
    d.identity_pending_ = 0;
    all(true);
    require(!d.reset_speed_access_allowed_(now) && !d.panel_lock_access_allowed_(now),
            "identity read admission still blocked");
    d.identity_pending_ = 255;
    require(changes == initial_changes, "background reads cause no availability events");
    d.limit_phase_ = LimitPhase::WriteAck;
    all(false);
    d.limit_phase_ = LimitPhase::Verify;
    all(false);
    d.limit_phase_ = LimitPhase::Idle;
    all(true);
    require(d.preset_settings_.begin_write(3, 830, now), "availability preset write");
    all(false);
    d.preset_settings_ = desk_protocol::PresetSettings(d.height_profile_);
    all(true);
    require(d.panel_lock_.set(true, now), "availability CL01 SET");
    all(false);
    d.panel_lock_ = panel_lock;
    all(true);
    require(d.reset_speed_.set_speed(true, now), "availability speed SET");
    all(false);
    d.reset_speed_ = desk_protocol::ResetSpeed();
    d.rs_reply(now - 2);
    all(true);
    d.stop_latched_ = true;
    all(false);
    d.stop_latched_ = false;
    all(true);
    d.last_height_change_ = now;
    all(false);
    d.last_height_change_ = now - 2000;
    all(true);
    d.last_height_received_ = now - 1000;
    all(false);
    d.last_height_received_ = now;
    all(true);
    d.firmware_export_active_ = true;
    all(false);
    d.firmware_export_active_ = false;
    all(true);
    d.reset_speed_ = desk_protocol::ResetSpeed();
    d.rs_reply(now - 2, 3);
    publish();
    require(reset_available.state && !speed_available.state && !lock_available.state,
            "paused reset makes the Reset command available");
    d.reset_speed_ = desk_protocol::ResetSpeed();
    d.rs_reply(now - 2, 3, 0, 5);
    all(false);
    require(!d.reset_desk(), "paused START eligibility cannot restart through Reset");
    desk_protocol::Frame rejected_reset_frame{};
    require(!d.reset_speed_.busy() && !d.reset_speed_.take_frame(rejected_reset_frame),
            "blocked paused reset queues no frame");
    d.reset_speed_ = desk_protocol::ResetSpeed();
    d.rs_reply(now - 1002, 3);
    all(false);
    require(d.reset_speed_access_allowed_(now, true),
            "fresh telemetry permits checking stale paused RS01 admission");
    require(!d.reset_desk() && !d.reset_desk(true),
            "stale paused reply cannot admit either Reset command");
    require(!d.reset_speed_.busy() && !d.reset_speed_.take_frame(rejected_reset_frame),
            "stale paused reset queues no frame");
    d.reset_speed_ = desk_protocol::ResetSpeed();
    d.rs_reply(now - 2, 1);
    all(false);
    d.reset_speed_ = desk_protocol::ResetSpeed();
    d.rs_reply(now - 2, 5);
    all(false);
    d.reset_speed_ = desk_protocol::ResetSpeed();
    d.rs_reply(now - 1002);
    publish();
    require(!reset_available.state && !speed_available.state,
            "stale RS01 disables reset and speed immediately");
    // Isolate reply freshness from the movement/settling gate. The display may
    // retain a confirmed selection even at rest, but cannot authorize a write.
    require(d.reset_speed_access_allowed_(now),
            "idle adapter gates permit checking stale RS01 admission");
    require(speed.has_state() && speed.current_option() == "Normal",
            "idle stale reply retains confirmed speed");
    require(!d.set_motion_speed(true), "stale RS01 alone rejects an idle speed change");
    desk_protocol::Frame rejected_speed_frame{};
    require(!d.reset_speed_.busy() && !d.reset_speed_.take_frame(rejected_speed_frame),
            "stale speed rejection queues no frame");
    // The selected speed survives a pause in idle-only RS01 polling during
    // movement. This is display state, not permission to issue a speed write.
    for (uint8_t selected : {uint8_t(0), uint8_t(1)}) {
      d.reset_speed_ = desk_protocol::ResetSpeed();
      d.rs_reply(now - 1002, 0, selected);
      d.last_height_change_ = now;
      d.publish_reset_speed_(now);
      require(speed.has_state() && speed.current_option() == (selected ? "Slow" : "Normal"),
              "confirmed speed retained during motion");
      require(!d.set_motion_speed(selected == 0), "cached display cannot admit speed changes");
      d.last_height_received_ = now - 1000;
      d.publish_reset_speed_(now);
      require(!speed.has_state(), "UART loss clears retained speed");
      d.last_height_received_ = now;
      d.reset_speed_.disconnect("lost");
      d.publish_reset_speed_(now);
      require(!speed.has_state(), "new link requires a new confirmed speed");
    }
    ESP_LOGI("status_test", "availability checks passed");
  }
  static void maintenance_capability_checks() {
    const uint32_t now = esphome::millis();
    StatusDisplayAccess d;
    d.idle(now);
    d.reset_enabled_ = true;
    d.last_preset_recall_ = now - 5000;
    esphome::sensor::Sensor height;
    d.height_sensor_ = &height;
    d.rs_reply(now - 2);
    require(d.reset_speed_confirmed_, "validated D6 confirms RS01 before maintenance");
    d.last_height_change_ = now;
    require(!d.begin_firmware_export(), "unsettled desk refuses maintenance");
    require(d.reset_speed_confirmed_, "refused maintenance preserves capability proof");
    d.last_height_change_ = now - 2000;
    require(d.begin_firmware_export(), "settled desk admits maintenance");
    require(!d.reset_speed_confirmed_, "admitted maintenance clears capability proof");
    d.end_firmware_export();
    d.idle(now);
    d.cl_reply(now - 2);
    desk_protocol::Frame request{};
    require(d.reset_speed_.probe(now - 1000) && d.reset_speed_.take_frame(request),
            "post-maintenance discovery starts");
    d.reset_speed_.tick(now, true);
    require(d.reset_speed_.error() && !d.reset_speed_.blocking(),
            "removed RS01 remains unverified");
    d.expect(now, "Ready");
    d.rs_reply(now - 2);
    require(d.reset_speed_confirmed_, "fresh D6 confirms RS01 again after maintenance");
    ESP_LOGI("status_test", "maintenance capability checks passed");
  }
  static void run() {
    constexpr uint32_t now = 10000;
    {
      StatusDisplayAccess d;
      d.idle(now);
      d.reset_enabled_ = true;
      d.cl_reply(now - 2);
      desk_protocol::Frame request{};
      require(d.reset_speed_.probe(now - 1000) && d.reset_speed_.take_frame(request),
              "optional RS01 discovery starts");
      d.reset_speed_.tick(now, true);
      require(d.reset_speed_.error() && !d.reset_speed_.blocking(),
              "unsupported capability stays unverified");
      d.expect(now, "Ready");
      d.panel_lock_.disconnect(
          "Panel-lock saving failed; locked now, lock state after power loss uncertain", false);
      d.expect(now, "Panel-lock saving failed; locked now, lock state after power loss uncertain",
               "Ready");
    }
    {
      StatusDisplayAccess d;
      d.idle(now);
      d.reset_enabled_ = true;
      d.cl_reply(now - 2);
      d.rs_reply(now - 2);
      d.reset_speed_.disconnect("No recent controller reply; reset and speed state unknown");
      d.expect(now, "No recent controller reply; reset and speed state unknown", "Ready");
    }
    for (uint8_t result : {uint8_t(3), uint8_t(4), uint8_t(5), uint8_t(6)}) {
      StatusDisplayAccess d;
      d.idle(now);
      d.rs_reply(now - 2, 3, 0, 4, result);
      d.expect(now, result == 4   ? "Reset paused; release physical keys to continue"
                    : result == 5 ? "Reset paused; panel lock blocks continuation"
                    : result == 6 ? "Reset paused; controller is blocking movement"
                                  : "Reset paused; continuation unavailable; inspect the desk");
      d.expect(now + 1000, "Reset and speed status unknown; waiting for a new controller reply");
    }
    {
      StatusDisplayAccess d;
      d.idle(now);
      d.rs_reply(now - 2, 3, 0, 4, 6, false);
      d.expect(now, "Reset paused; continuation unavailable; inspect the desk");
    }
    {
      StatusDisplayAccess d;
      d.idle(now);
      d.rs_reply(now - 2);
      require(d.reset_speed_.probe(now), "background GET starts");
      require(d.reset_speed_blocking_(), "GET still serializes commands");
      d.expect(now, "Ready", "Reading reset and speed status");
      esphome::text_sensor::TextSensor sensor;
      d.control_status_ = &sensor;
      unsigned publishes = 0;
      sensor.add_on_state_callback([&](const std::string&) {
        ++publishes;
      });
      for (unsigned i = 0; i < 100; ++i)
        d.controls_loop_(now);
      require(publishes == 1 && sensor.state == "Ready", "background GET does not flicker");
      d.firmware_export_active_ = true;
      d.expect(now, "Firmware read active; desk controls paused");
      d.firmware_export_active_ = false;
      d.release_fault_ = true;
      d.expect(now, "Button release uncertain; inspect desk and restart ESP");
      d.release_fault_ = false;
      d.stop_latched_ = true;
      d.expect(now, "Stop not confirmed by controller; movement controls blocked");
      d.stop_acknowledged_ = true;
      d.expect(now, "Stop: waiting for settled height and preset cleanup");
      d.stop_latched_ = false;
      d.expect(now + 3000,
               "No recent height reading; wake the panel or check the controller connection");
      d.last_height_change_ = now;
      d.expect(now, "Waiting for height to settle");
    }
    {
      StatusDisplayAccess d;
      d.idle(now);
      d.cl_reply(now - 2);
      require(d.panel_lock_.probe(now), "CL01 background GET");
      d.expect(now, "Ready", "Panel-lock request in progress");
      esphome::text_sensor::TextSensor sensor;
      d.panel_lock_status_ = &sensor;
      d.publish_panel_lock_(now);
      require(sensor.state == "Unlocked", "CL01 confirmed status retained during GET");
      d.panel_lock_.disconnect("lost", false);
      d.publish_panel_lock_(now);
      require(sensor.state != "Unlocked", "CL01 loss immediately visible");
    }
    {
      StatusDisplayAccess d;
      d.idle(now);
      d.cl_reply(now - 2);
      require(d.panel_lock_.set(true, now), "CL01 SET");
      d.expect(now, "Lock requested; waiting for controller confirmation",
               "Panel-lock request in progress");
    }
    {
      StatusDisplayAccess d;
      d.idle(now);
      d.last_preset_recall_ = now - 5000;
      static esphome::desk_uart::PanelLockSwitch lock;
      d.panel_lock_switch_ = &lock;
      d.cl_reply(now - 2, true, true);
      d.expect(now, "Panel-lock saving failed; locked now, lock state after power loss uncertain",
               "Ready");
    }
    {
      StatusDisplayAccess d;
      d.idle(now);
      static esphome::desk_uart::PanelLockSwitch lock;
      d.panel_lock_switch_ = &lock;
      d.cl_reply(now - 2, true);
      d.expect(now, "Ready");
    }
    {
      StatusDisplayAccess d;
      d.idle(now);
      static esphome::desk_uart::PanelLockSwitch lock;
      d.panel_lock_switch_ = &lock;
      d.cl_reply(now - 2, false, false, true);
      d.expect(now, "Unlocked; panel buttons in use", "Ready");
    }
    {
      StatusDisplayAccess d;
      d.idle(now);
      require(d.preset_settings_.begin_read(1, now), "preset read");
      d.expect(now, "Ready", "Reading or updating preset height");
      require(!d.preset_access_allowed_(now) || d.preset_settings_.busy(),
              "preset read retains busy gate");
    }
    {
      StatusDisplayAccess d;
      d.idle(now);
      require(d.preset_settings_.begin_write(3, 830, now), "preset write");
      d.expect(now, "Reading or updating preset height");
    }
    {
      StatusDisplayAccess d;
      d.idle(now);
      d.limit_phase_ = LimitPhase::Read;
      d.expect(now, "Ready", "Load setting read or write in progress");
      require(!d.preset_access_allowed_(now), "load read still serializes commands");
      d.limit_phase_ = LimitPhase::WriteAck;
      d.expect(now, "Load setting read or write in progress");
      d.limit_phase_ = LimitPhase::Verify;
      d.expect(now, "Load setting read or write in progress");
    }
    {
      StatusDisplayAccess d;
      d.idle(now);
      d.rs_reply(now - 2);
      require(d.reset_speed_.set_speed(true, now), "speed write");
      d.expect(now, "Updating motion speed; awaiting acknowledgement");
      esphome::text_sensor::TextSensor result;
      d.command_result_ = &result;
      require(
          !d.recall_preset(1) && result.state == "Updating motion speed; awaiting acknowledgement",
          "speed busy is not a reset error");
      require(!d.set_timer(30) && result.state == "Updating motion speed; awaiting acknowledgement",
              "timer speed busy reason");
    }
    for (uint8_t state : {uint8_t(1), uint8_t(3), uint8_t(5)}) {
      StatusDisplayAccess d;
      d.idle(now);
      d.rs_reply(now - 2, state);
      d.expect(now, state == 1   ? "Reset descending; ESPHome control not confirmed"
                    : state == 3 ? "Reset paused; select Reset to continue"
                                 : "Reset fault; power-cycle the controller");
      d.reset_speed_.disconnect("lost");
      d.expect(now, "lost");
    }
    {
      StatusDisplayAccess d;
      d.idle(now);
      d.rs_reply(now - 2);
      require(d.reset_speed_.begin(false, now), "START awaiting ACK");
      d.expect(now, "Waiting for the controller to confirm the reset or speed request");
    }
    availability_checks();
    maintenance_capability_checks();
    ESP_LOGI("status_test", "presentation checks passed");
  }
};
