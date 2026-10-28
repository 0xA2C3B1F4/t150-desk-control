#include "desk_protocol/presets.h"
#include <cstdlib>
#include <iostream>
using namespace desk_protocol;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while(false)
static Frame value(uint8_t selector,uint16_t raw) { Frame f;f.size=6;f.bytes[0]=0x16;f.bytes[1]=selector;f.bytes[2]=raw>>8;f.bytes[3]=raw&255;return f; }
static Frame ack(uint8_t slot,uint8_t result=0) { Frame f;f.size=5;f.bytes[0]=0x1A;f.bytes[1]=0x1C+slot;f.bytes[2]=result;return f; }
static Frame take(PresetSettings &s,uint8_t kind,uint8_t selector) { Frame f;CHECK(s.take_frame(f));CHECK(f.bytes[0]==kind && f.bytes[1]==selector);return f; }
static void scale(PresetSettings &s,uint32_t start=0) { take(s,0x15,11);s.accept(value(12,39),start);Frame ignored;CHECK(!s.take_frame(ignored));s.accept(value(11,24),start+1);take(s,0x15,12);s.accept(value(12,39),start+2); }
int main() {
 Frame f;uint16_t cm=0;PresetSettings::Result r{};
 CHECK(!read_preset_raw(0,f) && !read_preset_raw(4,f));CHECK(!preset_height(0,cm));CHECK(!preset_height(741,cm));
 CHECK(preset_height(8,cm)&&cm==730);CHECK(preset_height(456,cm)&&cm==1005);CHECK(preset_height(740,cm)&&cm==1180);
 for(uint8_t slot=1;slot<=3;++slot) {
  PresetSettings s;CHECK(s.begin_read(slot,0));CHECK(!s.begin_write(slot,800,0));scale(s);take(s,0x15,0x1C+slot);
  s.accept(value(0x1C+(slot%3+1),90),3);CHECK(!s.take_result(r));s.accept(value(0x1C+slot,90),4);
  CHECK(s.take_result(r)&&r.success&&r.raw==90&&r.slot==slot&&!r.write&&!s.busy());
  CHECK(s.begin_write(slot,800,10));scale(s,10);f=take(s,0x19,0x1C+slot);CHECK(be16(f.bytes+2)==122);
  s.accept(ack(slot%3+1),13);CHECK(!s.take_frame(f));s.accept(ack(slot),14);take(s,0x15,0x1C+slot);
  s.accept(value(0x1C+slot,122),15);CHECK(s.take_result(r)&&r.success&&r.write&&r.value_known);
 }
 {
  PresetSettings s;CHECK(!s.begin_write(1,729,0));CHECK(!s.begin_write(1,1181,0));CHECK(s.begin_write(1,800,0));
  take(s,0x15,11);s.accept(value(11,25),1);CHECK(s.take_result(r)&&!r.success&&!r.value_known);CHECK(!s.take_frame(f));
  CHECK(s.begin_write(1,800,2));take(s,0x15,11);s.accept(value(11,24),3);take(s,0x15,12);s.accept(value(12,40),4);CHECK(s.take_result(r)&&!r.success&&!s.take_frame(f));
 }
 {
  PresetSettings s;CHECK(s.begin_write(2,800,0));scale(s);take(s,0x19,30);s.accept(ack(2),3);take(s,0x15,30);
  s.accept(value(30,90),4);CHECK(s.take_result(r)&&!r.success&&r.value_known&&r.raw==90);CHECK(!s.take_frame(f));
 }
 {
  PresetSettings s;CHECK(s.begin_write(3,800,0));scale(s);take(s,0x19,31);s.tick(702);CHECK(s.take_result(r)&&!r.success&&!r.value_known);
  s.accept(ack(3),703);CHECK(!s.take_frame(f)&&!s.busy());
  CHECK(s.begin_write(1,800,1000));scale(s,1000);s.cancel("Stop");CHECK(s.take_result(r)&&!r.success);CHECK(!s.take_frame(f));
 }
 {
  PresetSettings s;const uint32_t n=0xFFFFFF00U;CHECK(s.begin_read(1,n));take(s,0x15,11);s.tick(n+700);CHECK(s.take_result(r)&&!r.success);
 }
 std::cout << "Preset reads, scaling, writes, ACK/readback, mismatch, cancellation and timeout passed\n";
}
