#pragma once
#include "firmware_export.h"

namespace desk_protocol {
// The pinned T150 loader protocol. One transmission per packet, no retries,
// abort bytes, reset requests or automatic application launch. The adapter
// must validate the full image, preflight and durable recovery marker first.
class FirmwareInstall {
 public:
  enum class State { Idle, Boot, InitialAck, InitialC, HeaderAck, HeaderC, DataAck, EotAck, CloseAck, Complete, Failed };
  enum class Profile { OriginalDT02, ChildLockCL01 };
  using Write = FirmwareExport::Write;
  using BeforeErase = bool (*)(void *);
  void set_before_erase(BeforeErase callback) { before_erase_=callback; }
  bool begin(const uint8_t *image, size_t size, Write write, void *context, uint32_t now, Profile profile=Profile::OriginalDT02) {
    if (active() || !image || size != 55296 || !write) return false;
    if (profile!=Profile::OriginalDT02 && profile!=Profile::ChildLockCL01) return false;
    payload_size_=profile==Profile::ChildLockCL01 ? 26624 : 25600;
    // This erase profile leaves the final page untouched. Everything after
    // the explicitly selected 25/26-block profile must be erased in the target.
    for (size_t i=payload_size_; i<size; ++i) if (image[i]!=255) return false;
    image_=image; write_=write; context_=context; started_=now;
    matched_=rx_=acked_=0; erase_possible_=false; error_=nullptr;
    transition_(State::Boot,now); return true;
  }
  State state() const { return state_; }
  bool active() const { return state_>=State::Boot && state_<=State::CloseAck; }
  bool complete() const { return state_==State::Complete; }
  bool erase_possible() const { return erase_possible_; }
  unsigned acked_blocks() const { return acked_; }
  const char *error() const { return error_; }
  void fail(const char *error) { error_=error; state_=State::Failed; }
  void tick(uint32_t now) {
    if (!active()) return;
    if (uint32_t(now-started_)>=180000) fail("Installation timeout; no retry sent");
    else if (uint32_t(now-since_)>=(state_==State::Boot ? 120000U : 5000U))
      fail("Missing loader response; no retry sent");
  }
  void feed(uint8_t byte, uint32_t now) {
    tick(now); if (!active()) return;
    if (++rx_>65536) { fail("Installation receive budget exceeded"); return; }
    if (state_==State::Boot) {
      const uint8_t boot[]={0xfa,0x30,0,1,0x31,0xfd};
      matched_=byte==boot[matched_] ? matched_+1 : (byte==boot[0] ? 1 : 0);
      if (matched_==sizeof(boot)) {
        const uint8_t request[]={0xfa,0x31,1,0x5b,0x3e,0,0,0x55,0xfd};
        if (before_erase_ && !before_erase_(context_)) { fail("Cannot persist erase-attempt evidence"); return; }
        // Conservatively retain recovery from the first write request, even
        // if the loader never ACKs it or the UART write is only partial.
        erase_possible_=true;
        transition_(State::InitialAck,now); send_(request,sizeof(request));
      }
      return;
    }
    const bool want_c=state_==State::InitialC || state_==State::HeaderC;
    if (byte!=(want_c ? 0x43 : 6)) { fail("Unexpected loader response; no retry sent"); return; }
    switch (state_) {
      case State::InitialAck: transition_(State::InitialC,now); break;
      case State::InitialC: {
        uint8_t header[128]{};
        const char name[]="t150-application.bin";
        std::memcpy(header,name,sizeof(name));
        std::memcpy(header+sizeof(name),payload_size_==26624 ? "26624 " : "25600 ",6);
        transition_(State::HeaderAck,now); send_packet_(0,header,128); break;
      }
      case State::HeaderAck: transition_(State::HeaderC,now); break;
      case State::HeaderC: transition_(State::DataAck,now); send_packet_(1,image_,1024); break;
      case State::DataAck:
        ++acked_;
        if (acked_==payload_size_/1024) { transition_(State::EotAck,now); const uint8_t eot=4; send_(&eot,1); }
        else { transition_(State::DataAck,now); send_packet_(acked_+1,image_+acked_*1024,1024); }
        break;
      case State::EotAck: {
        uint8_t empty[128]{};
        transition_(State::CloseAck,now); send_packet_(0,empty,128); break;
      }
      case State::CloseAck: transition_(State::Complete,now); break;
      default: fail("Invalid installation state"); break;
    }
  }
 private:
  void transition_(State state,uint32_t now) { state_=state; since_=now; }
  bool send_(const uint8_t *bytes,size_t size) {
    if (!write_(context_,bytes,size)) { fail("UART write failed; no retry sent"); return false; }
    return true;
  }
  void send_packet_(uint8_t sequence,const uint8_t *data,size_t size) {
    packet_[0]=size==128 ? 1 : 2; packet_[1]=sequence; packet_[2]=255-sequence;
    std::memcpy(packet_+3,data,size);
    const uint16_t crc=FirmwareExport::crc(data,size);
    packet_[size+3]=crc>>8; packet_[size+4]=crc&255;
    send_(packet_,size+5);
  }
  BeforeErase before_erase_{nullptr};
  State state_{State::Idle};
  const uint8_t *image_{nullptr}; Write write_{nullptr}; void *context_{nullptr};
  uint32_t started_{0},since_{0},rx_{0};
  size_t matched_{0},payload_size_{25600}; unsigned acked_{0}; bool erase_possible_{false};
  const char *error_{nullptr}; uint8_t packet_[1029]{};
};
}  // namespace desk_protocol
