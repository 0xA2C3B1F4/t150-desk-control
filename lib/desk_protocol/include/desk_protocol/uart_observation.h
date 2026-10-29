#pragma once

#include <cstdint>
#include <limits>

namespace desk_protocol {

// Cumulative diagnostics only. Never use these counters as motion interlocks.
// Call tick at least once per uint32_t millis period, with current timestamps.
class UARTObservation {
 public:
  struct Snapshot {
    uint32_t boot{0};
    uint64_t ms{0}, samples{0}, gaps{0}, errors{0}, tx{0}, changes{0}, handoffs{0};
    uint32_t age{0};
    uint16_t sequence{0}, q{0};
    bool seen{false};
  };

  void reset(uint32_t boot, uint32_t now) {
    state_ = Snapshot{};
    state_.boot = boot;
    state_.ms = now;
    previous_tick_ = now;
    last_sample_ = 0;
    gap_open_ = false;
  }

  void tick(uint32_t now) {
    state_.ms += static_cast<uint32_t>(now - previous_tick_);
    previous_tick_ = now;
    if (!state_.seen) return;
    const uint64_t elapsed = state_.ms - last_sample_;
    const uint32_t maximum = std::numeric_limits<uint32_t>::max();
    state_.age = elapsed > maximum ? maximum : static_cast<uint32_t>(elapsed);
    if (elapsed > 1000 && !gap_open_) {
      ++state_.gaps;
      gap_open_ = true;
    }
  }

  // The caller has already accepted a new valid height frame and sequence.
  void observe_height(uint32_t now, uint16_t q, uint16_t sequence) {
    tick(now);  // Count a gap even when it ends before another loop iteration.
    if (state_.seen && state_.q != q) ++state_.changes;
    state_.seen = true;
    state_.q = q;
    state_.sequence = sequence;
    ++state_.samples;
    last_sample_ = state_.ms;
    state_.age = 0;
    gap_open_ = false;
  }

  void frame_error() { ++state_.errors; }
  void transmitted() { ++state_.tx; }
  // Firmware export owns UART directly, outside the normal frame sender.
  void handoff() { ++state_.handoffs; }
  const Snapshot &snapshot() const { return state_; }

 private:
  Snapshot state_{};
  uint32_t previous_tick_{0};
  uint64_t last_sample_{0};
  bool gap_open_{false};
};

}  // namespace desk_protocol
