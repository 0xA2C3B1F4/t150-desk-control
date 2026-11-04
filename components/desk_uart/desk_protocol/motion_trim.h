#pragma once

#include <cmath>
#include "protocol.h"

namespace desk_protocol {

// Wire representation only. MotionTrim below handles request correlation.
class MotionTrimCodec {
 public:
  enum Operation : uint8_t { Get = 0, Set = 1 };
  enum Result : uint8_t { Ok = 0, Bad = 1, Stale = 2, Busy = 3, Guard = 4, StorageError = 5 };
  enum StorageState : uint8_t { Default = 0, Verified = 1, Fault = 2, Uncertain = 3 };
  enum StatusFlag : uint8_t { ControlsLocked = 1, PlatformFault = 2 };

  struct Values {
    // Normal up/down, then Slow up/down. Values are controller units, not mm.
    int8_t trim[4]{};
  };

  struct Response {
    Operation operation{Get};
    Result result{Ok};
    StorageState storage{Default};
    uint8_t speed{0};
    uint8_t flags{0};
    uint16_t nonce{0};
    uint16_t sequence{0};
    Values active{};
  };

  static bool valid(const Values& values) {
    for (const int8_t value : values.trim) {
      if (value < -3 || value > 3) {
        return false;
      }
    }
    return true;
  }

  static Frame get(uint16_t nonce) { return request(Get, nonce); }

  // Rejected input leaves out unchanged. A successful build does not consume
  // the token or confirm persistence; never retry a transmitted SET automatically.
  static bool set(uint16_t nonce, uint32_t token, const Values& expected, const Values& desired,
                  Frame& out) {
    if (token == 0 || !valid(expected) || !valid(desired)) {
      return false;
    }
    Frame frame = request(Set, nonce);
    for (size_t i = 0; i < 4; ++i) {
      frame.bytes[5 + i] = static_cast<uint8_t>(token >> (24 - 8 * i));
      frame.bytes[9 + i] = static_cast<uint8_t>(expected.trim[i]);
      frame.bytes[13 + i] = static_cast<uint8_t>(desired.trim[i]);
    }
    out = frame;
    return true;
  }

  // Frame excludes the UART checksum. Decode success means only that the frame
  // is well formed, not that values were saved. Active values describe the
  // controller's current RAM state regardless of the operation result.
  static bool decode(const Frame& frame, Response& out) {
    if (frame.size != 16 || frame.bytes[0] != 0xDA || frame.bytes[1] != 1 || frame.bytes[2] != 3 ||
        frame.bytes[3] > Set || frame.bytes[4] > StorageError || frame.bytes[5] > Uncertain ||
        frame.bytes[6] > 1 || (frame.bytes[7] & ~(ControlsLocked | PlatformFault))) {
      return false;
    }
    Response response{};
    for (size_t i = 0; i < 4; ++i) {
      const uint8_t wire_value = frame.bytes[12 + i];
      // Avoid an implementation-defined unsigned-to-signed narrowing conversion.
      const int value = wire_value < 128 ? wire_value : static_cast<int>(wire_value) - 256;
      if (value < -3 || value > 3) {
        return false;
      }
      response.active.trim[i] = static_cast<int8_t>(value);
    }
    response.operation = static_cast<Operation>(frame.bytes[3]);
    response.result = static_cast<Result>(frame.bytes[4]);
    response.storage = static_cast<StorageState>(frame.bytes[5]);
    response.speed = frame.bytes[6];
    response.flags = frame.bytes[7];
    response.nonce = be16(frame.bytes + 8);
    response.sequence = be16(frame.bytes + 10);
    out = response;
    return true;
  }

 private:
  static Frame request(Operation operation, uint16_t nonce) {
    Frame frame{};
    frame.size = 17;
    frame.bytes[0] = 0xD9;
    frame.bytes[1] = 1;
    frame.bytes[2] = operation;
    frame.bytes[3] = static_cast<uint8_t>(nonce >> 8);
    frame.bytes[4] = static_cast<uint8_t>(nonce);
    return frame;
  }
};

// One read-only probe or one user edit at a time. Each edit reads the four
// current values, then waits for a new RS01 GET before building a single SET.
// The caller owns UART exclusivity, RS01 admission/token lifetime and bounded
// capability probes. Cancel for Stop or maintenance, and disconnect on link
// loss or a controller restart. Neither method sends anything to the controller.
class MotionTrim {
 public:
  static constexpr uint32_t RESPONSE_TIMEOUT_MS = 2000;

