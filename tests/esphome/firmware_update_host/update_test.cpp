#include "desk_firmware_update.h"
#include "dt02_patch.h"
#include "child_lock_patch.h"
#include "partial_recovery.h"
#include "esphome/components/sha256/sha256.h"
#include <algorithm>
#include <cstring>
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
  void rebind(){AsyncWebServerRequest req("/desk-firmware/status");req.request_headers={{"Host","evil.example"},{"Origin","http://evil.example"}};handleRequest(&req);CHECK(req.response_code==409);}
  void claim(){CHECK(post("claim",{{"motor_isolated","yes"}})==200);CHECK(owned_);}
  std::string state(){return phase_name_();}
  std::string hash(){return hash_;}
  bool erase_possible(){return sender_.erase_possible();}
  std::string write_target(){return recovery_record_.write_target;}
  std::string source(){return recovery_record_.source;}
  std::string target(){return recovery_record_.target;}
  void legacy(unsigned phase){LegacyRecoveryRecord saved=recovery_record_;saved.phase=phase;auto pref=esphome::global_preferences->make_preference<LegacyRecoveryRecord>(0x71504433);CHECK(pref.save(&saved));}
  static void seed_legacy(unsigned phase){LegacyRecoveryRecord saved{};saved.magic=0x44543032;saved.phase=phase;std::memcpy(saved.source,fw::STOCK_SHA,65);std::memcpy(saved.target,fw::DT02_SHA,65);std::memcpy(saved.write_target,fw::DT02_SHA,65);auto pref=esphome::global_preferences->make_preference<LegacyRecoveryRecord>(0x71504433);CHECK(pref.save(&saved));}
  size_t legacy_size(){return sizeof(LegacyRecoveryRecord);}
  bool original_recorded(){return recovery_record_.original_blocks_valid;}
  bool recovery_required(){return recovery_;}
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
  void read(const std::vector<uint8_t> &image){CHECK(post("read")==200);export_image(image);}
  void backup(){CHECK(post("backup",{{"confirmed","yes"}})==200);CHECK(error_==nullptr);}
  void upload(const std::vector<uint8_t> &image){CHECK(post("stage")==200);CHECK(state()=="uploading");const char *hex="0123456789abcdef";for(size_t i=0;i<image.size();i+=128){std::string data;for(size_t j=i;j<std::min(i+128,image.size());++j){data+=hex[image[j]>>4];data+=hex[image[j]&15];}CHECK(post("chunk",{{"offset",std::to_string(i)},{"data",data}})==200);}CHECK(post("seal")==200);}
  void install(){CHECK(post("install",{{"confirmed","yes"}})==200);CHECK(state()=="installing");CHECK(esphome::saved_recovery!=0);}
  void complete_write(){feed({0xfa,0x30,0,1,0x31,0xfd});feed({6,0x43,6,0x43});for(size_t i=0;i<payload_size_()/1024+2;++i)feed({6});CHECK(state()=="readback_required");}
  std::vector<uint8_t> download(){AsyncWebServerRequest req("/desk-firmware/download");req.request_headers["Host"]="desk.local";handleRequest(&req);CHECK(req.response_code==200);return req.body;}
};
std::vector<uint8_t> load(const char *path){std::ifstream f(path,std::ios::binary);CHECK(f.good());std::vector<uint8_t> b{std::istreambuf_iterator<char>(f),{}};if(b.size()==65536)b.erase(b.begin(),b.begin()+0x2800);CHECK(b.size()==55296);return b;}
int main(int argc,char **argv){
 {Rig r;r.rebind();CHECK(r.post("claim",{{"motor_isolated","yes"}},std::string(32,'a'),true)==409);CHECK(r.post("claim",{{"motor_isolated","yes"}},std::string(32,'a'),false,false)==409);CHECK(r.post("claim")==409);r.claim();CHECK(r.post("read",{},std::string(32,'b'))==409);CHECK(r.post("read",{},std::string(32,'a'),false,true,true)==409);r.read(std::vector<uint8_t>(55296,255));CHECK(r.state()=="read_complete");CHECK(export_host::live_allocations()==1);r.backup();r.post("stage");CHECK(r.state()=="error");CHECK(esphome::saved_recovery==0);}
 export_host::reset();
 {Rig r;r.claim();export_host::fail_malloc=true;r.read(std::vector<uint8_t>(55296,255));CHECK(r.state()=="error");CHECK(export_host::live_allocations()==0);}
 export_host::reset();
 {Rig r;r.claim();r.post("read");CHECK(r.post("claim",{{"motor_isolated","yes"},{"takeover","yes"}},std::string(32,'b'))==409);auto tx=export_host::tx.size();r.post("cancel");r.tick();CHECK(export_host::tx.size()==tx);CHECK(r.state()=="error");CHECK(r.desk.end_count==0);r.post("exit");CHECK(r.desk.end_count==0);export_host::now+=3000;r.post("exit");CHECK(r.desk.end_count==1);}
 export_host::reset();
 // Public fixtures cover the actual prior-size NVS upgrade without vendor bytes.
 for(unsigned phase : {1U,2U}) {
  Rig::seed_legacy(phase);const auto saved=esphome::preference_data;
  esphome::corrupt_failed_load=true;
  {Rig r;CHECK(r.recovery_required());CHECK(r.state()=="error");CHECK(r.desk.begin_count==1);CHECK(!r.original_recorded());CHECK(esphome::preference_data==saved);CHECK(esphome::preference_data.size()==r.legacy_size());}
  esphome::corrupt_failed_load=false;esphome::saved_recovery=0;esphome::preference_data.clear();export_host::reset();
 }
 if(argc>=3){
  auto stock=load(argv[1]),patched=load(argv[2]);bool interrupted_used=false;
  const bool has_interrupted=argc>=4 && std::strcmp(argv[3],"-");
  // Once the loader write request is attempted, failures keep durable recovery.
  for(int failure=0;failure<3;++failure){
   Rig r;r.claim();r.read(stock);r.backup();r.post("patch");r.install();
   r.feed({0xfa,0x30,0,1,0x31,0xfd});
   if(failure==0){export_host::now+=5001;r.tick();}
   else if(failure==1)r.feed({0x15});
   else r.post("cancel");
   CHECK(r.state()=="error");CHECK(esphome::saved_recovery!=0);
   r.read(stock);CHECK(r.state()=="read_complete");CHECK(esphome::saved_recovery==0);
  }
  export_host::reset();
  {Rig r;r.claim();r.read(stock);CHECK(r.hash()==fw::STOCK_SHA);r.post("exit");CHECK(r.state()=="read_complete");r.backup();r.post("patch");CHECK(r.hash()==fw::DT02_SHA);CHECK(r.download()==patched);CHECK(export_host::live_allocations()==1);
   esphome::fail_preferences=true;r.post("install",{{"confirmed","yes"}});CHECK(r.state()=="error");CHECK(esphome::saved_recovery==0);esphome::fail_preferences=false;
   r.read(stock);r.backup();r.post("patch");r.install();r.post("cancel");CHECK(esphome::saved_recovery==0);
   r.read(stock);r.backup();r.post("patch");r.install();export_host::now+=120001;r.tick();CHECK(r.state()=="error");CHECK(esphome::saved_recovery==0);
   r.read(stock);r.backup();r.post("patch");r.install();r.complete_write();CHECK(r.post("exit")==200);CHECK(r.state()=="readback_required");
   r.post("verify");r.export_image(patched);CHECK(r.state()=="verified");CHECK(esphome::saved_recovery==0);CHECK(export_host::live_allocations()==1);
   export_host::now+=3000;r.post("exit");CHECK(r.desk.end_count==1);CHECK(export_host::live_allocations()==0);
  }
  export_host::reset();
  {Rig r;r.claim();r.read(stock);r.backup();r.post("patch");r.install();r.feed({0xfa,0x30,0,1,0x31,0xfd});r.feed({6,0x43,0x15});CHECK(r.state()=="error");CHECK(esphome::saved_recovery!=0);
   // Neither a stale preflight nor a partial upload can restart a failed write.
   r.post("stage");CHECK(r.state()=="error");
  }
  export_host::reset();
  {Rig r;CHECK(r.state()=="error");CHECK(r.desk.begin_count==1);r.claim();auto unsupported=patched;unsupported[5]^=1;r.read(unsupported);r.backup();CHECK(unsupported[1023]!=255);r.post("stage");CHECK(r.state()=="error"); // not a torn prefix: refused before upload
   auto damaged=patched;std::fill(damaged.begin()+2048,damaged.begin()+3072,255);r.read(damaged);r.backup();r.upload(stock);CHECK(r.state()=="staged");r.install();r.complete_write();r.post("verify");r.export_image(damaged);CHECK(r.state()=="error");CHECK(esphome::saved_recovery!=0);
   r.read(damaged);r.backup();r.upload(stock);r.install();r.complete_write();r.post("verify");r.export_image(stock);CHECK(r.state()=="verified");CHECK(esphome::saved_recovery==0);
  }
  export_host::reset();
  {Rig r;r.claim();r.read(stock);r.backup();r.post("patch");r.install();}
  export_host::reset();
  {Rig r;CHECK(r.desk.begin_count==1);r.claim();esphome::fail_preferences=true;r.read(stock);CHECK(r.state()=="error");CHECK(esphome::saved_recovery!=0);esphome::fail_preferences=false;r.read(stock);CHECK(r.state()=="read_complete");CHECK(esphome::saved_recovery==0);}
  // The real power-cut image and synthetic torn stock/DT02 targets use the
  // actual adapter, pinned SHA checks, persisted attempt and UART readback.
  for(int target=0;target<2;++target) for(size_t cut : {size_t(18*1024+128),size_t(0x1658),size_t(0x53b0),size_t(0x6010)}) {
   if(!target && cut==0x6010) continue; // stock tail is already erased, not a torn image
   export_host::reset();
   {Rig r;r.claim();r.read(target?stock:patched);r.backup();if(target)r.post("patch");else r.upload(stock);r.install();r.feed({0xfa,0x30,0,1,0x31,0xfd});r.post("cancel");}
   export_host::reset();
   {Rig r;r.claim();auto torn=target?patched:stock;std::fill(torn.begin()+cut,torn.end(),255);
    if(!target && cut==18*1024+128 && has_interrupted){auto saved=load(argv[3]);CHECK(saved==torn);torn=saved;interrupted_used=true;}
    r.read(torn);r.backup();r.upload(patched);CHECK(r.state()=="error"); // original-only recovery
    r.read(torn);r.backup();r.upload(stock);CHECK(r.state()=="staged");
    export_host::now+=300001;r.post("install",{{"confirmed","yes"}});CHECK(r.state()=="error");
    r.read(torn);r.backup();r.upload(stock);r.install();r.complete_write();r.post("verify");r.export_image(stock);CHECK(r.state()=="verified");CHECK(esphome::saved_recovery==0);
   }
  }
  for(int damage=0;damage<3;++damage) {
   export_host::reset();
   {Rig r;r.claim();r.read(patched);r.backup();r.upload(stock);r.install();r.feed({0xfa,0x30,0,1,0x31,0xfd});r.post("cancel");
    auto torn=stock;std::fill(torn.begin()+18*1024+128,torn.end(),255);
    if(damage==0) torn[18*1024+4]^=1; // unexpected programmed byte
    if(damage==1) torn[19*1024]=0; // second partial block
    if(damage==2) torn.back()=0; // untouched page damage
    r.read(torn);r.backup();
    if(damage==0) r.upload(stock);else r.post("stage");
    CHECK(r.state()=="error");CHECK(esphome::saved_recovery!=0);
    r.read(stock);CHECK(esphome::saved_recovery==0);
   }
  }
  // No history: even a plausible torn stock image must not become writable.
  export_host::reset();
  {Rig r;r.claim();auto torn=stock;std::fill(torn.begin()+18*1024+128,torn.end(),255);r.read(torn);r.backup();r.post("stage");CHECK(r.state()=="error");}
  // A recovery arm that times out before erase rewrites write_target to stock.
  // The torn DT02 block must still be admitted through the recorded target,
  // and a torn stock block from a later recovery write through the source.
  for(size_t cut : {size_t(18*1024+128),size_t(0x1658),size_t(0x53b0),size_t(0x6010)}) {
   export_host::reset();
   {Rig r;r.claim();r.read(stock);r.backup();r.post("patch");r.install();r.feed({0xfa,0x30,0,1,0x31,0xfd});r.post("cancel");}
   export_host::reset();
   {Rig r;r.claim();auto torn=patched;std::fill(torn.begin()+cut,torn.end(),255);
    r.read(torn);r.backup();r.upload(stock);r.install();export_host::now+=120001;r.tick();
    CHECK(r.state()=="error");CHECK(!r.erase_possible());CHECK(esphome::saved_recovery!=0);
    CHECK(r.write_target()==fw::STOCK_SHA);
    r.read(torn);r.backup();r.upload(stock);CHECK(r.state()=="staged");
    // Interrupt this recovery write too, leaving a torn stock prefix.
    r.install();r.feed({0xfa,0x30,0,1,0x31,0xfd});r.post("cancel");CHECK(esphome::saved_recovery!=0);
    auto again=stock;std::fill(again.begin()+(cut==0x6010?0x53b0:cut),again.end(),255);
    r.read(again);r.backup();r.upload(patched);CHECK(r.state()=="error");
    r.read(again);r.backup();r.upload(stock);r.install();r.complete_write();r.post("verify");r.export_image(stock);CHECK(r.state()=="verified");CHECK(esphome::saved_recovery==0);
   }
  }
  // DT02->DT02 reinstall: arming stock recovery records stock in the redundant
  // source slot, keeping DT02 evidence in the target slot across a reboot.
  for(size_t cut : {size_t(18*1024+128),size_t(0x1658),size_t(0x53b0),size_t(0x6010)}) {
   export_host::reset();
   {Rig r;r.claim();r.read(patched);r.backup();r.upload(patched);r.install();r.feed({0xfa,0x30,0,1,0x31,0xfd});r.post("cancel");
    CHECK(r.source()==fw::DT02_SHA);CHECK(r.target()==fw::DT02_SHA);}
   export_host::reset();
   auto torn=patched;std::fill(torn.begin()+cut,torn.end(),255);
   auto again=stock;std::fill(again.begin()+(cut==0x6010?0x53b0:cut),again.end(),255);
   {Rig r;r.claim();r.read(torn);r.backup();r.upload(stock);r.install();export_host::now+=120001;r.tick();
    CHECK(r.state()=="error");CHECK(!r.erase_possible());CHECK(esphome::saved_recovery!=0);
    CHECK(r.source()==fw::STOCK_SHA);CHECK(r.target()==fw::DT02_SHA);CHECK(r.write_target()==fw::STOCK_SHA);
    r.read(torn);r.backup();r.upload(stock);CHECK(r.state()=="staged");
    r.install();r.feed({0xfa,0x30,0,1,0x31,0xfd});r.post("cancel");CHECK(esphome::saved_recovery!=0);}
   export_host::reset();
   {Rig r;CHECK(r.state()=="error");r.claim();
    r.read(again);r.backup();r.upload(patched);CHECK(r.state()=="error");
    r.read(again);r.backup();r.upload(stock);r.install();r.complete_write();r.post("verify");r.export_image(stock);CHECK(r.state()=="verified");CHECK(esphome::saved_recovery==0);}
  }
  // The moved identity still clears on a clean read of the untouched original.
  export_host::reset();
  {Rig r;r.claim();r.read(patched);r.backup();r.upload(patched);r.install();r.feed({0xfa,0x30,0,1,0x31,0xfd});r.post("cancel");
   auto torn=patched;std::fill(torn.begin()+0x1658,torn.end(),255);
   r.read(torn);r.backup();r.upload(stock);r.install();export_host::now+=120001;r.tick();CHECK(r.source()==fw::STOCK_SHA);
   r.read(patched);CHECK(r.state()=="read_complete");CHECK(esphome::saved_recovery==0);}
  // Halfword boundary: an odd programmed length is accepted only when the
  // next expected byte is erased; a missing programmed byte is refused.
  size_t missing=0,erased_next=0;
  for(size_t k=0;k+2<fw::PAYLOAD_SIZE;k+=2){
   const size_t end=(k/1024+1)*1024;
   if(!missing && k%1024 && k+2<end && patched[k]!=255 && patched[k+1]!=255 && stock[k+1]!=255) missing=k;
   if(!erased_next && k%1024 && patched[k]!=255 && patched[k+1]==255 && std::any_of(patched.begin()+k+2,patched.begin()+end,[](uint8_t b){return b!=255;})) erased_next=k;
  }
  CHECK(missing);
  for(size_t k : {missing,erased_next}) {
   if(!k) continue;
   export_host::reset();
   {Rig r;r.claim();r.read(stock);r.backup();r.post("patch");r.install();r.feed({0xfa,0x30,0,1,0x31,0xfd});r.post("cancel");
    auto torn=patched;std::fill(torn.begin()+k+1,torn.end(),255);
    CHECK(fw::partial_prefix_length(torn.data()+k/1024*1024)==k%1024+2);
    r.read(torn);r.backup();r.upload(stock);CHECK(r.state()==(k==missing?"error":"staged"));
    r.read(stock);CHECK(esphome::saved_recovery==0);
   }
  }
  // A programmed prefix outside the 25-block payload is refused before upload.
  export_host::reset();
  {Rig r;r.claim();r.read(stock);r.backup();r.post("patch");r.install();r.feed({0xfa,0x30,0,1,0x31,0xfd});r.post("cancel");
   auto torn=stock;CHECK(std::all_of(torn.begin()+30*1024,torn.begin()+31*1024,[](uint8_t b){return b==255;}));torn[30*1024]=0;
   r.read(torn);r.backup();r.post("stage");CHECK(r.state()=="error");CHECK(esphome::saved_recovery!=0);
   r.read(stock);CHECK(esphome::saved_recovery==0);
  }
  for(size_t i=0;i<stock.size();++i) CHECK(fw::dt02_byte(i,stock[i])==patched[i]);
  std::cout<<"Torn stock/DT02 blocks, reboot, original-only recovery, stale preflight and malformed images passed\n";
  std::cout<<"Recovery arm timeout before erase, interrupted recovery write, DT02 reinstall recovery after reboot and halfword/payload boundaries passed"<<(erased_next?"":" (no odd-length erased-next case in image)")<<"\n";
  if(has_interrupted){
   CHECK(interrupted_used);auto saved=load(argv[3]);char hex[65];
   esphome::sha256::SHA256 sha;sha.init();sha.add(saved.data(),saved.size());sha.calculate();sha.get_hex(hex);
   std::cout<<"Saved power-cut image "<<hex<<" recovery passed\n";
  }
  std::cout<<"Private stock/DT02 full flow, flash preference refusal, interrupted write, reboot recovery and mismatching readback passed\n";
  // An upgrade must retain the exact previous-size marker without writing it
  // during setup. Both historical phases retain UART ownership until readback.
  for(unsigned phase : {1U,2U}) {
   export_host::reset();
   {Rig r;r.claim();r.read(stock);r.backup();r.post("patch");r.install();r.legacy(phase);CHECK(esphome::preference_data.size()==r.legacy_size());}
   export_host::reset();
   {Rig r;CHECK(r.recovery_required());CHECK(r.state()=="error");CHECK(r.desk.begin_count==1);CHECK(!r.original_recorded());CHECK(esphome::preference_data.size()==r.legacy_size());
    r.claim();r.read(stock);CHECK(r.state()=="read_complete");CHECK(!r.recovery_required());}
  }
  if(argc>=5) {
   const auto child=load(argv[4]);
   for(size_t i=0;i<stock.size();++i) CHECK(fw::cl01_byte(i,stock[i])==child[i]);
   for(const auto &source : {stock,patched}) {
    export_host::reset();
    {Rig r;r.claim();r.read(source);r.post("patch_child_lock");CHECK(r.state()=="error");
     r.read(source);r.backup();r.post("patch_child_lock");CHECK(r.state()=="staged");CHECK(r.hash()==fw::CL01_SHA);CHECK(r.download()==child);
     r.install();r.complete_write();r.post("verify");r.export_image(child);CHECK(r.state()=="verified");CHECK(!r.recovery_required());}
   }
   const std::vector<std::vector<uint8_t>> images{stock,patched,child};
   // Every admitted full-image transition, including CL01 removal and same
   // image reinstalls, uses the retained exact image for separate readback.
   for(const auto &source : images) for(const auto &target : images) {
    export_host::reset();
    {Rig r;r.claim();r.read(source);r.backup();r.upload(target);CHECK(r.state()=="staged");r.install();r.complete_write();r.post("verify");r.export_image(target);CHECK(r.state()=="verified");CHECK(!r.recovery_required());}
   }
   // A recognized but unrecorded third image must not rebase unresolved history.
   export_host::reset();
   {Rig r;r.claim();r.read(patched);r.backup();r.upload(patched);r.install();r.feed({0xfa,0x30,0,1,0x31,0xfd});r.post("cancel");r.read(child);r.backup();const auto saved=esphome::preference_data;
    for(const char *action : {"patch","patch_child_lock","stage"}){r.post(action);CHECK(r.state()=="error");CHECK(r.recovery_required());CHECK(esphome::preference_data==saved);}
    r.read(patched);CHECK(!r.recovery_required());}
   // A CL01 mismatch keeps the marker, and only a clean independent read clears it.
   export_host::reset();
   {Rig r;r.claim();r.read(patched);r.backup();r.post("patch_child_lock");r.install();r.complete_write();r.post("verify");auto bad=child;bad[0]^=1;r.export_image(bad);CHECK(r.state()=="error");CHECK(r.recovery_required());r.read(child);CHECK(!r.recovery_required());}
   for(const auto &source : images) for(const auto &target : images) {
    for(size_t cut : {size_t(0x1b8a),size_t(0x53b0),size_t(0x5dfa),size_t(0x6010),size_t(0x6402),size_t(0x6600)}) {
     if(!std::any_of(target.begin()+cut,target.end(),[](uint8_t b){return b!=255;})) continue;
     export_host::reset();
     {Rig r;r.claim();r.read(source);r.backup();r.upload(target);r.install();r.feed({0xfa,0x30,0,1,0x31,0xfd});r.post("cancel");}
     auto torn=target;std::fill(torn.begin()+cut,torn.end(),255);
     export_host::reset();
     {Rig r;CHECK(r.recovery_required());r.claim();r.read(torn);CHECK(r.recovery_required());r.backup();
      r.upload(child);CHECK(r.state()=="error"); // damaged recovery remains original-only
      r.read(torn);r.backup();r.upload(stock);CHECK(r.state()=="staged");r.install();r.feed({0xfa,0x30,0,1,0x31,0xfd});r.post("cancel");CHECK(r.original_recorded());}
     // Interrupt the stock recovery too. Its completed hook blocks can differ
     // from both DT02 and CL01, so the third fingerprint slot must survive reboot.
     export_host::reset();
     {Rig r;CHECK(r.original_recorded());r.claim();auto again=stock;std::fill(again.begin()+0x5dfa,again.end(),255);
      r.read(again);r.backup();r.upload(stock);CHECK(r.state()=="staged");r.install();r.complete_write();r.post("verify");r.export_image(stock);CHECK(r.state()=="verified");CHECK(!r.recovery_required());}
    }
   }
   // Twenty-sixth-block prefixes require a CL01 identity in the saved attempt.
   export_host::reset();
   {Rig r;r.claim();r.read(stock);r.backup();r.upload(patched);r.install();r.feed({0xfa,0x30,0,1,0x31,0xfd});r.post("cancel");auto bad=patched;bad[25*1024]=0;r.read(bad);r.backup();r.post("stage");CHECK(r.state()=="error");r.read(stock);}
   // A partial halfword, second torn block, or damaged untouched tail still fails.
   for(int damage=0;damage<3;++damage) {
    export_host::reset();
    {Rig r;r.claim();r.read(patched);r.backup();r.upload(child);r.install();r.feed({0xfa,0x30,0,1,0x31,0xfd});r.post("cancel");auto bad=child;std::fill(bad.begin()+0x6600,bad.end(),255);
     if(damage==0)std::fill(bad.begin()+0x6401,bad.end(),255);
     if(damage==1)std::fill(bad.begin()+0x6100,bad.begin()+0x6400,255);
     if(damage==2)bad.back()=0;
     r.read(bad);r.backup();if(damage==0)r.upload(stock);else r.post("stage");if(r.state()!="error")std::cerr<<"CL01 damage case "<<damage<<" state "<<r.state()<<"\n";CHECK(r.state()=="error");CHECK(r.recovery_required());r.read(patched);}
   }
   std::cout<<"CL01 exact generated bytes, nine full-image transitions, legacy marker upgrade, torn blocks and repeated three-identity original recovery passed\n";
  }
 }
 std::cout<<"HTTP ownership, CSRF, unknown image, RAM refusal and cancellation passed\n";
}
