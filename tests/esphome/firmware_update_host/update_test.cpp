#include "desk_firmware_update.h"
#include "dt02_patch.h"
#include <fstream>
#include <iostream>
#include <iterator>
#define CHECK(x) do{if(!(x)){std::cerr<<__LINE__<<": " #x "\n";std::exit(1);}}while(false)
namespace fw=esphome::desk_firmware_update;
using Receiver=desk_protocol::FirmwareExport;
struct Rig:fw::FirmwareUpdate {
  esphome::desk_uart::DeskUART desk;
  Rig(){set_desk(&desk);setup();AsyncWebServerRequest req("/desk-firmware/status");req.request_headers["Host"]="desk.local";handleRequest(&req);}
  void tick(){++export_host::now;loop();}
  int post(std::string action,std::map<std::string,std::string> extra={},std::string who=std::string(32,'a'),bool foreign=false,bool csrf=true,bool stale=false){
    AsyncWebServerRequest req("/desk-firmware/action",HTTP_POST);
    req.params={{"action",action},{"owner",who},{"revision",std::to_string(revision_+(stale?1:0))}};
    req.params.insert(extra.begin(),extra.end());req.request_headers["Host"]="desk.local";
    if(csrf)req.request_headers["X-Desk-CSRF"]=csrf_;
    if(foreign)req.request_headers["Origin"]="https://untrusted.example";
    handleRequest(&req);tick();return req.response_code;
  }
  void rebind(){AsyncWebServerRequest req("/desk-firmware/status");req.request_headers={{"Host","evil.example"},{"Origin","http://evil.example"}};handleRequest(&req);CHECK(req.response_code==403);}
  void claim(){CHECK(post("claim",{{"motor_isolated","yes"}})==202);CHECK(owned_);}
  std::string state(){return phase_name_();}
  std::string hash(){return hash_;}
  void feed(const std::vector<uint8_t> &p){export_host::rx.insert(export_host::rx.end(),p.begin(),p.end());tick();}
  static std::vector<uint8_t> packet(uint8_t n,std::vector<uint8_t> data){
    std::vector<uint8_t> p{uint8_t(data.size()==128?1:2),n,uint8_t(255-n)};
    p.insert(p.end(),data.begin(),data.end());auto c=Receiver::crc(data.data(),data.size());p.push_back(c>>8);p.push_back(c&255);return p;
  }
  void export_image(const std::vector<uint8_t> &image){
    feed({0xfa,0x30,0,1,0x31,0xfd});const std::string prompt="Select Receive File";feed({prompt.begin(),prompt.end()});
    std::vector<uint8_t> header(128,0);const char name[]="UploadedFlashImage.bin";std::copy(name,name+sizeof(name),header.begin());auto size=std::to_string(image.size());std::copy(size.begin(),size.end(),header.begin()+sizeof(name));feed(packet(0,header));
    for(size_t offset=0;offset<image.size()&&receiver_.active();offset+=1024){std::vector<uint8_t> block(1024,0);std::copy(image.begin()+offset,image.begin()+std::min(offset+1024,image.size()),block.begin());feed(packet(offset/1024+1,block));}
    if(receiver_.active()){feed({4});feed(packet(0,std::vector<uint8_t>(128,0)));feed({4});}
  }
  void read(const std::vector<uint8_t> &image){CHECK(post("read")==202);export_image(image);}
  void backup(){CHECK(post("backup",{{"confirmed","yes"}})==202);CHECK(error_==nullptr);}
  void upload(const std::vector<uint8_t> &image){CHECK(post("stage")==202);CHECK(state()=="uploading");const char *hex="0123456789abcdef";for(size_t i=0;i<image.size();i+=128){std::string data;for(size_t j=i;j<std::min(i+128,image.size());++j){data+=hex[image[j]>>4];data+=hex[image[j]&15];}CHECK(post("chunk",{{"offset",std::to_string(i)},{"data",data}})==200);}CHECK(post("seal")==202);}
  void install(){CHECK(post("install",{{"confirmed","yes"}})==202);CHECK(state()=="installing");CHECK(esphome::saved_recovery!=0);}
  void complete_write(){feed({0xfa,0x30,0,1,0x31,0xfd});feed({6,0x43,6,0x43});for(int i=0;i<27;++i)feed({6});CHECK(state()=="readback_required");}
  std::vector<uint8_t> download(){AsyncWebServerRequest req("/desk-firmware/download");req.request_headers["Host"]="desk.local";handleRequest(&req);CHECK(req.response_code==200);return req.body;}
};
std::vector<uint8_t> load(const char *path){std::ifstream f(path,std::ios::binary);CHECK(f.good());std::vector<uint8_t> b{std::istreambuf_iterator<char>(f),{}};if(b.size()==65536)b.erase(b.begin(),b.begin()+0x2800);CHECK(b.size()==55296);return b;}
int main(int argc,char **argv){
 {Rig r;r.rebind();CHECK(r.post("claim",{{"motor_isolated","yes"}},std::string(32,'a'),true)==403);CHECK(r.post("claim",{{"motor_isolated","yes"}},std::string(32,'a'),false,false)==403);CHECK(r.post("claim")==409);r.claim();CHECK(r.post("read",{},std::string(32,'b'))==409);CHECK(r.post("read",{},std::string(32,'a'),false,true,true)==409);r.read(std::vector<uint8_t>(55296,255));CHECK(r.state()=="read_complete");CHECK(export_host::live_allocations()==1);r.backup();r.post("stage");CHECK(r.state()=="error");CHECK(esphome::saved_recovery==0);}
 export_host::reset();
 {Rig r;r.claim();export_host::fail_malloc=true;r.read(std::vector<uint8_t>(55296,255));CHECK(r.state()=="error");CHECK(export_host::live_allocations()==0);}
 export_host::reset();
 {Rig r;r.claim();r.post("read");CHECK(r.post("claim",{{"motor_isolated","yes"},{"takeover","yes"}},std::string(32,'b'))==409);auto tx=export_host::tx.size();r.post("cancel");r.tick();CHECK(export_host::tx.size()==tx);CHECK(r.state()=="error");CHECK(r.desk.end_count==0);r.post("exit");CHECK(r.desk.end_count==0);export_host::now+=3000;r.post("exit");CHECK(r.desk.end_count==1);}
 export_host::reset();
 if(argc==3){
  auto stock=load(argv[1]),patched=load(argv[2]);
  {Rig r;r.claim();r.read(stock);CHECK(r.hash()==fw::STOCK_SHA);r.post("exit");CHECK(r.state()=="read_complete");r.backup();r.post("patch");CHECK(r.hash()==fw::DT02_SHA);CHECK(r.download()==patched);CHECK(export_host::live_allocations()==1);
   esphome::fail_preferences=true;r.post("install",{{"confirmed","yes"}});CHECK(r.state()=="error");CHECK(esphome::saved_recovery==0);esphome::fail_preferences=false;
   r.read(stock);r.backup();r.post("patch");r.install();r.post("cancel");CHECK(esphome::saved_recovery==0);
   r.read(stock);r.backup();r.post("patch");r.install();export_host::now+=120001;r.tick();CHECK(r.state()=="error");CHECK(esphome::saved_recovery==0);
   r.read(stock);r.backup();r.post("patch");r.install();r.complete_write();CHECK(r.post("exit")==202);CHECK(r.state()=="readback_required");
   r.post("verify");r.export_image(patched);CHECK(r.state()=="verified");CHECK(esphome::saved_recovery==0);CHECK(export_host::live_allocations()==1);
   export_host::now+=3000;r.post("exit");CHECK(r.desk.end_count==1);CHECK(export_host::live_allocations()==0);
  }
  export_host::reset();
  {Rig r;r.claim();r.read(stock);r.backup();r.post("patch");r.install();r.feed({0xfa,0x30,0,1,0x31,0xfd});r.feed({6,0x43,0x15});CHECK(r.state()=="error");CHECK(esphome::saved_recovery!=0);
   // Neither a stale preflight nor a partial upload can restart a failed write.
   r.post("stage");CHECK(r.state()=="error");
  }
  export_host::reset();
  {Rig r;CHECK(r.state()=="error");CHECK(r.desk.begin_count==1);r.claim();auto unsupported=patched;unsupported[5]^=1;r.read(unsupported);r.backup();r.post("stage");CHECK(r.state()=="error");
   auto damaged=patched;std::fill(damaged.begin()+2048,damaged.begin()+3072,255);r.read(damaged);r.backup();r.upload(stock);CHECK(r.state()=="staged");r.install();r.complete_write();r.post("verify");r.export_image(damaged);CHECK(r.state()=="error");CHECK(esphome::saved_recovery!=0);
   r.read(damaged);r.backup();r.upload(stock);r.install();r.complete_write();r.post("verify");r.export_image(stock);CHECK(r.state()=="verified");CHECK(esphome::saved_recovery==0);
  }
  export_host::reset();
  {Rig r;r.claim();r.read(stock);r.backup();r.post("patch");r.install();}
  export_host::reset();
  {Rig r;CHECK(r.desk.begin_count==1);r.claim();esphome::fail_preferences=true;r.read(stock);CHECK(r.state()=="error");CHECK(esphome::saved_recovery!=0);esphome::fail_preferences=false;r.read(stock);CHECK(r.state()=="read_complete");CHECK(esphome::saved_recovery==0);}
  std::cout<<"Private stock/DT02 full flow, flash preference refusal, interrupted write, reboot recovery and mismatching readback passed\n";
 }
 std::cout<<"HTTP ownership, CSRF, unknown image, RAM refusal and cancellation passed\n";
}
