#pragma once

#include "protocol.h"

namespace desk_protocol {

class DisplaySettingsCodec {
 public:
  enum Operation : uint8_t { Get, Set };
  enum Mode : uint8_t { AutoBlank, Normal };
  enum Result : uint8_t { Ok, Bad, Busy };
  enum Flag : uint8_t { Auto = 1, Attention = 2, TimeLeft = 4, Blank = 8, Unavailable = 16 };

  struct Settings {
    Mode mode{AutoBlank};
    uint16_t timeout_seconds{60};
    bool operator==(const Settings& other) const {
      return mode == other.mode && timeout_seconds == other.timeout_seconds;
    }
  };

  struct Response {
    Operation operation{Get};
    uint16_t nonce{0};
    Result result{Busy};
    uint8_t flags{Unavailable};
    Settings settings{};
  };

  static bool valid(const Settings& settings) {
    return settings.mode <= Normal && settings.timeout_seconds != 0;
  }

  static Frame get(uint16_t nonce) { return request_(Get, nonce); }

  static bool set(uint16_t nonce, const Settings& settings, Frame& output) {
    if (!valid(settings)) {
      return false;
    }
    Frame frame = request_(Set, nonce);
    frame.bytes[6] = settings.mode;
    frame.bytes[7] = static_cast<uint8_t>(settings.timeout_seconds >> 8);
    frame.bytes[8] = static_cast<uint8_t>(settings.timeout_seconds);
    output = frame;
    return true;
  }

  static bool decode(const Frame& frame, Response& output) {
    if (frame.size != 10 || frame.bytes[0] != 0xD8 || frame.bytes[1] != 4 || frame.bytes[2] > Set ||
        frame.bytes[5] > Busy || (frame.bytes[6] & 3) != 3 || (frame.bytes[7] & ~0x1F)) {
      return false;
    }
    uint16_t seconds = be16(frame.bytes + 8);
    const uint8_t flags = frame.bytes[7];
    if (frame.bytes[5] == Ok ? ((flags & Unavailable) || seconds == 0)
                             : (flags != Unavailable || seconds != 0)) {
      return false;
    }
    Response response{};
    response.operation = static_cast<Operation>(frame.bytes[2]);
    response.nonce = be16(frame.bytes + 3);
    response.result = static_cast<Result>(frame.bytes[5]);
    response.flags = flags;
    response.settings.mode = flags & Auto ? AutoBlank : Normal;
    response.settings.timeout_seconds = seconds;
    output = response;
    return true;
  }

 private:
  static Frame request_(Operation operation, uint16_t nonce) {
    Frame frame{};
    frame.size = 12;
    frame.bytes[0] = 0xD7;
    frame.bytes[1] = 4;
    frame.bytes[2] = operation;
    frame.bytes[3] = static_cast<uint8_t>(nonce >> 8);
    frame.bytes[4] = static_cast<uint8_t>(nonce);
    return frame;
  }
};

// The adapter owns discovery limits, UART arbitration and saved user settings.
// This client confirms one request at a time and never retries a sent write.
class DisplaySettings {
 public:
  using Codec = DisplaySettingsCodec;
  static constexpr uint32_t RESPONSE_TIMEOUT_MS = 2000;
  enum class RequestStatus : uint8_t { Started, Busy, Unavailable, InvalidValue };
  enum class Outcome : uint8_t {
    None,
    Read,
    Applied,
    Rejected,
    InvalidReply,
    Timeout,
    Cancelled,
    Disconnected
  };

  bool seed(uint16_t nonce) {
    if (busy()) {
      return false;
    }
    next_nonce_ = nonce;
    return true;
  }

  RequestStatus probe() {
    if (busy()) {
      return RequestStatus::Busy;
    }
    frame_ = Codec::get(next_nonce_++);
    stage_ = Stage::Queued;
    outcome_ = Outcome::None;
    return RequestStatus::Started;
  }

  RequestStatus edit(const Codec::Settings& settings) {
    if (busy()) {
      return RequestStatus::Busy;
    }
    if (!Codec::valid(settings)) {
      return RequestStatus::InvalidValue;
    }
    if (!supported_ || !known_) {
      return RequestStatus::Unavailable;
    }
    if (!Codec::set(next_nonce_, settings, frame_)) {
      return RequestStatus::InvalidValue;
    }
    ++next_nonce_;
    desired_ = settings;
    stage_ = Stage::Queued;
    outcome_ = Outcome::None;
    return RequestStatus::Started;
  }

  bool pending_frame(Frame& output) const {
    if (stage_ != Stage::Queued) {
      return false;
    }
    output = frame_;
    return true;
  }

  bool mark_transmitted(uint32_t now) {
    if (stage_ != Stage::Queued) {
      return false;
    }
    stage_ = Stage::Sent;
    sent_at_ = now;
    if (frame_.bytes[2] == Codec::Set) {
      known_ = false;
    }
    return true;
  }

  bool accept(const Frame& frame, uint32_t now) {
    if (stage_ != Stage::Sent) {
      return false;
    }
    tick(now);
    if (stage_ != Stage::Sent) {
      return false;
    }
    Codec::Response response{};
    if (!Codec::decode(frame, response) || response.operation != frame_.bytes[2] ||
        response.nonce != be16(frame_.bytes + 3)) {
      return false;
    }
    supported_ = true;
    if (response.result != Codec::Ok) {
      known_ = false;
      finish_(Outcome::Rejected);
    } else if (response.operation == Codec::Set && !(response.settings == desired_)) {
      known_ = false;
      finish_(Outcome::InvalidReply);
    } else {
      confirmed_ = response;
      known_ = true;
      finish_(response.operation == Codec::Get ? Outcome::Read : Outcome::Applied);
    }
    return true;
  }

  void tick(uint32_t now) {
    if (stage_ == Stage::Sent && static_cast<uint32_t>(now - sent_at_) >= RESPONSE_TIMEOUT_MS) {
      known_ = false;
      finish_(Outcome::Timeout);
    }
  }

  void cancel() {
    if (busy()) {
      finish_(Outcome::Cancelled);
    }
  }

  void disconnect() {
    supported_ = known_ = false;
    finish_(Outcome::Disconnected);
  }

  bool busy() const { return stage_ != Stage::Idle; }
  bool supported() const { return supported_; }
  bool state_known() const { return known_; }
  bool writing() const { return busy() && frame_.bytes[2] == Codec::Set; }
  Outcome outcome() const { return outcome_; }
  const Codec::Response& confirmed() const { return confirmed_; }

 private:
  enum class Stage : uint8_t { Idle, Queued, Sent };
  void finish_(Outcome outcome) {
    stage_ = Stage::Idle;
    outcome_ = outcome;
  }

  Frame frame_{};
  Codec::Settings desired_{};
  Codec::Response confirmed_{};
  Stage stage_{Stage::Idle};
  Outcome outcome_{Outcome::None};
  uint32_t sent_at_{0};
  uint16_t next_nonce_{0};
  bool supported_{false}, known_{false};
};

}  // namespace desk_protocol
