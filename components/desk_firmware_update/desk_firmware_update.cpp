#include "desk_firmware_update.h"
#include "dt02_patch.h"
#include "child_lock_patch.h"
#include "partial_recovery.h"
#include "ui.h"
#include <cstdio>
#include <cstring>
#include <esp_random.h>
#include <esp_heap_caps.h>
#include "esphome/components/sha256/sha256.h"
#include "esphome/core/hal.h"
#include "esphome/core/application.h"
#include "esphome/components/network/util.h"

namespace esphome::desk_firmware_update {
namespace {
constexpr uint32_t RECOVERY_TAG=0x44543032;
bool known(const char *hash) { return !std::strcmp(hash,STOCK_SHA) || !std::strcmp(hash,DT02_SHA) || !std::strcmp(hash,CL01_SHA); }
bool hex_string(const std::string &s,size_t length) {
  if (s.size()!=length) return false;
  for (char c:s) if (!((c>='0'&&c<='9')||(c>='a'&&c<='f'))) return false;
  return true;
}
unsigned hex_digit(char c) { return c<='9' ? c-'0' : c-'a'+10; }
}
void FirmwareUpdate::setup() {
  recovery_preference_=global_preferences->make_preference<RecoveryRecord>(0x71504433);
  bool loaded=recovery_preference_.load(&recovery_record_);
  if (!loaded) {
    recovery_record_={};
    auto previous=global_preferences->make_preference<LegacyRecoveryRecord>(0x71504433);
    loaded=previous.load(static_cast<LegacyRecoveryRecord *>(&recovery_record_));
  }
  recovery_=loaded && recovery_record_.magic==RECOVERY_TAG &&
      (recovery_record_.phase==1 || recovery_record_.phase==2) &&
      recovery_record_.source[64]==0 && recovery_record_.target[64]==0 && recovery_record_.write_target[64]==0 &&
      known(recovery_record_.source) && known(recovery_record_.target) && known(recovery_record_.write_target) &&
      recovery_record_.original_blocks_valid<=1;
  if (!recovery_) recovery_record_={};
  if (recovery_) { owned_=desk_->begin_firmware_export(true); phase_=Phase::Error; error_="Previous installation requires readback or original-image recovery"; }
  FirmwareExport::setup();
  if (is_failed()) return;
  auto *uart=static_cast<uart::IDFUARTComponent *>(desk_->firmware_export_uart());
  if (uart->get_rx_buffer_size()<4096) { mark_failed(); return; }
}
bool FirmwareUpdate::save_recovery_(bool value) {
  if (value) {
    const bool damaged=recovery_ && recovery_record_.phase==2;
    recovery_record_.magic=RECOVERY_TAG;
    // Persist before Boot, outside all time-critical loader handshakes.
    recovery_record_.phase=2;
    recovery_record_.attempt=esp_random();
    if (!damaged) {
      recovery_record_.original_blocks_valid=0;
      std::memcpy(recovery_record_.source,source_hash_,65);
      std::memcpy(recovery_record_.source_blocks,preflight_blocks_,sizeof(preflight_blocks_));
      std::memcpy(recovery_record_.target,hash_,65);
      block_hashes_(recovery_record_.target_blocks);
    } else if (!std::strcmp(recovery_record_.source,recovery_record_.target) && std::strcmp(recovery_record_.source,hash_)) {
      // Same-image reinstall: both slots hold one identity. Record this
      // admitted write in the redundant source slot so a torn block from it
      // remains bound to its own block hash; the target slot keeps the original.
      std::memcpy(recovery_record_.source,hash_,65);
      block_hashes_(recovery_record_.source_blocks);
    }
    // A DT02/CL01 attempt can contain both patched identities. Preserve both
    // when an original-only recovery is itself interrupted, including reboot.
    if (damaged && !std::strcmp(hash_,STOCK_SHA)) {
      block_hashes_(recovery_record_.original_blocks);
      recovery_record_.original_blocks_valid=1;
    }
    std::memcpy(recovery_record_.write_target,hash_,65);
  } else {
    // Keep the in-memory recovery evidence if a clear cannot be persisted.
    std::unique_ptr<RecoveryRecord> cleared(new(std::nothrow) RecoveryRecord{});
    if (!cleared || !recovery_preference_.save(cleared.get()) || !global_preferences->sync()) return false;
    recovery_record_={}; recovery_=false; return true;
  }
  if (!recovery_preference_.save(&recovery_record_) || !global_preferences->sync()) return false;
  recovery_=true; return true;
}
void FirmwareUpdate::block_hashes_(uint8_t output[54][32]) {
  for (size_t block=0;block<54;++block) {
    sha256::SHA256 hash; hash.init(); hash.add(buffer_.get()+block*1024,1024);
    hash.calculate(); hash.get_bytes(output[block]);
  }
}
bool FirmwareUpdate::recovery_source_matches_() {
  partial_block_=-1;
  if (!recovery_ || recovery_record_.phase!=2 || size_!=IMAGE_SIZE) return false;
  for (size_t block=0;block<54;++block) {
    if (!std::memcmp(preflight_blocks_[block],recovery_record_.source_blocks[block],32) ||
        !std::memcmp(preflight_blocks_[block],recovery_record_.target_blocks[block],32) ||
        (recovery_record_.original_blocks_valid &&
         !std::memcmp(preflight_blocks_[block],recovery_record_.original_blocks[block],32))) continue;
    bool erased=true;
    for (size_t i=0;i<1024;++i) if (buffer_.get()[block*1024+i]!=255) { erased=false; break; }
    if (erased) continue;
    // Retain at most one torn programmed block. Its contents are NOT admitted
    // until the user supplies the exact stock image at seal/install time.
    if (block>=recovery_payload_size_()/1024 || partial_block_>=0 || !partial_prefix_length(buffer_.get()+block*1024)) {
      partial_block_=-1; return false;
    }
    partial_block_=int(block);
    std::memcpy(partial_image_,buffer_.get()+block*1024,1024);
  }
  return partial_block_<0;
}
bool FirmwareUpdate::validate_partial_recovery_() const {
  if (!recovery_ || recovery_record_.phase!=2 || partial_block_<0 ||
      partial_block_>=int(recovery_payload_size_()/1024) || std::strcmp(hash_,STOCK_SHA) || !buffer_) return false;
  const size_t base=size_t(partial_block_)*1024;
  // A failed recovery attempt rewrites write_target before erase, so derive
  // all pinned candidates from the uploaded original. Each is admitted only
  // through the block hash recorded for the same persistent identity.
  for (const char *identity : {STOCK_SHA,DT02_SHA,CL01_SHA}) {
    const uint8_t *recorded=!std::strcmp(recovery_record_.source,identity) ? recovery_record_.source_blocks[partial_block_]
        : !std::strcmp(recovery_record_.target,identity) ? recovery_record_.target_blocks[partial_block_]
        : !std::strcmp(identity,STOCK_SHA) && recovery_record_.original_blocks_valid ? recovery_record_.original_blocks[partial_block_] : nullptr;
    if (!recorded) continue;
    uint8_t expected[1024], digest[32];
    for (size_t i=0;i<1024;++i) {
      const uint8_t stock=buffer_.get()[base+i];
      expected[i]=!std::strcmp(identity,CL01_SHA) ? cl01_byte(base+i,stock)
          : !std::strcmp(identity,DT02_SHA) ? dt02_byte(base+i,stock) : stock;
    }
    sha256::SHA256 sha; sha.init(); sha.add(expected,sizeof(expected)); sha.calculate(); sha.get_bytes(digest);
    if (!std::memcmp(digest,recorded,32) && partial_prefix_matches(partial_image_,expected)) return true;
  }
  return false;
}
size_t FirmwareUpdate::payload_size_() const {
  return !std::strcmp(hash_,CL01_SHA) ? 26624 : PAYLOAD_SIZE;
}
size_t FirmwareUpdate::recovery_payload_size_() const {
  return !std::strcmp(recovery_record_.source,CL01_SHA) || !std::strcmp(recovery_record_.target,CL01_SHA) ? 26624 : PAYLOAD_SIZE;
}
bool FirmwareUpdate::active_() const {
  return phase_==Phase::Reading || phase_==Phase::Writing || phase_==Phase::Verifying;
}
const char *FirmwareUpdate::phase_name_() const {
  switch (phase_) {
    case Phase::Idle:return "idle"; case Phase::Reading:return "reading";
    case Phase::Ready:return "read_complete"; case Phase::Uploading:return "uploading";
    case Phase::Staged:return "staged"; case Phase::Writing:return "installing";
    case Phase::AwaitVerify:return "readback_required"; case Phase::Verifying:return "verifying";
    case Phase::Verified:return "verified"; default:return "error";
  }
}
std::string FirmwareUpdate::maintenance_status() {
  std::lock_guard<std::mutex> lock(mutex_); return phase_name_();
}
void FirmwareUpdate::fail_(const char *reason) {
  error_=reason; finished_at_=millis(); phase_=Phase::Error; fast_loop_.stop();
  // UART ownership remains with maintenance, including failures and browser loss.
}
void FirmwareUpdate::digest_() {
  sha256::SHA256 digest; digest.init(); digest.add(buffer_.get(),size_);
  digest.calculate(); digest.get_hex(hash_);
}
uint8_t *FirmwareUpdate::allocate_image_(void *context,size_t size) {
  auto *self=static_cast<FirmwareUpdate *>(context);
  if (self->phase_==Phase::Verifying)
    return size==self->size_ ? self->buffer_.get() : nullptr;
  const auto free=heap_caps_get_free_size(MALLOC_CAP_8BIT);
  if (!size || size>Receiver::MAX_IMAGE_SIZE || free<size+MIN_FREE_HEAP ||
      heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)<size) return nullptr;
  uint8_t *raw=static_cast<uint8_t *>(heap_caps_malloc(size,MALLOC_CAP_8BIT));
  if (!raw) return nullptr;
  self->buffer_=std::shared_ptr<uint8_t>(raw,heap_caps_free);
  if (heap_caps_get_free_size(MALLOC_CAP_8BIT)<MIN_FREE_HEAP) { self->buffer_.reset(); return nullptr; }
  self->size_=size; return raw;
}
void FirmwareUpdate::read_(bool verify) {
  if (!owned_) { fail_("Claim maintenance with the motor disconnected first"); return; }
  if (!verify) {
    buffer_.reset(); size_=0; hash_[0]=0; preflight_=false; backup_=false; partial_block_=-1;
  }
  phase_=verify ? Phase::Verifying : Phase::Reading; error_=nullptr;
  receiver_.begin(allocate_image_,transmit_,this,millis());
  if (verify) receiver_.compare_only();
  error_baseline_=errors_();
  if (uart_flush_input(port_)!=ESP_OK) receiver_.fail("UART flush failed");
  fast_loop_.start();
}
bool FirmwareUpdate::admit_() const {
  if (!preflight_ || !tail_erased_ || uint32_t(millis()-preflight_at_)>300000 ||
      size_!=IMAGE_SIZE || !known(hash_) || !backup_) return false;
  // Recovery after a failed attempt is deliberately limited to the original.
  return known(source_hash_) || ((recovery_source_valid_ || validate_partial_recovery_()) && !std::strcmp(hash_,STOCK_SHA));
}
void FirmwareUpdate::execute_(const std::string &action) {
  error_=nullptr;
  if (action=="claim") {
    if (!owned_) owned_=desk_->begin_firmware_export(true);
    error_=nullptr;
    if (!owned_) fail_("Cannot acquire UART ownership");
    return;
  }
  if (action=="read") { read_(false); return; }
  if (action=="backup") {
    if (phase_!=Phase::Ready || !buffer_) { fail_("Read and save the original backup first"); return; }
    backup_=true; return;
  }
  if ((action=="patch" || action=="patch_child_lock" || action=="stage") && buffer_ && buffer_.use_count()>1) {
    error_="Wait for the image download to finish"; return;
  }
  if ((action=="patch" || action=="patch_child_lock" || action=="stage") && recovery_ && known(source_hash_)) {
    fail_("Read image is not recorded for this recovery; read a recorded complete image or use external recovery"); return;
  }
  if (action=="patch") {
    if (phase_!=Phase::Ready || !backup_ || std::strcmp(hash_,STOCK_SHA) ||
        !apply_dt02(buffer_.get(),size_)) { fail_("Patch requires the exact supported original and a saved backup"); return; }
    digest_();
    if (std::strcmp(hash_,DT02_SHA)) { fail_("Generated patch hash mismatch"); return; }
    phase_=Phase::Staged; return;
  }
  if (action=="patch_child_lock") {
    if (phase_!=Phase::Ready || !backup_ || !preflight_ || !tail_erased_ ||
        uint32_t(millis()-preflight_at_)>300000 ||
        (std::strcmp(hash_,STOCK_SHA) && std::strcmp(hash_,DT02_SHA))) {
      fail_("CL01 requires the exact supported original or DT02 and a saved backup"); return;
    }
    if ((!std::strcmp(hash_,STOCK_SHA) && !apply_dt02(buffer_.get(),size_)) ||
        !apply_child_lock(buffer_.get(),size_)) { fail_("CL01 patch precondition failed"); return; }
    digest_();
    if (std::strcmp(hash_,CL01_SHA)) { fail_("Generated CL01 hash mismatch"); return; }
    phase_=Phase::Staged; return;
  }
  if (action=="stage") {
    if (!preflight_ || !tail_erased_ || !backup_ || (!known(source_hash_) && !recovery_source_valid_ && partial_block_<0)) {
      fail_("Read this controller and confirm the backup before selecting an image"); return;
    }
    // Admission guarantees a full-sized buffer; reuse it after the saved backup.
    if (!buffer_ || size_!=IMAGE_SIZE) { fail_("Read a supported application first"); return; }
    hash_[0]=0;
    upload_offset_=0; phase_=Phase::Uploading; error_=nullptr; return;
  }
  if (action=="seal") {
    if (phase_!=Phase::Uploading || upload_offset_!=IMAGE_SIZE) { fail_("Incomplete image upload"); return; }
    digest_();
    if (!admit_()) { fail_("Image not admitted, or preflight expired; read again"); return; }
    phase_=Phase::Staged; return;
  }
  if (action=="install") {
    if (phase_!=Phase::Staged) { fail_("Stage and validate an image before installation"); return; }
    digest_();
    if (!admit_()) { fail_("Installation refused: unsupported image or stale preflight"); return; }
    // Commit a recovery marker before selecting the loader, not after its ACK.
    attempt_had_recovery_=recovery_ && recovery_record_.phase==2;
    if (!save_recovery_(true)) { fail_("Cannot save recovery marker; no write started"); return; }
    error_baseline_=errors_();
    const auto profile=!std::strcmp(hash_,CL01_SHA) ? desk_protocol::FirmwareInstall::Profile::ChildLockCL01
        : desk_protocol::FirmwareInstall::Profile::OriginalDT02;
    if (uart_flush_input(port_)!=ESP_OK || !sender_.begin(buffer_.get(),size_,transmit_,this,millis(),profile)) {
      fail_("Cannot arm installation; recovery marker retained"); return;
    }
    preflight_=false; phase_=Phase::Writing; error_=nullptr; fast_loop_.start(); return;
  }
  if (action=="verify") {
    if ((phase_!=Phase::AwaitVerify && phase_!=Phase::Staged) || !known(hash_) || !recovery_) {
      fail_("No retained installation image to verify"); return;
    }
    read_(true); return;
  }
  if (action=="cancel") {
    if (phase_==Phase::Writing) {
      sender_.fail("Cancelled; no retry sent");
      if (!sender_.erase_possible() && !attempt_had_recovery_ && !save_recovery_(false)) {
        fail_("Cannot clear recovery record; read and verify before exiting"); return;
      }
    }
    if (phase_==Phase::Reading || phase_==Phase::Verifying) receiver_.fail("Cancelled");
    fail_("Cancelled; controller may remain in loader; read again before recovery"); return;
  }
  if (action=="exit") {
    if (active_() || recovery_ || uint32_t(millis()-finished_at_)<3000) { error_="Cannot exit with an active or unverified installation"; return; }
    buffer_.reset(); size_=0; hash_[0]=0; preflight_=false; backup_=false; partial_block_=-1;
    uart_flush_input(port_); desk_->end_firmware_export(); owned_=false;
    owner_.clear(); phase_=Phase::Idle; error_=nullptr; return;
  }
  fail_("Unknown maintenance action");
}
void FirmwareUpdate::loop() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (is_failed()) return;
  // After a Stamp reboot, do not allow application queries into an unknown loader.
  if (recovery_ && !owned_) owned_=desk_->begin_firmware_export(true);
  if (!pending_.empty()) { auto action=std::move(pending_); pending_.clear(); execute_(action); }
  if (!active_()) return;
  const bool writing=phase_==Phase::Writing;
  if (writing) {
    if (!sender_.erase_possible() && uint32_t(millis()-preflight_at_)>300000)
      sender_.fail("Preflight expired before erase; read again");
    sender_.tick(millis());
  } else receiver_.tick(millis());
  size_t available=0;
  if (errors_()!=error_baseline_ || uart_get_buffered_data_len(port_,&available)!=ESP_OK || available>=3584) {
    if (writing) sender_.fail("UART error or receive backlog"); else receiver_.fail("UART error or receive backlog");
  }
  for (size_t budget=0;budget<2048 && (writing ? sender_.active() : receiver_.active());++budget) {
    uint8_t byte; const int count=uart_read_bytes(port_,&byte,1,0);
    if (!count) break;
    if (count!=1) { if (writing) sender_.fail("UART read failed"); else receiver_.fail("UART read failed"); break; }
    if (writing) sender_.feed(byte,millis()); else receiver_.feed(byte,millis());
  }
  if (writing) {
    if (sender_.active()) return;
    fast_loop_.stop(); ++revision_;
    if (sender_.complete()) phase_=Phase::AwaitVerify;
    else {
      if (!sender_.erase_possible() && !attempt_had_recovery_ && !save_recovery_(false)) {
        fail_("Cannot clear recovery record; read and verify before exiting"); return;
      }
      fail_(sender_.error());
    }
    return;
  }
  if (receiver_.active()) return;
  fast_loop_.stop(); finished_at_=millis(); ++revision_;
  if (!receiver_.termination_complete() || receiver_.bad_packets() || receiver_.duplicates()) {
    fail_(receiver_.error() ? receiver_.error() : "Read was not clean; repeat the full read"); return;
  }
  if (phase_==Phase::Verifying) {
    if (!save_recovery_(false)) { fail_("Readback matched but recovery marker could not be cleared"); return; }
    phase_=Phase::Verified; return;
  }
  digest_(); std::memcpy(source_hash_,hash_,sizeof(hash_));
  tail_erased_=size_==IMAGE_SIZE;
  if (tail_erased_) for (size_t i=IMAGE_SIZE-1024;i<IMAGE_SIZE;++i) if (buffer_.get()[i]!=255) tail_erased_=false;
  recovery_source_valid_=false;
  if (size_==IMAGE_SIZE) { block_hashes_(preflight_blocks_); recovery_source_valid_=recovery_source_matches_(); }
  // A clean full read can also prove that an interrupted attempt left the
  // original intact, or that its complete intended target is already present.
  // The target slot is checked too: a same-image reinstall may have moved the
  // original identity there.
  if (recovery_ && (!std::strcmp(hash_,recovery_record_.source) || !std::strcmp(hash_,recovery_record_.target) ||
                    !std::strcmp(hash_,recovery_record_.write_target))) {
    if (!save_recovery_(false)) { fail_("Read matched but recovery record could not be cleared"); return; }
  }
  preflight_=true; preflight_at_=millis(); phase_=Phase::Ready;
}
bool FirmwareUpdate::canHandle(AsyncWebServerRequest *request) const {
  char path[AsyncWebServerRequest::URL_BUF_SIZE]; const auto url=request->url_to(path);
  return url=="/desk-firmware" || url=="/desk-firmware/status" ||
         url=="/desk-firmware/download" || url=="/desk-firmware/action";
}
void FirmwareUpdate::handleRequest(AsyncWebServerRequest *request) {
  if (is_failed()) { request->send(500,"text/plain","Maintenance UART unavailable; requires 115200 baud and 4096-byte RX buffer"); return; }
  const auto origin=request->get_header("Origin"), host=request->get_header("Host");
  std::string authority=host ? *host : "";
  const auto port=web_server_base::global_web_server_base->get_port();
  const std::string suffix=":"+std::to_string(port);
  if (authority.size()>suffix.size() && authority.compare(authority.size()-suffix.size(),suffix.size(),suffix)==0)
    authority.resize(authority.size()-suffix.size());
  bool local_host=authority==std::string(App.get_name().c_str())+".local";
  for (const auto &address:network::get_ip_addresses()) {
    if (!address.is_set()) continue;
    char ip[64]; address.str_to(ip);
    if (authority==ip || authority=="["+std::string(ip)+"]") local_host=true;
  }
  if (!local_host) { request->send(409,"text/plain","Use the device IP address or its .local name"); return; }
  // ESPHome adds permissive CORS headers. Reject foreign origins here before
  // exposing the CSRF token or accepting authenticated maintenance actions.
  if (origin && (!host || (*origin!="http://"+*host && *origin!="https://"+*host))) {
    request->send(409,"text/plain","Same-origin access required"); return;
  }
  char path[AsyncWebServerRequest::URL_BUF_SIZE]; const auto url=request->url_to(path);
  std::unique_lock<std::mutex> lock(mutex_);
  if (csrf_.empty()) {
    char token[33];
    std::snprintf(token,sizeof(token),"%08lx%08lx%08lx%08lx",(unsigned long)esp_random(),
        (unsigned long)esp_random(),(unsigned long)esp_random(),(unsigned long)esp_random());
    csrf_=token;
  }
  // ESPHome IDF maps unsupported response codes (including 202) to 500.
  // Use 200 for accepted requests and 409 for rejected maintenance actions.
  auto send=[&](int code,const char *type,const char *body) {
    lock.unlock();
    auto *response=request->beginResponse(code,type,std::string(body));
    response->addHeader("Cache-Control","no-store");
    response->addHeader("X-Frame-Options","DENY");
    response->addHeader("Content-Security-Policy","frame-ancestors 'none'");
    response->addHeader("X-Content-Type-Options","nosniff"); request->send(response);
  };
  if (request->method()==HTTP_GET && url=="/desk-firmware") {
    lock.unlock();
    auto *response=request->beginResponse(200,"text/html",reinterpret_cast<const uint8_t *>(FIRMWARE_UI),sizeof(FIRMWARE_UI)-1);
    response->addHeader("X-Frame-Options","DENY");
    response->addHeader("Content-Security-Policy","frame-ancestors 'none'");
    response->addHeader("Cache-Control","no-store"); request->send(response); return;
  }
  if (request->method()==HTTP_GET && url=="/desk-firmware/status") {
    std::string json(1800,0);
    const int length=std::snprintf(&json[0],json.size(),
      "{\"state\":\"%s\",\"error\":\"%s\",\"revision\":%lu,\"csrf\":\"%s\",\"claimed\":%s,\"active\":%s,\"waiting_for_power_cycle\":%s,\"pending\":%s,\"recovery_required\":%s,\"size\":%u,\"progress\":%u,\"progress_max\":%u,\"preflight_seconds\":%u,\"sha256\":\"%s\",\"source_sha256\":\"%s\",\"free_heap\":%u,\"largest_block\":%u,\"research_url\":\"%s\"}",
      phase_name_(),error_ ? error_ : "",(unsigned long)revision_,csrf_.c_str(),owner_.empty()?"false":"true",
      active_()?"true":"false",
      ((phase_==Phase::Writing && sender_.state()==desk_protocol::FirmwareInstall::State::Boot) ||
       ((phase_==Phase::Reading || phase_==Phase::Verifying) && receiver_.state()==Receiver::State::Boot)) ? "true":"false",pending_.empty()?"false":"true",recovery_?"true":"false",unsigned(size_),
      unsigned(phase_==Phase::Writing ? sender_.acked_blocks()*1024 : phase_==Phase::Uploading ? upload_offset_ : receiver_.size()),
      unsigned(phase_==Phase::Writing ? payload_size_() : size_),
      unsigned(preflight_ && uint32_t(millis()-preflight_at_)<300000 ? (300000-uint32_t(millis()-preflight_at_))/1000 : 0),
      hash_,source_hash_,unsigned(heap_caps_get_free_size(MALLOC_CAP_8BIT)),unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),research_url_.c_str());
    if (length<0 || size_t(length)>=json.size()) { send(500,"text/plain","Status response too large"); return; }
    send(200,"application/json",json.c_str()); return;
  }
  if (request->method()==HTTP_GET && url=="/desk-firmware/download") {
    if (!buffer_ || (phase_!=Phase::Ready && phase_!=Phase::Staged && phase_!=Phase::Verified)) {
      send(409,"text/plain","No complete downloadable image"); return;
    }
    const auto image=buffer_; const size_t size=size_;
    lock.unlock();
    auto *response=request->beginResponse(200,"application/octet-stream",image.get(),size);
    response->addHeader("Content-Disposition","attachment; filename=t150-application.bin");
    response->addHeader("Cache-Control","no-store"); request->send(response); return;
  }
  if (request->method()!=HTTP_POST || url!="/desk-firmware/action") { send(409,"text/plain","Use GET or POST"); return; }
  const auto csrf=request->get_header("X-Desk-CSRF");
  if (!csrf || *csrf!=csrf_) { send(409,"text/plain","Invalid request token"); return; }
  const auto owner=request->arg("owner"), action=request->arg("action");
  if (!hex_string(owner,32) || request->arg("revision")!=std::to_string(revision_) || !pending_.empty()) {
    send(409,"text/plain","Stale or concurrent request; refresh status"); return;
  }
  if (action=="claim") {
    if (active_() || (!owner_.empty() && owner_!=owner && request->arg("takeover")!="yes") ||
        request->arg("motor_isolated")!="yes") { send(409,"text/plain","Confirm motor isolation and session ownership"); return; }
    // Claim is queued indirectly: UART ownership is acquired by the loop below.
    pending_="claim";
    owner_=owner; ++revision_; send(200,"text/plain","Claim queued"); return;
  }
  if (owner_!=owner || !owned_) { send(409,"text/plain","This tab does not own maintenance"); return; }
  if (active_() && action!="cancel") { send(409,"text/plain","Transfer active"); return; }
  if (action=="chunk") {
    const auto data=request->arg("data");
    if (phase_!=Phase::Uploading || request->arg("offset")!=std::to_string(upload_offset_) ||
        data.empty() || data.size()>256 || data.size()%2 || !hex_string(data,data.size()) ||
        upload_offset_+data.size()/2>size_) { send(409,"text/plain","Invalid upload chunk"); return; }
    for (size_t i=0;i<data.size();i+=2) buffer_.get()[upload_offset_++]=hex_digit(data[i])*16+hex_digit(data[i+1]);
    ++revision_; char accepted[64];
    std::snprintf(accepted,sizeof(accepted),"{\"revision\":%lu}",(unsigned long)revision_);
    send(200,"application/json",accepted); return;
  }
  if (action!="read" && action!="backup" && action!="patch" && action!="patch_child_lock" && action!="stage" && action!="seal" &&
      action!="install" && action!="verify" && action!="cancel" && action!="exit") { send(409,"text/plain","Unknown action"); return; }
  if ((action=="install" || action=="backup") && request->arg("confirmed")!="yes") {
    send(409,"text/plain","Explicit confirmation required"); return;
  }
  pending_=action; ++revision_; send(200,"text/plain","Action queued");
}
}  // namespace esphome::desk_firmware_update
