#include "desk_protocol/protocol.h"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

using namespace desk_protocol;
#define CHECK(x) do { if (!(x)) { std::cerr << __FILE__ << ':' << __LINE__ << ": " #x "\n"; std::exit(1); } } while (false)

static Frame parse(const std::vector<uint8_t> &wire) {
  Parser p; Frame f; unsigned ready = 0;
  for (auto b : wire) if (p.feed(b, 10, f) == ParseResult::FrameReady) ++ready;
  CHECK(ready == 1); return f;
}
static void wire_equals(const Frame &frame, const std::vector<uint8_t> &expected) {
  Packet p; CHECK(encode(frame, p));
  CHECK(std::vector<uint8_t>(p.bytes, p.bytes + p.size) == expected);
}

static void golden_commands() {
  // Bytes captured on real hardware, independent of encode().
  wire_equals(status_query(), {0xFA,0xA1,0xA1,0xFD});
  wire_equals(stop(), {0xFA,0x17,0x01,0,0,0x16,0xFD});
  Frame f;
  CHECK(telemetry(2,500,f)); wire_equals(f,{0xFA,0x13,2,0,1,0xF4,0,0,0xE4,0xFD});
  CHECK(telemetry(2,0xFFFF,f)); wire_equals(f,{0xFA,0x13,2,0,0xFF,0xFF,0,0,0x11,0xFD});
  CHECK(write_preset_raw(3,8,f)); wire_equals(f,{0xFA,0x19,0x1F,0,8,0x0E,0xFD});
  CHECK(button(Button::Preset3,true,f)); wire_equals(f,{0xFA,0x17,8,1,0x1E,0xFD});
  CHECK(button(Button::Preset3,false,f)); wire_equals(f,{0xFA,0x17,8,0,0x1F,0xFD});
  CHECK(read_load_limit(LoadLimit::Upward,f)); wire_equals(f,{0xFA,0x15,0x0E,0,0,0x1B,0xFD});
  CHECK(write_load_limit(LoadLimit::Upward,2700,f)); wire_equals(f,{0xFA,0x19,0x0E,0x0A,0x8C,0x91,0xFD});
  // Downward vectors are static protocol fixtures, not physical test evidence.
  CHECK(read_load_limit(LoadLimit::DownwardBase,f)); wire_equals(f,{0xFA,0x15,0x0F,0,0,0x1A,0xFD});
  CHECK(write_load_limit(LoadLimit::DownwardBase,3200,f)); wire_equals(f,{0xFA,0x19,0x0F,0x0C,0x80,0x9A,0xFD});
  CHECK(!read_load_limit(static_cast<LoadLimit>(0x10),f)); CHECK(f.size==0);
  CHECK(!write_load_limit(static_cast<LoadLimit>(0x13),123,f));
  CHECK(downward_motion_cutoff(3200)==4400); CHECK(downward_motion_cutoff(65535)==66735);
  LoadLimitValue limit;
  Frame readback=parse({0xFA,0x16,0x0F,0x0C,0x80,0,1,0x94,0xFD});
  CHECK(decode_load_limit(readback,limit)); CHECK(limit.limit==LoadLimit::DownwardBase && limit.raw==3200 && limit.sequence==1);
  readback.size=5; CHECK(!decode_load_limit(readback,limit));
  readback.size=7; CHECK(!decode_load_limit(readback,limit));
  readback.size=6; readback.bytes[1]=0x10; CHECK(!decode_load_limit(readback,limit));
  CHECK(!telemetry(1,500,f)); CHECK(f.size == 0);
  CHECK(!telemetry(2,0,f)); CHECK(!write_preset_raw(0,0,f)); CHECK(!write_preset_raw(4,8,f));
  CHECK(!button(static_cast<Button>(2),true,f));
  Packet p; CHECK(!encode(f,p)); CHECK(p.size == 0);
  f.size = MAX_BODY+1; CHECK(!encode(f,p));
}

