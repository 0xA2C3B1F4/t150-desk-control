#pragma once
#include "esphome/components/desk_uart/desk_uart.h"
#include <cstdlib>
#include <cmath>
#include <deque>
#include <memory>
#include <vector>
namespace firmware_export_test {
class Wire : public esphome::uart::UARTComponent {
 public:
  size_t writes{0}, read_bytes{0}, stall_at_byte{0};
  std::deque<uint8_t> incoming;
  void feed(const desk_protocol::Frame &frame) {
    desk_protocol::Packet packet;
    if (!desk_protocol::encode(frame,packet)) std::exit(1);
    incoming.insert(incoming.end(),packet.bytes,packet.bytes+packet.size);
  }
  void check_logger_conflict() override {}
  void write_array(const uint8_t *, size_t size) override { writes += size; }
  bool peek_byte(uint8_t *value) override {
    if(incoming.empty()) return false;
    *value=incoming.front();return true;
  }
  bool read_array(uint8_t *data, size_t size) override {
    if(size>incoming.size()) return false;
    for(size_t i=0;i<size;++i){
      if (++read_bytes==stall_at_byte) esphome::delay(550);
      data[i]=incoming.front();incoming.pop_front();
    }
    return true;
  }
  size_t available() override { return incoming.size(); }
  esphome::uart::UARTFlushResult flush() override { return esphome::uart::UARTFlushResult::UART_FLUSH_RESULT_SUCCESS; }
};
class Probe : public esphome::desk_uart::DeskUART {
 public:
  static void run(esphome::sensor::Sensor *height, esphome::sensor::Sensor *load) {
    Wire wire;
    auto ready=[&](Probe &p){ready_(p,wire,height,load);};
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
    Probe p;ready(p);
    auto sample=[&](uint16_t raw,uint16_t sequence){
      wire.feed(frame_(raw,sequence));p.loop();
      require(wire.incoming.empty(),"sample consumed by real parser");
    };
    sample(123,1);require(load->state==123,"load published from accepted telemetry");
    const auto invalidations=std::make_shared<unsigned>(0);
    load->add_on_state_callback([invalidations](float value){if(std::isnan(value)) ++*invalidations;});
    require(p.begin_firmware_export(),"idle handoff");
    require(std::isnan(load->state)&&*invalidations==1,"handoff invalidates load once");
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
    require(std::isnan(load->state)&&*invalidations==1,"export keeps load unknown without repeat publication");
    p.end_firmware_export();require(!p.firmware_export_active(),"ownership released");
    require(std::isnan(load->state)&&*invalidations==1,"release keeps load unknown");
    require(!p.move_to_height(85),"stale telemetry must not permit queued movement");
    p.loop();require(wire.writes>0,"normal telemetry polling resumes");
    require(std::isnan(load->state),"subscription alone cannot restore load");
    sample(456,0);require(load->state==456,"fresh post-export sample restores load");
    callback_export_(false);callback_export_(true);
    callback_delay_(false);callback_delay_(true);
    callback_direct_brake_();
    byte_stall_timeout_();
    ESP_LOGI("fixture","FIRMWARE_HANDOFF=passed");
  }
 private:
  static desk_protocol::Frame frame_(uint16_t raw,uint16_t sequence) {
    desk_protocol::Frame f;f.size=32;f.bytes[0]=5;f.bytes[20]=6;
    f.bytes[3]=static_cast<uint8_t>(raw>>8);f.bytes[4]=static_cast<uint8_t>(raw);
    f.bytes[30]=static_cast<uint8_t>(sequence>>8);f.bytes[31]=static_cast<uint8_t>(sequence);
    return f;
  }
  static void ready_(Probe &p,Wire &wire,esphome::sensor::Sensor *height,esphome::sensor::Sensor *load) {
    p.set_uart_parent(&wire);p.set_height_sensor(height);p.set_load_raw_sensor(load);
    p.target_enabled_=p.timer_enabled_=p.motion_enabled_=true;
    const uint32_t now=esphome::millis();
    require(p.monitor_.observe(frame_(0,0),now),"fresh telemetry");
    p.height_seen_=p.connected_=true;p.last_q_=6;
    p.last_height_received_=now;p.last_height_change_=now-2000;p.last_preset_recall_=now-4000;
  }
  static void byte_stall_timeout_() {
    Wire wire;
    esphome::sensor::Sensor height,load;
    Probe p;ready_(p,wire,&height,&load);
    wire.stall_at_byte=4;wire.feed(frame_(0,1));p.loop();
    require(p.frame_errors_==1,"mid-frame stall timeout counted");
    require(p.uart_observation_.snapshot().errors==1,"cumulative timeout counted");
  }
  static void callback_direct_brake_() {
    Wire wire;
    esphome::sensor::Sensor height,load;
    Probe p;ready_(p,wire,&height,&load);p.direct_mode_=true;p.direct_probe_done_=true;
    const uint32_t now=esphome::millis();
    desk_protocol::Frame request, response;
    auto respond=[&](const desk_protocol::Frame &r) {
      response=desk_protocol::Frame{};response.size=37;
      response.bytes[0]=0xD2;response.bytes[1]=1;response.bytes[2]=3;response.bytes[3]=r.bytes[2];
      std::memcpy(response.bytes+5,"DT02",4);
      response.bytes[9]=r.bytes[3];response.bytes[10]=r.bytes[4];
      response.bytes[11]=r.bytes[5];response.bytes[12]=r.bytes[6];response.bytes[14]=100;
      response.bytes[18]=r.bytes[2]?6:100;response.bytes[22]=6;
      response.bytes[25]=2;response.bytes[26]=0xEB;response.bytes[28]=24;response.bytes[30]=39;
      response.bytes[31]=r.bytes[2]?1:0;
    };
    require(p.direct_.begin(8,6,now-4),"begin direct fixture");
    require(p.direct_.take_frame(request),"GET fixture");respond(request);p.direct_.accept(response,now-3);
    require(p.direct_.take_frame(request),"SET fixture");respond(request);p.direct_.accept(response,now-2);
    require(p.direct_.active(),"accepted direct fixture");
    p.direct_.disconnect();require(p.direct_.brake_pending(),"uncertain active owner needs brake");
    unsigned attempts=0;
    height.add_on_state_callback([&](float) {
      ++attempts;
      require(!p.begin_firmware_export(),"callback cannot steal pending safety brake");
      require(!p.recall_preset(1),"callback preset cannot discard pending safety brake");
      require(!p.recovery_access_allowed_(esphome::millis()),"recovery waits for pending brake");
      require(!p.preset_access_allowed_(esphome::millis()),"parameter access waits for pending brake");
    });
    wire.feed(frame_(0,1));p.loop();
    require(attempts==1,"height callback exercised");
    require(p.stop_latched_&&wire.writes>0,"pending brake reached real UART Stop path");
    require(p.uart_observation_.snapshot().handoffs==0,"rejected handoff not counted");
  }
  static void callback_export_(bool from_load) {
    Wire wire;
    esphome::sensor::Sensor height,load;
    Probe p;ready_(p,wire,&height,&load);
    load.publish_state(NAN);
    bool requested=false;
    auto handoff=[&](float value){
      if(!requested&&!std::isnan(value)) {
        requested=true;
        require(p.begin_firmware_export(),"callback handoff accepted");
      }
    };
    if(from_load) load.add_on_state_callback(handoff);
    else height.add_on_state_callback(handoff);
    wire.feed(frame_(123,1));
    const auto first_size=wire.incoming.size();
    wire.feed(frame_(999,2));
    const auto queued_size=wire.incoming.size()-first_size;
    p.loop();
    require(requested&&p.firmware_export_active(),"callback hands ownership to reader");
    require(wire.incoming.size()==queued_size,"callback handoff leaves next frame unconsumed");
    require(std::isnan(load.state),"callback handoff leaves load unknown");
    require(wire.writes==0,"callback handoff emits no normal UART traffic");
    // The export owner consumes its stream before returning the UART.
    wire.incoming.clear();p.end_firmware_export();
    require(std::isnan(load.state),"callback release needs fresh telemetry");
    wire.feed(frame_(456,0));p.loop();
    require(load.state==456&&!p.firmware_export_active(),"callback release recovers on fresh sample");
  }
  static void callback_delay_(bool from_load) {
    Wire wire;
    esphome::sensor::Sensor height,load;
    Probe p;p.set_uart_parent(&wire);p.set_height_sensor(&height);p.set_load_raw_sensor(&load);
    p.last_subscribe_=esphome::millis();load.publish_state(NAN);
    bool delayed=false;
    std::vector<float> load_values;
    load.add_on_state_callback([&](float value){load_values.push_back(value);});
    auto delay=[&](float value){
      if(!delayed&&!std::isnan(value)){delayed=true;esphome::delay(3050);}
    };
    if(from_load) load.add_on_state_callback(delay);
    else height.add_on_state_callback(delay);
    wire.feed(frame_(123,1));p.loop();
    require(delayed&&!p.connected_&&!p.monitor_.fresh(esphome::millis()),"callback crosses first-sample freshness deadline");
    require(std::isnan(load.state),"delayed callback leaves expired load unknown");
    require(wire.writes==0,"callback delay adds no UART traffic");
    if(from_load) {
      require(load_values.size()==2&&load_values[0]==123&&std::isnan(load_values[1]),"load callback invalidates before a connected edge");
    } else require(load_values.empty(),"height callback cannot publish expired load");
    wire.feed(frame_(456,2));p.loop();
    require(load.state==456&&p.connected_,"delayed callback recovers on a new sample");
  }
  static void require(bool ok,const char *message){if(!ok){ESP_LOGE("fixture","%s",message);std::exit(1);}}
};
}
