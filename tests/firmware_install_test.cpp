#include "desk_protocol/firmware_install.h"
#include <cstdlib>
#include <iostream>
#include <vector>
#include <string>
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while(false)
using Sender=desk_protocol::FirmwareInstall;
struct Rig {
  Sender sender;
  std::vector<uint8_t> image=std::vector<uint8_t>(55296,255);
  std::vector<std::vector<uint8_t>> writes;
  uint32_t now=100; bool fail=false;
  Rig() { for(size_t i=0;i<25600;++i)image[i]=uint8_t(i*37); }
  static bool write(void *c,const uint8_t *p,size_t n) {
    auto *r=static_cast<Rig *>(c); r->writes.emplace_back(p,p+n); return !r->fail;
  }
  void begin(){CHECK(sender.begin(image.data(),image.size(),write,this,now));}
  void feed(uint8_t b){sender.feed(b,++now);}
  void boot(){for(uint8_t b:{0xfa,0x30,0,1,0x31,0xfd})feed(b);}
  void header(){begin();boot();feed(6);feed(0x43);CHECK(sender.erase_possible());feed(6);feed(0x43);}
};
int main(){
  {Rig r;r.header();for(int i=0;i<25;++i)r.feed(6);CHECK(r.sender.acked_blocks()==25);CHECK(r.writes.back()==std::vector<uint8_t>{4});r.feed(6);CHECK(r.writes.back().size()==133);r.feed(6);CHECK(r.sender.complete());r.feed(0x43);CHECK(r.sender.complete());CHECK(r.writes.size()==29);
   for(size_t i=2;i<27;++i){auto &p=r.writes[i];CHECK(p.size()==1029);CHECK(p[1]==i-1);CHECK((p[1]^p[2])==255);CHECK(std::equal(p.begin()+3,p.begin()+1027,r.image.begin()+(i-2)*1024));CHECK(desk_protocol::FirmwareExport::crc(p.data()+3,1024)==(unsigned(p[1027])<<8|p[1028]));}}
  // Every response timeout stops once, even after erase or a data ACK.
  for(unsigned stop=0;stop<31;++stop){Rig r;r.begin();r.boot();std::vector<uint8_t> replies{6,0x43,6,0x43};replies.insert(replies.end(),27,6);for(unsigned i=0;i<stop;++i)r.feed(replies[i]);const size_t before=r.writes.size();r.sender.tick(r.now+5001);CHECK(!r.sender.active());r.feed(6);r.sender.tick(r.now+10000);CHECK(r.writes.size()==before);}
  for(uint8_t bad:{0x15,0x18,0x43,0xff}){Rig r;r.begin();r.boot();r.feed(bad);CHECK(!r.sender.active());CHECK(r.sender.erase_possible());CHECK(r.writes.size()==1);}
  {Rig r;r.begin();r.sender.tick(r.now+120000);CHECK(!r.sender.active());CHECK(r.writes.empty());}
  {Rig r;r.sender.set_before_erase([](void *){return false;});r.begin();r.boot();r.feed(6);r.feed(0x43);CHECK(!r.sender.active());CHECK(!r.sender.erase_possible());CHECK(r.writes.empty());}
  {Rig r;r.begin();r.boot();r.feed(6);r.fail=true;r.feed(0x43);CHECK(r.sender.erase_possible());CHECK(!r.sender.active());CHECK(r.writes.size()==2);}
  {Rig r;r.begin();r.fail=true;r.boot();CHECK(r.sender.erase_possible());CHECK(!r.sender.active());CHECK(r.writes.size()==1);}
  {Rig r;r.image.back()=0;CHECK(!r.sender.begin(r.image.data(),r.image.size(),Rig::write,&r,r.now));CHECK(r.writes.empty());}
  {Rig r;r.now=0xfffffff0;r.begin();r.sender.tick(r.now+120000);CHECK(!r.sender.active());}
  {Rig r;r.begin();CHECK(!r.sender.begin(r.image.data(),r.image.size(),Rig::write,&r,r.now));}
  std::cout<<"Sender sequence, payload CRC, all response timeouts, NAK/CAN, tail and wrap checks passed\n";
}
