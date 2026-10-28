#include "desk_protocol/firmware_export.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>
#include <string>
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while(false)
using Receiver = desk_protocol::FirmwareExport;
struct Rig {
  Receiver receiver;
  std::vector<uint8_t> image;
  unsigned allocations{0};
  size_t requested{0};
  bool fail_allocation{false};
  std::vector<uint8_t> transmitted;
  uint32_t now{100};
  bool fail_write{false};
  Rig() { restart(); }
  void restart() { receiver.begin(allocate, write, this, now); }
  static uint8_t *allocate(void *context, size_t size) {
    auto *r = static_cast<Rig *>(context);
    ++r->allocations; r->requested = size;
    if (r->fail_allocation) return nullptr;
    r->image.assign(size, 0);
    return r->image.data();
  }
  static bool write(void *context, const uint8_t *p, size_t n) {
    auto *r = static_cast<Rig *>(context);
    if (r->fail_write) return false;
    r->transmitted.insert(r->transmitted.end(),p,p+n); return true;
  }
  void feed(const std::vector<uint8_t> &bytes) { for (auto b:bytes) receiver.feed(b,now); ++now; }
  void boot() { feed({0xFA,0x30,0,1,0x31,0xFD}); }
  void prompt() { const char p[]="Select Receive File"; feed(std::vector<uint8_t>(p,p+sizeof(p)-1)); }
  static std::vector<uint8_t> packet(uint8_t index, const std::vector<uint8_t> &payload) {
    std::vector<uint8_t> p{uint8_t(payload.size()==128 ? 1:2),index,uint8_t(index^255)};
    p.insert(p.end(),payload.begin(),payload.end());
    const uint16_t c=Receiver::crc(payload.data(),payload.size()); p.push_back(c>>8);p.push_back(c&255); return p;
  }
  static std::vector<uint8_t> metadata_text(const std::string &size) {
    std::vector<uint8_t> p(128, 0);
    const char name[] = "UploadedFlashImage.bin";
    std::copy(name, name + sizeof(name), p.begin());
    CHECK(size.size() < p.size() - sizeof(name));
    std::copy(size.begin(), size.end(), p.begin() + sizeof(name));
    return p;
  }
  static std::vector<uint8_t> metadata(bool cr=true, size_t size=55296) {
    return metadata_text(std::to_string(size) + (cr ? "\r" : ""));
  }
  static std::vector<uint8_t> block(unsigned n, size_t length=1024) {
    std::vector<uint8_t> p(length);
    for (size_t i=0; i<p.size(); ++i) p[i]=uint8_t(i*37+n);
    return p;
  }
  void begin_data(size_t size=55296) { boot(); prompt(); feed(packet(0,metadata(true,size))); }
  void data(size_t length=1024) {
    const size_t count=(receiver.expected_size()+length-1)/length;
    for (size_t i=1; i<=count; ++i) feed(packet(uint8_t(i),block(unsigned(i),length)));
  }
  void close() { feed({4});feed(packet(0,std::vector<uint8_t>(128,0)));feed({4}); }
};
int main(int argc,char **argv) {
  unsigned passed=0;
  { Rig r;r.begin_data();r.data();r.close();CHECK(r.receiver.termination_complete());CHECK(r.receiver.size()==55296);CHECK(r.transmitted.size()==68);CHECK(r.transmitted[9]=='C');for(size_t i=10;i<68;++i)CHECK(r.transmitted[i]==6);for(unsigned i=1;i<=54;++i){auto b=Rig::block(i);CHECK(std::equal(b.begin(),b.end(),r.image.begin()+(i-1)*1024));}++passed; }
  { Rig r;r.feed({0xFA,0x30,0,0,0x31,0xFD});r.receiver.tick(r.now+120000);CHECK(r.transmitted.empty());CHECK(!r.receiver.active());++passed; }
  { Rig r;r.boot();r.prompt();r.feed(Rig::packet(0,Rig::metadata(false)));CHECK(r.receiver.state()==Receiver::State::Data);++passed; }
  for(int kind=0;kind<3;++kind){Rig r;r.boot();r.prompt();auto m=Rig::metadata();if(kind==0)m[0]='X';if(kind==1)m[22]='-';if(kind==2)m[27]='X';r.feed(Rig::packet(0,m));CHECK(!r.receiver.active());CHECK(r.transmitted.size()==10);++passed;}
  { Rig r;r.begin_data();auto p=Rig::packet(1,Rig::block(1));p.back()^=1;r.feed(p);CHECK(r.transmitted.back()==0x15);r.feed(Rig::packet(1,Rig::block(1)));CHECK(r.receiver.size()==1024);++passed; }
  { Rig r;r.begin_data();auto p=Rig::packet(1,Rig::block(1));p[2]^=1;for(int i=0;i<4;++i)r.feed(p);CHECK(!r.receiver.active());CHECK(std::count(r.transmitted.begin(),r.transmitted.end(),0x15)==3);++passed; }
  { Rig r;r.begin_data();r.feed(Rig::packet(0,Rig::metadata()));r.feed(Rig::packet(1,Rig::block(1)));r.feed(Rig::packet(1,Rig::block(1)));CHECK(r.receiver.size()==1024);CHECK(r.receiver.duplicates()==2);auto bad=Rig::block(1);bad[2]^=1;r.feed(Rig::packet(1,bad));CHECK(!r.receiver.active());++passed; }
  { Rig r;r.begin_data();for(int i=0;i<9;++i)r.feed(Rig::packet(0,Rig::metadata()));CHECK(!r.receiver.active());CHECK(r.receiver.size()==0);++passed; }
  { Rig r;r.begin_data();r.feed(Rig::packet(2,Rig::block(2)));CHECK(!r.receiver.active());CHECK(r.receiver.size()==0);++passed; }
  { Rig r;r.begin_data();r.feed({4});CHECK(!r.receiver.active());CHECK(!r.receiver.data_complete());++passed; }
  { Rig r;r.begin_data();r.data();r.feed({4});r.receiver.tick(r.now+5000);CHECK(r.receiver.data_complete());CHECK(!r.receiver.termination_complete());CHECK(!r.receiver.active());++passed; }
  { Rig r;r.begin_data();r.data();for(int i=0;i<11;++i)r.feed({4});CHECK(!r.receiver.active());CHECK(r.receiver.data_complete());++passed; }
  { Rig r;r.begin_data();r.feed({2,1,254,0});r.receiver.tick(r.now+5000);CHECK(!r.receiver.active());CHECK(r.receiver.size()==0);++passed; }
  { Rig r;r.fail_write=true;r.boot();CHECK(!r.receiver.active());CHECK(r.transmitted.empty());++passed; }
  { Rig r;r.now=0xfffffff0;r.restart();r.receiver.tick(r.now+119999U);CHECK(r.receiver.active());r.receiver.tick(r.now+120000U);CHECK(!r.receiver.active());++passed; }
  { Rig r;r.begin_data();r.receiver.fail("Cancelled");const auto n=r.transmitted.size();r.data();CHECK(r.transmitted.size()==n);CHECK(r.receiver.size()==0);++passed; }
  // Header size controls allocation, completion and the exact retained length.
  for (size_t size : {size_t(1), size_t(127), size_t(128), size_t(129), size_t(1023),
                     size_t(1024), size_t(1025), size_t(65536), Receiver::MAX_IMAGE_SIZE}) {
    for (size_t length : {size_t(128), size_t(1024)}) {
      Rig r;
      CHECK(r.allocations == 0 && !r.receiver.data_complete());
      r.begin_data(size);
      CHECK(r.allocations == 1 && r.requested == size);
      CHECK(r.receiver.expected_size() == size && r.image.size() == size);
      r.data(length);
      CHECK(r.receiver.data_complete());
      CHECK(r.receiver.size() == size);
      for (size_t i=0; i<size; ++i)
        CHECK(r.image[i] == uint8_t((i % length)*37 + (i / length + 1)));
      // Repeat the last full wire packet, including bytes beyond the file end.
      const size_t blocks=(size+length-1)/length;
      r.feed(Rig::packet(uint8_t(blocks),Rig::block(unsigned(blocks),length)));
      CHECK(r.receiver.active() && r.receiver.duplicates() == 1 && r.allocations == 1);
      r.close(); CHECK(r.receiver.termination_complete());
      ++passed;
    }
  }
  // Refuse absent, malformed, zero, oversized and overflowing decimal sizes
  // before allocation or ACK. Never trust the peer's size as an allocation.
  for (const char *value : {"", "0", "0000", "-1", "+1", "1.5", "1x", " 1", "1\n",
                            "131073", "4294967296", "18446744073709551616"}) {
    Rig r; r.boot(); r.prompt();
    r.feed(Rig::packet(0,Rig::metadata_text(value)));
    CHECK(!r.receiver.active() && !r.receiver.data_complete());
    CHECK(r.allocations == 0 && r.transmitted.size() == 10);
    ++passed;
  }
  { Rig r; r.boot(); r.prompt(); auto m=Rig::metadata();
    std::fill(m.begin()+22,m.end(),'1'); r.feed(Rig::packet(0,m));
    CHECK(!r.receiver.active() && r.allocations == 0); ++passed; }
  for (const char *value : {"0001025", "1025 0 0", "1025\r 0 0"}) {
    Rig r; r.boot(); r.prompt(); r.feed(Rig::packet(0,Rig::metadata_text(value)));
    CHECK(r.receiver.expected_size() == 1025 && r.allocations == 1);
    r.data(); r.close(); CHECK(r.receiver.termination_complete()); ++passed;
  }
  { Rig r; r.boot(); r.prompt(); auto metadata=Rig::packet(0,Rig::metadata());
    metadata.back() ^= 1; r.feed(metadata);
    CHECK(r.allocations == 0 && r.receiver.state() == Receiver::State::Metadata);
    r.feed(Rig::packet(0,Rig::metadata())); r.feed(Rig::packet(0,Rig::metadata()));
    CHECK(r.allocations == 1); ++passed; }
  { Rig r; r.fail_allocation=true; r.begin_data(65536);
    CHECK(r.allocations == 1 && r.requested == 65536);
    CHECK(!r.receiver.active() && !r.receiver.data_complete());
    CHECK(r.transmitted.size() == 10 && r.receiver.size() == 0);
    CHECK(std::strstr(r.receiver.error(), "RAM")); ++passed; }
  { Rig r; r.receiver.begin(nullptr,Rig::write,&r,r.now); r.begin_data(1);
    CHECK(!r.receiver.active() && r.allocations == 0 && r.transmitted.size() == 10); ++passed; }
  { Rig r; r.begin_data(1025); r.feed(Rig::packet(1,Rig::block(1))); r.feed({4});
    CHECK(!r.receiver.active() && !r.receiver.data_complete()); ++passed; }
  { Rig r; r.begin_data(1); r.data(); auto changed=Rig::block(1); changed.back() ^= 1;
    r.feed(Rig::packet(1,changed)); CHECK(!r.receiver.active());
    CHECK(std::string(r.receiver.error()) == "Changed duplicate packet");
    CHECK(r.receiver.size() == 1 && r.image.size() == 1); ++passed; }
  { Rig r; r.begin_data(1); r.data(); r.feed(Rig::packet(2,Rig::block(2)));
    CHECK(!r.receiver.active() && r.receiver.size() == 1); ++passed; }
  { Rig r; r.begin_data(32769);
    for (unsigned i=1; i<=257; ++i) {
      r.feed(Rig::packet(uint8_t(i),Rig::block(i,128)));
      if (i == 255 || i == 256) r.feed(Rig::packet(uint8_t(i),Rig::block(i,128)));
    }
    CHECK(r.receiver.data_complete() && r.receiver.duplicates() == 2);
    r.close(); CHECK(r.receiver.termination_complete()); ++passed; }
  { Rig r; r.begin_data(1025); r.feed(Rig::packet(1,Rig::block(1,128)));
    r.feed(Rig::packet(2,Rig::block(2,1024))); r.close();
    CHECK(r.receiver.termination_complete() && r.image.size() == 1025); ++passed; }
  { Rig r; r.begin_data(1); r.data(); r.close(); r.restart();
    CHECK(!r.receiver.data_complete() && r.receiver.expected_size() == 0);
    r.begin_data(1025); r.data(); r.close();
    CHECK(r.allocations == 2 && r.receiver.termination_complete() && r.image.size() == 1025); ++passed; }
  if(argc==3){
    std::ifstream wire(argv[1],std::ios::binary),reference(argv[2],std::ios::binary);
    CHECK(wire && reference);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(wire)),{}), expected((std::istreambuf_iterator<char>(reference)),{});
    Rig r;r.feed(bytes);CHECK(r.receiver.termination_complete());CHECK(r.image==expected);CHECK(r.transmitted.size()==68);++passed;
  }
  std::printf("%u firmware export scenarios passed\n",passed);
}
