#pragma once
#include "esphome/components/desk_uart/desk_uart.h"
#include <cstdlib>
namespace firmware_export_test {
class Wire : public esphome::uart::UARTComponent {
 public:
  size_t writes{0};
  void check_logger_conflict() override {}
  void write_array(const uint8_t *, size_t size) override { writes += size; }
  bool peek_byte(uint8_t *) override { return false; }
  bool read_array(uint8_t *, size_t) override { return false; }
  size_t available() override { return 0; }
  esphome::uart::UARTFlushResult flush() override { return esphome::uart::UARTFlushResult::UART_FLUSH_RESULT_SUCCESS; }
};
class Probe : public esphome::desk_uart::DeskUART {
 public:
  static void run(esphome::sensor::Sensor *height) {
    Wire wire;
    auto ready=[&](Probe &p){
      p.set_uart_parent(&wire);p.set_height_sensor(height);
      p.target_enabled_=p.timer_enabled_=p.motion_enabled_=true;
      const uint32_t now=esphome::millis();
      desk_protocol::Frame f;f.size=32;f.bytes[0]=5;f.bytes[20]=6;
      require(p.monitor_.observe(f,now),"fresh telemetry");
      p.height_seen_=p.connected_=true;p.last_height_received_=now;p.last_height_change_=now-2000;p.last_preset_recall_=now-4000;
    };
    {Probe p;require(!p.begin_firmware_export(),"stale telemetry must refuse handoff");}
    for(unsigned scenario=0;scenario<9;++scenario){
      Probe p;ready(p);
      switch(scenario){
        case 0:p.keep_awake_active_=true;break;
        case 1:p.target_move_.recover(730,850);break;
        case 2:p.stop_latched_=true;break;
        case 3:p.limit_phase_=LimitPhase::Read;break;
        case 4:p.identity_pending_=0;break;
        case 5:p.timer_.begin(30,esphome::millis());break;
        case 6:p.release_ack_.begin(0x18,1,0,esphome::millis());break;
        case 7:p.recovery_check_active_=true;break;
        case 8:p.last_height_change_=esphome::millis();break;
      }
      require(!p.begin_firmware_export(),"busy handoff must fail");
    }
    Probe p;ready(p);require(p.begin_firmware_export(),"idle handoff");
    require(!p.begin_firmware_export(),"second handoff refused");
    require(!p.monitor_.fresh(esphome::millis())&&!p.connected_,"telemetry invalidated");
    require(!p.recall_preset(1),"preset blocked");require(!p.move_to_height(85),"target blocked");
    require(!p.step_height(true),"step blocked");require(!p.set_preset_height(1,85),"preset edit blocked");
    require(!p.set_load_limit(desk_protocol::LoadLimit::Upward,5000),"load edit blocked");
    require(!p.restore_load_limit(desk_protocol::LoadLimit::Upward),"load restore blocked");
    require(!p.set_timer(30)&&!p.cancel_timer(),"timer blocked");
    require(!p.restore_temporary_preset()&&!p.accept_current_preset(),"recovery blocked");
    require(!p.stop()&&!p.clear_stop_latch()&&!p.stop_latched_,"stop must not mutate or transmit in loader mode");
    p.loop();p.on_shutdown();require(wire.writes==0,"no normal UART traffic during export");
    p.end_firmware_export();require(!p.firmware_export_active(),"ownership released");
    require(!p.move_to_height(85),"stale telemetry must not permit queued movement");
    p.loop();require(wire.writes>0,"normal telemetry polling resumes");
    ESP_LOGI("fixture","FIRMWARE_HANDOFF=passed");
  }
 private:
  static void require(bool ok,const char *message){if(!ok){ESP_LOGE("fixture","%s",message);std::exit(1);}}
};
}
