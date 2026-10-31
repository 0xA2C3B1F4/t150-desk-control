#pragma once
#include "esphome/components/desk_uart/desk_uart.h"
#include <cstdlib>
#include <cstring>

// Keep Python phase labels bound to the production enum.
static_assert(int(desk_protocol::TemporaryPreset::Phase::Recall) == 9, "Update phase fixture: Recall");
static_assert(int(desk_protocol::TemporaryPreset::Phase::Moving) == 10, "Update phase fixture: Moving");
static_assert(int(desk_protocol::TemporaryPreset::Phase::RestoreRead) == 12, "Update phase fixture: RestoreRead");
static_assert(int(desk_protocol::TemporaryPreset::Phase::RestoreWrite) == 13, "Update phase fixture: RestoreWrite");
static_assert(int(desk_protocol::TemporaryPreset::Phase::RestoreVerify) == 14, "Update phase fixture: RestoreVerify");
static_assert(int(desk_protocol::TemporaryPreset::Phase::ClearJournal) == 15, "Update phase fixture: ClearJournal");
static_assert(int(desk_protocol::TemporaryPreset::Phase::Monitoring) == 20, "Update phase fixture: Monitoring");

// Read-only access to the real transaction. No phase or timing state is set.
class RetargetPhaseAccess : public esphome::desk_uart::DeskUART {
 public:
  static const desk_protocol::TemporaryPreset &transaction(esphome::desk_uart::DeskUART *desk) {
    auto member = &RetargetPhaseAccess::target_move_;
    return desk->*member;
  }
  static void height_callback(esphome::desk_uart::DeskUART *desk) {
    static bool fired = false;
    const char *wanted = std::getenv("RETARGET_PHASE");
    if (fired || wanted == nullptr || int(transaction(desk).phase()) != std::atoi(wanted)) return;
    fired = true;
    const auto phase = transaction(desk).phase();
    ESP_LOGI("phase_test", "TRIGGER phase=%d dirty=%d stop=%d ms=%u", int(phase),
             transaction(desk).dirty(), desk->is_stop_latched(), esphome::millis());
    const char *action = std::getenv("RETARGET_ACTION");
    if (action != nullptr && std::strcmp(action, "stop") == 0) {
      ESP_LOGI("phase_test", "STOP accepted=%d", desk->stop());
    } else {
      using Phase = desk_protocol::TemporaryPreset::Phase;
      if (phase == Phase::Moving || phase == Phase::Monitoring) {
        const bool same = desk->move_to_height(80);
        ESP_LOGI("phase_test", "SAME accepted=%d stop=%d", same, desk->is_stop_latched());
      }
      const bool accepted = desk->move_to_height(84);
      ESP_LOGI("phase_test", "RETARGET accepted=%d stop=%d phase_after=%d", accepted,
               desk->is_stop_latched(), int(transaction(desk).phase()));
    }
  }
};
