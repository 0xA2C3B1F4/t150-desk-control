#include "desk_protocol/direct_target.h"
#include <cstdlib>
#include <iostream>
using namespace desk_protocol;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while(false)
static void put16(Frame &f, unsigned p, uint16_t v) { f.bytes[p]=v>>8; f.bytes[p+1]=v; }
static void put32(Frame &f, unsigned p, uint32_t v) { put16(f,p,v>>16); put16(f,p+2,v); }
static Frame reply(const Frame &request, int32_t t=100, uint8_t status=0, uint16_t guards=0) {
 Frame f; f.size=37; f.bytes[0]=0xD2; f.bytes[1]=1; f.bytes[2]=3;
 f.bytes[3]=request.bytes[2]; f.bytes[4]=status;
 f.bytes[5]='D'; f.bytes[6]='T'; f.bytes[7]='0'; f.bytes[8]='2';
 put16(f,9,be16(request.bytes+3)); put16(f,11,be16(request.bytes+5)); put16(f,13,100);
 put32(f,15,static_cast<uint32_t>(t)); put32(f,19,6); put32(f,23,747);
 put16(f,27,24); put16(f,29,39); f.bytes[31]=request.bytes[2]?1:0; put16(f,33,guards); return f;
}
static Frame begin(DirectTarget &d, uint16_t s=8, uint32_t now=0) {
 Frame q; CHECK(d.begin(s,6,now)); CHECK(d.take_frame(q)); CHECK(q.size==7 && q.bytes[0]==0xD1 && q.bytes[2]==0);
 d.accept(reply(q),now+1); Frame set; CHECK(d.take_frame(set)); CHECK(set.bytes[2]==1 && be16(set.bytes+5)==s); return set;
}
int main() {
 Frame f;
 { DirectTarget d; d.seed(0xFFFF); CHECK(d.probe(0)); CHECK(d.take_frame(f)); CHECK(be16(f.bytes+3)==0xFFFF);
   d.accept(reply(f),1); CHECK(d.known() && d.sleep_wrap() && !d.busy());
   auto set=begin(d); CHECK(be16(set.bytes+3)==1); d.accept(reply(set,6),2);
   CHECK(d.active() && d.take_accepted() && !d.take_accepted()); CHECK(!d.take_frame(f));
   d.observe_q(7); d.tick(2003,true,1500); CHECK(!d.busy()); }
 for (auto pair : {std::pair<uint16_t,int32_t>{8,6},{740,747},{104,104},{105,112},{95,89}}) {
   DirectTarget d; auto set=begin(d,pair.first); d.accept(reply(set,pair.second),2); CHECK(d.active()); }
 for (unsigned offset : {1U,2U,5U,8U,19U,23U,27U,29U,33U}) {
   DirectTarget d; CHECK(d.begin(8,6,0)); CHECK(d.take_frame(f)); auto bad=reply(f);
   bad.bytes[offset]^=0x80; d.accept(bad,1); CHECK(!d.known() && !d.busy()); CHECK(!d.take_frame(f) && !d.take_brake()); }
 { DirectTarget d; auto set=begin(d); auto bad=reply(set,7); d.accept(bad,2);
   CHECK(!d.active() && d.take_brake() && !d.take_brake()); }
 { DirectTarget d; auto set=begin(d); auto rejected=reply(set,100,2,1); d.accept(rejected,2);
   CHECK(!d.busy() && d.known() && !d.take_brake()); }
 for (unsigned field : {3U,9U,11U}) {
   DirectTarget d; auto set=begin(d); auto bad=reply(set,6); bad.bytes[field]^=1;
   d.accept(bad,2); d.tick(801,true,0); CHECK(d.busy() && d.brake_pending()); CHECK(d.take_brake() && !d.busy()); CHECK(!d.take_frame(f)); }
 { DirectTarget d; CHECK(d.begin(8,6,0)); CHECK(d.take_frame(f)); auto blocked=reply(f,100,0,2);
   d.accept(blocked,1); CHECK(d.known() && !d.busy() && !d.take_frame(f) && !d.take_brake()); }
 { DirectTarget d; auto set=begin(d); d.accept(reply(set,6),801);
   CHECK(!d.known() && d.take_brake() && !d.take_frame(f)); }
 { DirectTarget d; CHECK(d.probe(0xFFFFFF00U)); CHECK(d.take_frame(f)); d.tick(0x220U,true,0);
   CHECK(!d.known() && !d.busy() && !d.take_brake()); }
 { DirectTarget d; auto set=begin(d); d.accept(reply(set,6),2); d.disconnect();
   CHECK(d.busy() && d.brake_pending() && !d.probe(3) && !d.begin(90,6,3));
   CHECK(!d.known() && d.take_brake()); d.tick(4000,true,3000); CHECK(!d.take_frame(f)); }
 { DirectTarget d; auto set=begin(d); d.accept(reply(set,6),2); d.cancel();
   CHECK(d.known() && !d.busy() && !d.take_brake() && !d.take_frame(f)); }
 { DirectTarget d; CHECK(d.probe(0)); CHECK(d.take_frame(f)); Frame error; error.size=2; error.bytes[0]=0xEE; error.bytes[1]=3;
   d.accept(error,1); d.tick(800,true,0); CHECK(!d.known() && !d.take_frame(f) && !d.take_brake()); }
 { DirectTarget d; CHECK(d.begin(8,6,0)); CHECK(d.take_frame(f)); d.accept(reply(f),800);
   CHECK(!d.known() && !d.take_frame(f)); }
 { DirectTarget d; CHECK(d.begin(8,6,0)); CHECK(d.take_frame(f)); d.accept(reply(f),1);
   CHECK(d.awaiting_set() && !d.set_sent()); d.cancel(); CHECK(!d.take_frame(f) && !d.take_brake()); }
 std::cout << "DT02 strict binding, compensation, deadlines, wrap, loss and no fallback passed\n";
}