static void parser_edges() {
  // FA 10 FE FA FE FD FE FE E9 FD: escaped data and hand-computed XOR.
  Frame f = parse({0xFA,0x10,0xFE,0xFA,0xFE,0xFD,0xFE,0xFE,0xE9,0xFD});
  CHECK(f.size == 4 && f.bytes[1] == 0xFA && f.bytes[2] == 0xFD && f.bytes[3] == 0xFE);
  f = parse({0xFA,0x13,0xE9,0xFE,0xFA,0xFD}); // escaped checksum FA
  CHECK(f.size == 2 && f.bytes[1] == 0xE9);
  Parser p;
  CHECK(p.feed(0xFD,0,f) == ParseResult::None);
  p.feed(0xFA,0,f); CHECK(p.feed(0xFD,0,f) == ParseResult::BadLength);
  p.feed(0xFA,0,f);p.feed(0xA1,0,f);p.feed(0,0,f);
  CHECK(p.feed(0xFD,0,f) == ParseResult::BadChecksum);
  // Noise, interrupted packet, and consecutive frames.
  unsigned count=0;
  for (auto b : std::vector<uint8_t>{0,0xFA,9,8,0xFA,0xA1,0xA1,0xFD,0xFA,0xA1,0xA1,0xFD})
    if (p.feed(b,0,f) == ParseResult::FrameReady) ++count;
  CHECK(count == 2);
  p.feed(START,0,f);
  for (std::size_t i=0;i<MAX_BODY+1;++i) CHECK(p.feed(1,0,f)==ParseResult::None);
  CHECK(p.feed(1,0,f)==ParseResult::Overflow);
  p.feed(START,0,f);p.feed(0xA1,0,f);p.feed(0xA1,0,f);
  CHECK(p.feed(END,0,f)==ParseResult::FrameReady);
  p.feed(START,0xFFFFFFF0u,f);p.feed(ESCAPE,0xFFFFFFF0u,f);
  CHECK(!p.expire(0x20)); CHECK(p.expire(0x100));
  CHECK(p.feed(END,0x100,f)==ParseResult::None);
  // Feed timeout discards a partial frame, then accepts the new start byte.
  p.feed(START,1000,f);p.feed(0xA1,1001,f);
  p.feed(START,1400,f);p.feed(0xA1,1401,f);p.feed(0xA1,1402,f);
  CHECK(p.feed(END,1403,f)==ParseResult::FrameReady);
  // Every possible byte, at maximum supported body size.
  for(unsigned seed=0;seed<256;++seed) {
    Frame input; input.size=MAX_BODY;
    for(std::size_t i=0;i<input.size;++i) input.bytes[i]=static_cast<uint8_t>(seed+i);
    Packet packet; CHECK(encode(input,packet)); CHECK(packet.size<=MAX_WIRE);
    Frame decoded=parse(std::vector<uint8_t>(packet.bytes,packet.bytes+packet.size));
    CHECK(decoded.size==input.size);
    for(std::size_t i=0;i<input.size;++i) CHECK(decoded.bytes[i]==input.bytes[i]);
  }
  // Arbitrary streams must stay bounded and recover to a valid frame.
  uint32_t rng=12345;
  for(unsigned i=0;i<100000;++i) { rng=rng*1664525u+1013904223u; p.feed(static_cast<uint8_t>(rng>>24),i,f); }
  p.reset();p.feed(START,0,f);p.feed(0xA1,0,f);p.feed(0xA1,0,f);
  CHECK(p.feed(END,0,f)==ParseResult::FrameReady);
}