  enum class RequestStatus : uint8_t { Started, Busy, Unavailable, InvalidValue };
  enum class Outcome : uint8_t {
    None,
    Read,
    Saved,
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

  // Feed the controller's accepted shared sequence, including RS01 and CL01.
  // A restart needs disconnect() before accepting its new sequence epoch.
  bool observe_sequence(uint16_t sequence) {
    if (have_sequence_ && !newer_(sequence, sequence_)) {
      return false;
    }
    sequence_ = sequence;
    have_sequence_ = true;
    return true;
  }

  RequestStatus probe() {
    if (busy()) {
      return RequestStatus::Busy;
    }
    editing_ = false;
    queue_get_();
    return RequestStatus::Started;
  }

  // Validate the entity's numeric input before any narrowing conversion. A
  // refused edit leaves the in-flight request and its original intent intact.
  RequestStatus edit(size_t index, float value) {
    if (busy()) {
      return RequestStatus::Busy;
    }
    if (index >= 4 || !std::isfinite(value) || value < -3 || value > 3 ||
        value != static_cast<int>(value)) {
      return RequestStatus::InvalidValue;
    }
    if (!supported_ || reply_.flags) {
      return RequestStatus::Unavailable;
    }
    editing_ = true;
    index_ = index;
    value_ = static_cast<int8_t>(value);
    queue_get_();
    return RequestStatus::Started;
  }

  // Supply only the token from a newly requested RS01 GET after awaiting_token()
  // becomes true. Its sequence must follow the TR01 read and must not trail
  // newer telemetry. The caller sends promptly within the RS01 token lifetime.
  bool provide_token(uint32_t token, uint16_t sequence) {
    if (!awaiting_token() || token == 0 || !newer_(sequence, last_confirmed_.sequence) ||
        (have_sequence_ && sequence != sequence_ && !newer_(sequence, sequence_))) {
      return false;
    }
    Frame frame{};
    if (!MotionTrimCodec::set(next_nonce_, token, expected_, desired_, frame)) {
      return false;
    }
    nonce_ = next_nonce_++;
    sequence_ = sequence;
    have_sequence_ = true;
    frame_ = frame;
    stage_ = Stage::SetQueued;
    return true;
  }

  // Copy without consuming. Call mark_transmitted() only after a successful
  // UART send, with its actual transmission time, before dispatching replies.
  // Waiting for UART ownership never starts a controller-response timeout.
  bool pending_frame(Frame& out) const {
    if (stage_ != Stage::GetQueued && stage_ != Stage::SetQueued) {
      return false;
    }
    out = frame_;
    return true;
  }

  bool mark_transmitted(uint32_t now) {
    if (stage_ == Stage::GetQueued) {
      stage_ = Stage::GetSent;
    } else if (stage_ == Stage::SetQueued) {
      stage_ = Stage::SetSent;
      state_known_ = false;
    } else {
      return false;
    }
    sent_at_ = now;
    return true;
  }

