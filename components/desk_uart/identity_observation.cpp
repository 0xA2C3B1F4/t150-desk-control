#include "desk_uart.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include <cstdio>
#include <cstring>
#include <cinttypes>

namespace esphome {
namespace desk_uart {

static const char* const TAG = "desk_uart";

// Optional, bounded background reads. No identity query participates in motion
// interlocks, ACK trackers, preset recovery or the keep-awake mechanism.
static const uint8_t IDENTITY_SELECTORS[6] = {3, 4, 5, 6, 7, 9};
static const uint8_t IDENTITY_ENTITIES[6] = {0, 1, 2, 2, 2, 3};
void DeskUART::identity_loop_(uint32_t now) {
  if (this->identity_pending_ != 255) {
    if (static_cast<uint32_t>(now - this->identity_sent_at_) < 700)
      return;
    this->identity_pending_ = 255;
  }
  if (static_cast<uint32_t>(now - this->identity_sent_at_) < 1000 ||
      !this->preset_access_allowed_(now) || this->preset_settings_.busy() ||
      this->target_move_.dirty())
    return;
  for (uint8_t offset = 0; offset < 6; ++offset) {
    const uint8_t index = (this->identity_next_ + offset) % 6;
    if (!this->identity_sensors_[IDENTITY_ENTITIES[index]] ||
        (this->identity_received_ & (1U << index)) || this->identity_attempts_[index] >= 3)
      continue;
    desk_protocol::Frame query;
    if (!desk_protocol::read_identity(IDENTITY_SELECTORS[index], query))
      return;
    this->identity_pending_ = index;
    this->identity_next_ = (index + 1) % 6;
    ++this->identity_attempts_[index];
    this->identity_sent_at_ = now;
    this->send_(query);
    return;
  }
}
void DeskUART::identity_frame_(const desk_protocol::Frame& frame, uint32_t now) {
  if (this->identity_pending_ == 255 || static_cast<uint32_t>(now - this->identity_sent_at_) >= 700)
    return;
  desk_protocol::IdentityText value;
  const uint8_t index = this->identity_pending_;
  if (!desk_protocol::decode_identity(frame, value) || value.selector != IDENTITY_SELECTORS[index])
    return;
  this->identity_pending_ = 255;
  this->identity_received_ |= 1U << index;
  if (index >= 2 && index <= 4) {
    std::memcpy(this->identity_values_[index - 2], value.text, sizeof(value.text));
    if ((this->identity_received_ & 0x1C) != 0x1C)
      return;
    char identifiers[80];
    std::snprintf(identifiers, sizeof(identifiers), "05: %s; 06: %s; 07: %s",
                  this->identity_values_[0], this->identity_values_[1], this->identity_values_[2]);
    this->identity_sensors_[2]->publish_state(identifiers);
  } else {
    this->identity_sensors_[IDENTITY_ENTITIES[index]]->publish_state(value.text);
  }
}

void DeskUART::uart_observation_log_() {
  const uint32_t now = millis();
  this->uart_observation_.tick(now);
  if (!this->uart_observation_enabled_ ||
      static_cast<uint32_t>(now - this->last_uart_observation_log_) < 5000)
    return;
  this->last_uart_observation_log_ = now;
  const auto& s = this->uart_observation_.snapshot();
  char age[16];
  if (s.seen)
    std::snprintf(age, sizeof(age), "%" PRIu32, s.age);
  else
    std::snprintf(age, sizeof(age), "null");
  ESP_LOGD(TAG,
           "UART observation: {\"v\":1,\"boot\":%" PRIu32 ",\"ms\":%" PRIu64 ",\"samples\":%" PRIu64
           ",\"age\":%s,\"seq\":%u,\"q\":%u,\"gaps\":%" PRIu64 ",\"errors\":%" PRIu64
           ",\"tx\":%" PRIu64 ",\"changes\":%" PRIu64 ",\"handoffs\":%" PRIu64 "}",
           s.boot, s.ms, s.samples, age, unsigned(s.sequence), unsigned(s.q), s.gaps, s.errors,
           s.tx, s.changes, s.handoffs);
}

}  // namespace desk_uart
}  // namespace esphome