static void typed_messages_and_timing() {
  Frame f=parse({0xFA,0xA2,0,0,0,0xA2,0xFD});Status status;
  CHECK(decode_status(f,status)); CHECK(status.coarse_state==0 && status.sequence==0);
  Height h;CHECK(!decode_height(f,h));
  AckTracker a;
  CHECK(a.begin(0x18,8,0,0xFFFFFFF0u,100));CHECK(!a.begin(0x18,8,0,0));
  Frame press=parse({0xFA,0x18,8,1,0,0,1,0x10,0xFD});
  CHECK(!a.accept(press,0)); CHECK(a.pending());
  Frame release=parse({0xFA,0x18,8,0,0,0,2,0x12,0xFD});
  CHECK(a.accept(release,0x20));CHECK(!a.pending());
  CHECK(a.begin(0x18,8,0,1000,100));CHECK(!a.accept(release,1100));CHECK(a.pending());CHECK(a.expired(1100));
  CHECK(a.begin(0x18,8,0,2000,100));CHECK(!a.expired(2099));CHECK(a.expired(2100));
  // Actual preset-write ACK captured during the stop preflight. It is one byte
  // shorter than a button ACK, and sequence high byte is deliberately nonzero.
  Frame parameter=parse({0xFA,0x1A,0x1F,0,1,0x19,0x1D,0xFD});Ack pa;
  CHECK(decode_ack(parameter,pa));CHECK(pa.kind==0x1A && pa.first==0x1F && pa.second==0);
  CHECK(pa.sequence==0x0119 && pa.reserved_byte==0);
  CHECK(a.begin(0x1A,0x1F,0,2200));CHECK(a.accept(parameter,2201));
  parameter.size=6;CHECK(!decode_ack(parameter,pa));
  // Real brake ACK from the upward trial; selector1 is distinct from button ACKs.
  Frame brake_ack=parse({0xFA,0x18,1,0,0,5,0x6D,0x71,0xFD});
  CHECK(a.begin(0x18,1,0,2300));CHECK(!a.accept(release,2301));
  CHECK(a.accept(brake_ack,2302));CHECK(!a.pending());
  CHECK(!button(static_cast<Button>(1),false,f));
  ButtonPulse pulse;
  CHECK(pulse.begin(Button::Preset3,0xFFFFFFF0u,f));
  CHECK(!pulse.begin(Button::Preset1,0,f));CHECK(!pulse.poll(0x85,f));
  CHECK(pulse.poll(0x86,f));CHECK(f.bytes[1]==8 && f.bytes[2]==0);CHECK(!pulse.poll(0x87,f));
  CHECK(pulse.begin(Button::Timer,0,f));CHECK(pulse.cancel(f));CHECK(!pulse.active());
  HeightMonitor m; CHECK(!m.fresh(0));
  f=Frame{};f.size=32;f.bytes[0]=5;f.bytes[20]=6;f.bytes[30]=0xFF;f.bytes[31]=0xFF;
  CHECK(m.observe(f,0xFFFFFFF0u));CHECK(m.fresh(0x20));CHECK(!m.observe(f,0x20));
  f.bytes[30]=0;f.bytes[31]=0;CHECK(m.observe(f,0x20));
  f.bytes[30]=0xFF;f.bytes[31]=0xFE;CHECK(!m.observe(f,0x30));
  CHECK(!m.fresh(0x20+3000));CHECK(m.observe(f,0x20+3000));
  f.size=31;CHECK(!decode_height(f,h));f.size=33;CHECK(!decode_height(f,h));
  CHECK(metric_display(0).displayed_tenths_cm==725);
  for(uint16_t q=4;q<=7;++q) CHECK(metric_display(q).displayed_tenths_cm==730);
  CHECK(metric_display(14).displayed_tenths_cm==739);
  CHECK(metric_display(274).displayed_tenths_cm==999);
  CHECK(metric_display(275).decimal_places==0);
  CHECK(metric_display(276).displayed_tenths_cm==1000);
  for(uint16_t q=457;q<=459;++q) {
    CHECK(metric_display(q).before_rounding_tenths_cm==1188);
    CHECK(metric_display(q).displayed_tenths_cm==1180);
  }
  CHECK(metric_display(65535).before_rounding_tenths_cm==66260);
}

static void replay(const char *name,unsigned expected_frames,unsigned expected_heights,bool stable) {
  std::ifstream file(std::string(DESK_FIXTURES)+"/"+name,std::ios::binary);CHECK(file.good());
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),{});
  Parser parser;Frame f;unsigned frames=0,heights=0;uint16_t previous=0;
  // Chunking a byte-stream at every possible split must not change its result.
  for(std::size_t split=0;split<=bytes.size();++split) {
    parser.reset();frames=heights=0;
    for(int chunk=0;chunk<2;++chunk) {
      const auto begin=chunk==0?0:split, end=chunk==0?split:bytes.size();
      for(auto i=begin;i<end;++i) {
        auto result=parser.feed(bytes[i],static_cast<uint32_t>(i),f);
        CHECK(result!=ParseResult::BadChecksum && result!=ParseResult::Overflow);
        if(result!=ParseResult::FrameReady) continue;
        ++frames;Height h;
        if(decode_height(f,h)) {
          if(stable) {
            CHECK(h.q==6);
            if(heights) CHECK(static_cast<uint16_t>(h.sequence-previous)==1);
          }
          previous=h.sequence;++heights;
        }
      }
    }
    CHECK(frames==expected_frames && heights==expected_heights);
  }
}

int main() {
  golden_commands();parser_edges();typed_messages_and_timing();
  replay("channel2-120s.bin",241,239,true);
  replay("preset3-73.bin",21,15,false);
  std::cout << "Protocol, parser recovery, wraparound, ACK, pulse and live-capture replay tests passed.\n";
}
