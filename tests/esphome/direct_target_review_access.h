#pragma once
#include "esphome/components/desk_uart/desk_uart.h"

// Test-only scheduling access: make a parameter poll due during a real GET RTT.
class DirectReviewAccess : public esphome::desk_uart::DeskUART {
 public:
  static void poll_due(esphome::desk_uart::DeskUART *desk) {
    auto member = &DirectReviewAccess::last_limit_poll_;
    desk->*member = esphome::millis() - 5000;
  }
  static bool probing(esphome::desk_uart::DeskUART *desk) {
    auto member = &DirectReviewAccess::direct_;
    return (desk->*member).phase() == desk_protocol::DirectTarget::Phase::Probe;
  }
  static bool known(esphome::desk_uart::DeskUART *desk) {
    auto member = &DirectReviewAccess::direct_;
    return (desk->*member).known();
  }
};