  // Ignore unrelated, malformed, duplicate and older replies. A matching
  // successful SET confirms the exact tuple with Default or Verified status.
  // A no-op on a default value does not write its fallback byte to EEPROM.
  // Dispatch TR01 replies here before calling observe_sequence() for that same
  // frame; observing it first would make its sequence look like a duplicate.
  bool accept(const Frame& frame, uint32_t now) {
    if (!waiting_reply_()) {
      return false;
    }
    if (expired_(now)) {
      timeout_();
      return false;
    }
    MotionTrimCodec::Response response{};
    const MotionTrimCodec::Operation operation =
        stage_ == Stage::GetSent ? MotionTrimCodec::Get : MotionTrimCodec::Set;
    if (!MotionTrimCodec::decode(frame, response) || response.operation != operation ||
        response.nonce != nonce_ || (have_sequence_ && !newer_(response.sequence, sequence_))) {
      return false;
    }
    reply_ = response;
    sequence_ = response.sequence;
    have_sequence_ = supported_ = true;
    if (response.result != MotionTrimCodec::Ok) {
      state_known_ = false;
      finish_(Outcome::Rejected);
    } else if (operation == MotionTrimCodec::Get) {
      confirm_(response);
      if (editing_ && response.flags) {
        finish_(Outcome::Rejected);
      } else if (editing_) {
        expected_ = response.active;
        desired_ = expected_;
        desired_.trim[index_] = value_;
        stage_ = Stage::AwaitingToken;
      } else {
        finish_(Outcome::Read);
      }
    } else if (response.flags ||
               (response.storage != MotionTrimCodec::Verified &&
                response.storage != MotionTrimCodec::Default) ||
               !same_values_(response.active, desired_)) {
      state_known_ = false;
      finish_(Outcome::InvalidReply);
    } else {
      confirm_(response);
      finish_(Outcome::Saved);
    }
    return true;
  }

  void tick(uint32_t now) {
    if (waiting_reply_() && expired_(now)) {
      timeout_();
    }
  }

  bool cancel() {
    if (!busy()) {
      return false;
    }
    finish_(Outcome::Cancelled);
    return true;
  }

  void disconnect() {
    finish_(Outcome::Disconnected);
    supported_ = state_known_ = have_sequence_ = false;
    // Keep historical confirmed values for diagnostics, never for a new SET.
  }

  bool busy() const { return stage_ != Stage::Idle; }
  bool awaiting_token() const { return stage_ == Stage::AwaitingToken; }
  // An unanswered discovery probe never proves firmware lacks TR01.
  bool supported() const { return supported_; }
  // A timeout or failed SET leaves historical last_confirmed() values available
  // while state_known() is false. The caller owns connection freshness checks.
  bool state_known() const { return state_known_; }
  bool has_last_confirmed() const { return has_last_confirmed_; }
  Outcome outcome() const { return outcome_; }
  const MotionTrimCodec::Response& reply() const { return reply_; }
  const MotionTrimCodec::Response& last_confirmed() const { return last_confirmed_; }

 private:
  enum class Stage : uint8_t { Idle, GetQueued, GetSent, AwaitingToken, SetQueued, SetSent };

  static bool newer_(uint16_t sequence, uint16_t previous) {
    const uint16_t difference = static_cast<uint16_t>(sequence - previous);
    return difference != 0 && difference < 0x8000;
  }

  static bool same_values_(const MotionTrimCodec::Values& a, const MotionTrimCodec::Values& b) {
    for (size_t i = 0; i < 4; ++i) {
      if (a.trim[i] != b.trim[i]) {
        return false;
      }
    }
    return true;
  }

  bool waiting_reply_() const { return stage_ == Stage::GetSent || stage_ == Stage::SetSent; }
  bool expired_(uint32_t now) const {
    return static_cast<uint32_t>(now - sent_at_) >= RESPONSE_TIMEOUT_MS;
  }

  void queue_get_() {
    nonce_ = next_nonce_++;
    frame_ = MotionTrimCodec::get(nonce_);
    stage_ = Stage::GetQueued;
    outcome_ = Outcome::None;
  }

  void confirm_(const MotionTrimCodec::Response& response) {
    last_confirmed_ = response;
    has_last_confirmed_ = state_known_ = true;
  }

  void finish_(Outcome outcome) {
    stage_ = Stage::Idle;
    editing_ = false;
    outcome_ = outcome;
  }

  void timeout_() {
    state_known_ = false;
    finish_(Outcome::Timeout);
  }

  Frame frame_{};
  MotionTrimCodec::Response reply_{}, last_confirmed_{};
  MotionTrimCodec::Values expected_{}, desired_{};
  Stage stage_{Stage::Idle};
  Outcome outcome_{Outcome::None};
  uint16_t next_nonce_{0}, nonce_{0}, sequence_{0};
  uint32_t sent_at_{0};
  size_t index_{0};
  int8_t value_{0};
  bool editing_{false}, supported_{false}, have_sequence_{false}, state_known_{false},
      has_last_confirmed_{false};
};

}  // namespace desk_protocol
