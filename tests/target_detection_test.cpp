#include "desk_protocol/target_detection.h"
#include <cstdlib>
#include <iostream>
using namespace desk_protocol;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while(false)
static void put16(Frame &f, unsigned p, uint16_t v) { f.bytes[p]=v>>8; f.bytes[p+1]=v; }
static Frame identity(uint8_t selector, uint16_t sequence) {
  const char *const values[]={"T150-22-03","LUMI LEGEND GROUP","1.5.0","1.6","1.9.0118","21/01/18"};
  Frame f; f.size=identity_text_size(selector)+4; f.bytes[0]=0x12; f.bytes[1]=selector;
  const char *text=values[selector==9?5:selector-3];
  std::memcpy(f.bytes+2,text,std::strlen(text)); put16(f,f.size-2,sequence); return f;
}
static Frame rejection(uint16_t seq) { Frame f; f.size=4; f.bytes[0]=0xEE; f.bytes[1]=3; put16(f,2,seq); return f; }
static Frame capability(const Frame &q) {
  Frame f; f.size=37; f.bytes[0]=0xD2; f.bytes[1]=1; f.bytes[2]=3;
  std::memcpy(f.bytes+5,"DT02",4); put16(f,9,be16(q.bytes+3));
  put16(f,13,100); put16(f,21,6); put16(f,25,747); put16(f,27,24); put16(f,29,39); return f;
}
static Frame prepare(TargetDetection &d, uint16_t start=10, uint32_t now=0) {
  CHECK(d.start(now)); Frame q;
  for(uint8_t selector : {3,4,5,6,7,9}) {
    CHECK(d.take_frame(q)); CHECK(q.bytes[0]==0x11 && q.bytes[1]==selector);
    d.accept(identity(selector,start++),now+1);
  }
  for(uint8_t selector : {0x0B,0x0C}) {
    CHECK(d.take_frame(q)); CHECK(q.bytes[0]==0x15 && q.bytes[1]==selector);
    Frame f; f.size=6; f.bytes[0]=0x16; f.bytes[1]=selector; put16(f,2,selector==0x0B?24:39); put16(f,4,start++);
    d.accept(f,now+2);
  }
  CHECK(d.take_frame(q)); CHECK(q.size==7 && q.bytes[0]==0xD1 && q.bytes[2]==0); return q;
}
int main() {
  using B=TargetDetection::Backend;
  Frame q;
  { TargetDetection d; auto get=prepare(d); d.accept(capability(get),3); CHECK(d.backend()==B::Direct); CHECK(!d.take_frame(q));
    d.reset(); CHECK(d.backend()==B::Unknown && d.idle()); auto next=prepare(d); CHECK(be16(next.bytes+3)!=be16(get.bytes+3));
    d.accept(capability(get),3); CHECK(d.backend()==B::Unknown); }
  { TargetDetection d; prepare(d); d.accept(rejection(18),3); CHECK(d.backend()==B::Unknown);
    CHECK(d.take_frame(q) && q.bytes[0]==0x11); d.accept(identity(3,19),4);
    CHECK(d.take_frame(q) && q.bytes[0]==0xD1 && q.bytes[2]==0); d.accept(rejection(20),5);
    CHECK(d.backend()==B::Preset && !d.take_frame(q)); }
  for(unsigned stage=0;stage<11;stage++) { // Every response deadline is fail-closed, including the second rejection.
    TargetDetection d; CHECK(d.start(0)); uint16_t seq=10;
    for(unsigned i=0;i<stage;i++) {
      CHECK(d.take_frame(q));
      if(q.bytes[0]==0x11) d.accept(identity(q.bytes[1],seq++),1);
      else if(q.bytes[0]==0x15) {
        Frame f; f.size=6; f.bytes[0]=0x16; f.bytes[1]=q.bytes[1]; put16(f,2,q.bytes[1]==11?24:39); put16(f,4,seq++); d.accept(f,1);
      } else d.accept(rejection(seq++),1);
    }
    CHECK(d.take_frame(q)); d.tick(801); CHECK(d.backend()==B::Unknown && !d.busy() && d.error()); CHECK(!d.take_frame(q));
  }
  for(unsigned offset : {1U,2U,5U,9U,19U,23U,27U,29U,33U}) {
    TargetDetection d; auto get=prepare(d); auto bad=capability(get); bad.bytes[offset]^=0x80; d.accept(bad,3); d.tick(803);
    CHECK(d.backend()==B::Unknown && d.error());
  }
  for(uint16_t seq : {17,16,0x8011}) { TargetDetection d; prepare(d); d.accept(rejection(seq),3); CHECK(d.backend()==B::Unknown && d.error()); }
  { TargetDetection d; CHECK(d.start(0)); CHECK(d.take_frame(q)); auto f=identity(3,1); f.bytes[2]='X'; d.accept(f,1);
    CHECK(d.error() && !d.take_frame(q)); }
  { TargetDetection d; prepare(d); auto f=rejection(18); f.size=2; d.accept(f,3); CHECK(d.error()); }
  { TargetDetection d; prepare(d); auto f=rejection(18); f.bytes[1]=4; d.accept(f,3); CHECK(d.error()); }
  { TargetDetection d; prepare(d); d.accept(rejection(18),3); CHECK(d.take_frame(q)); d.accept(identity(3,19),4);
    CHECK(d.take_frame(q)); d.accept(capability(q),5); CHECK(d.error() && d.backend()==B::Unknown); }
  { TargetDetection d; auto get=prepare(d,65530,0xFFFFFFF0U); d.accept(capability(get),0x10); CHECK(d.backend()==B::Direct); }
  { TargetDetection d; CHECK(d.start(0)); d.accept(identity(3,1),1); CHECK(d.take_frame(q) && q.bytes[1]==3); // Not sent yet.
    d.reset(); CHECK(!d.take_frame(q) && d.backend()==B::Unknown); }
  { TargetDetection d; prepare(d); Frame height; height.size=32; height.bytes[0]=5; put16(height,30,30);
    d.accept(height,3); d.accept(rejection(29),4); CHECK(d.error() && d.backend()==B::Unknown); }
  { TargetDetection d; prepare(d); d.accept(rejection(50),3); CHECK(d.error()); } // Beyond bounded sequence window.
  { TargetDetection d; CHECK(d.start(0));
    for(uint8_t selector : {3,4,5,6,7,9}) { CHECK(d.take_frame(q)); d.accept(identity(selector,selector),1); }
    CHECK(d.take_frame(q)); Frame f; f.size=6; f.bytes[0]=0x16; f.bytes[1]=11; put16(f,2,25); put16(f,4,10);
    d.accept(f,2); CHECK(d.error() && !d.take_frame(q)); }
  { TargetDetection d; prepare(d); d.accept(rejection(18),3); CHECK(d.take_frame(q));
    d.accept(rejection(19),4); CHECK(d.error() && !d.take_frame(q)); }
  { TargetDetection d; auto get=prepare(d); auto other=capability(get); other.bytes[9]^=1;
    other.bytes[4]=1; d.accept(other,3); CHECK(!d.error() && d.busy()); d.accept(capability(get),4); CHECK(d.backend()==B::Direct); }
  { TargetDetection d; auto get=prepare(d); auto rejected=capability(get); rejected.bytes[4]=1;
    d.accept(rejected,3); CHECK(d.error() && d.backend()==B::Unknown); CHECK(!d.take_frame(q)); }
  { TargetDetection d; CHECK(d.start(0,20)); CHECK(d.take_frame(q)); d.accept(identity(3,20),1); CHECK(d.error()); }
  for(unsigned bad=0;bad<6;bad++) {
    TargetDetection d; CHECK(d.start(0));
    for(unsigned i=0;i<=bad;i++) { const uint8_t selector=i==5?9:i+3; CHECK(d.take_frame(q)); auto f=identity(selector,i+1);
      if(i==bad) f.bytes[2]='X';
      d.accept(f,1);
    }
    CHECK(d.error() && !d.take_frame(q));
  }
  { TargetDetection d; prepare(d,65528); d.accept(rejection(31),3); CHECK(d.busy() && !d.error()); } // delta 32 across wrap.
  { TargetDetection d; prepare(d,65528); d.accept(rejection(32),3); CHECK(d.error()); }
  std::cout << "Automatic detection: identity, explicit rejection, strict DT02, timeout, replay and reset passed\n";
}
