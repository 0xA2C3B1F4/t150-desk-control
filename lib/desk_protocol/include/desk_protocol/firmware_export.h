#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace desk_protocol {
// Read-only T150 application export. No reset, upload, erase or unprotect path.
// Storage belongs to the caller. All methods run on the same task.
class FirmwareExport {
 public:
  static constexpr size_t MAX_IMAGE_SIZE = 128 * 1024;
  enum class State { Idle, Boot, Prompt, Metadata, Data, FirstEot, EmptyMetadata, FinalEot, Complete, Failed };
  using Write = bool (*)(void *, const uint8_t *, size_t);
  // Called once, after a valid header and before its ACK. Return storage for
  // exactly size bytes, or nullptr to reject the transfer without a data ACK.
  using Allocate = uint8_t *(*)(void *, size_t);
  void begin(Allocate allocate, Write write, void *context, uint32_t now) {
    image_ = nullptr; allocate_ = allocate; write_ = write; context_ = context;
    size_ = expected_size_ = previous_size_ = used_ = packet_size_ = matched_ = skipped_ = rx_count_ = 0;
    expected_ = 1; bad_ = duplicates_ = eots_ = closing_duplicates_ = 0;
    error_ = nullptr; started_ = now;
    transition_(State::Boot, now);
  }
  bool active() const { return state_ >= State::Boot && state_ <= State::FinalEot; }
  bool data_complete() const { return expected_size_ != 0 && size_ == expected_size_; }
  bool termination_complete() const { return state_ == State::Complete; }
  size_t size() const { return size_; }
  size_t expected_size() const { return expected_size_; }
  State state() const { return state_; }
  unsigned bad_packets() const { return bad_; }
  unsigned duplicates() const { return duplicates_; }
  const char *error() const { return error_; }
  void fail(const char *error) { error_ = error; state_ = State::Failed; }
  void tick(uint32_t now) {
    if (!active()) return;
    if (uint32_t(now - started_) >= 180000U) fail("Overall timeout");
    else if (uint32_t(now - since_) >= (state_ == State::Boot ? 120000U : 5000U)) fail("Response timeout");
  }
  void feed(uint8_t byte, uint32_t now) {
    tick(now);
    if (!active()) return;
    if (++rx_count_ > 1048576U) { fail("Receive budget exceeded"); return; }
    if (state_ == State::Boot) {
      const uint8_t marker[] = {0xFA,0x30,0,1,0x31,0xFD};
      if (match_(byte, marker, sizeof(marker))) {
        transition_(State::Prompt, now);
        const uint8_t command[] = {0xFA,0x31,2,0x5B,0x3E,0,0,0x56,0xFD};
        send_(command, sizeof(command));
      }
      return;
    }
    if (state_ == State::Prompt) {
      const uint8_t prompt[] = "Select Receive File";
      if (match_(byte, prompt, sizeof(prompt)-1)) {
        transition_(State::Metadata, now);
        send_byte_(0x43);
      }
      return;
    }
    if (packet_size_ == 0) {
      if (byte == 4) { eot_(now); return; }
      if (byte == 1 || byte == 2) {
        packet_size_ = (byte == 1 ? 128 : 1024);
        used_ = 0; skipped_ = 0;
      } else if (++skipped_ > 4096) fail("Unexpected UART traffic");
      return;
    }
    packet_[used_++] = byte;
    if (used_ != packet_size_ + 4) return;
    const size_t length = packet_size_;
    packet_size_ = used_ = 0;
    const uint8_t index = packet_[0];
    const uint8_t *payload = packet_ + 2;
    const uint16_t check = uint16_t(packet_[length+2]) << 8 | packet_[length+3];
    if ((index ^ packet_[1]) != 255 || crc(payload, length) != check) {
      if (++bad_ > 3) { fail("Too many CRC or sequence complement errors"); return; }
      since_ = now; send_byte_(0x15); return;
    }
    accept_packet_(index, payload, length, now);
  }
  static uint16_t crc(const uint8_t *data, size_t size) {
    uint16_t value = 0;
    while (size--) {
      value ^= uint16_t(*data++) << 8;
      for (unsigned i=0; i<8; ++i) value = value & 0x8000 ? uint16_t((value << 1) ^ 0x1021) : uint16_t(value << 1);
    }
    return value;
  }
 private:
  bool match_(uint8_t byte, const uint8_t *pattern, size_t length) {
    if (byte == pattern[matched_]) ++matched_;
    else matched_ = byte == pattern[0] ? 1 : 0;
    if (matched_ != length) return false;
    matched_ = 0; return true;
  }
  void transition_(State next, uint32_t now) { state_ = next; since_ = now; matched_ = 0; }
  bool send_(const uint8_t *data, size_t size) {
    if (!write_ || !write_(context_, data, size)) { fail("UART transmission failed"); return false; }
    return true;
  }
  bool send_byte_(uint8_t byte) { return send_(&byte, 1); }
  size_t metadata_size_(const uint8_t *p) const {
    const size_t first = sizeof("UploadedFlashImage.bin");
    size_t end = first;
    while (end < 128 && p[end] != 0 && p[end] != ' ') ++end;
    if (end == 128) return 0;
    if (end > first && p[end-1] == '\r') --end;
    if (end == first) return 0;
    size_t value = 0;
    for (size_t i = first; i < end; ++i) {
      if (p[i] < '0' || p[i] > '9') return 0;
      const size_t digit = p[i] - '0';
      if (value > (MAX_IMAGE_SIZE - digit) / 10) return 0;
      value = value * 10 + digit;
    }
    return value;
  }
  void remember_packet_(const uint8_t *p, size_t length) {
    std::memcpy(previous_, p, length); previous_size_ = length;
  }
  bool same_packet_(const uint8_t *p, size_t length) const {
    return length == previous_size_ && std::memcmp(p, previous_, length) == 0;
  }
  bool empty_(const uint8_t *p, size_t length) const {
    if (length != 128) return false;
    for (size_t i=0; i<length; ++i) if (p[i]) return false;
    return true;
  }
  void duplicate_(uint32_t now) {
    if (++duplicates_ > 8) { fail("Too many duplicate packets"); return; }
    since_ = now; send_byte_(6);
  }
  void accept_packet_(uint8_t index, const uint8_t *p, size_t length, uint32_t now) {
    if (state_ == State::Metadata) {
      const char name[] = "UploadedFlashImage.bin";
      if (index || length != 128 || std::memcmp(p, name, sizeof(name))) {
        fail("Unexpected file header or name"); return;
      }
      expected_size_ = metadata_size_(p);
      if (!expected_size_) { fail("Invalid file size; supported range is 1 to 131072 bytes"); return; }
      image_ = allocate_ ? allocate_(context_, expected_size_) : nullptr;
      if (!image_) { fail("Not enough RAM for firmware image; clear the previous image and retry"); return; }
      remember_packet_(p, length);
      transition_(State::Data, now); send_byte_(6); return;
    }
    if (state_ == State::Data || state_ == State::FirstEot) {
      if (index == static_cast<uint8_t>(expected_-1)) {
        if (!same_packet_(p, length)) { fail("Changed duplicate packet"); return; }
        duplicate_(now); return;
      }
      if (state_ != State::Data || index != expected_) { fail("Unexpected data sequence"); return; }
      const size_t remaining = expected_size_ - size_;
      const size_t count = length < remaining ? length : remaining;
      // CRC covers the entire packet; only advertised file bytes are retained.
      std::memcpy(image_+size_, p, count); size_ += count;
      expected_ = static_cast<uint8_t>(expected_+1);
      remember_packet_(p, length);  // Includes final padding for duplicate checks.
      transition_(data_complete() ? State::FirstEot : State::Data, now);
      send_byte_(6); return;
    }
    if (state_ == State::EmptyMetadata && index == 0 && empty_(p, length)) {
      transition_(State::FinalEot, now); send_byte_(6); return;
    }
    if (state_ == State::FinalEot && index == 0 && empty_(p, length) && ++closing_duplicates_ <= 3) {
      since_ = now; send_byte_(6); return;
    }
    fail("Unexpected closing packet");
  }
  void eot_(uint32_t now) {
    if (state_ == State::FirstEot || state_ == State::EmptyMetadata) {
      if (++eots_ > 10) { fail("Too many EOT attempts"); return; }
      transition_(State::EmptyMetadata, now); send_byte_(6);
    } else if (state_ == State::FinalEot) {
      if (send_byte_(6)) transition_(State::Complete, now);
    } else fail("EOT before complete image");
  }
  State state_{State::Idle};
  uint8_t *image_{nullptr};
  Allocate allocate_{nullptr};
  Write write_{nullptr};
  void *context_{nullptr};
  const char *error_{nullptr};
  uint32_t started_{0}, since_{0}, rx_count_{0};
  size_t size_{0}, expected_size_{0}, previous_size_{0}, used_{0}, packet_size_{0}, matched_{0}, skipped_{0};
  uint8_t expected_{1}, bad_{0}, duplicates_{0}, eots_{0}, closing_duplicates_{0};
  uint8_t packet_[1028]{}, previous_[1024]{};
};
}  // namespace desk_protocol
