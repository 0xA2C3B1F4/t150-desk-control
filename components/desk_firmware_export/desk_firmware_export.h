#pragma once
#include <memory>
#include <mutex>
#include <driver/uart_select.h>
#include "desk_protocol/firmware_export.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/desk_uart/desk_uart.h"
#include "esphome/components/uart/uart_component_esp_idf.h"
#include "esphome/components/web_server_base/web_server_base.h"

namespace esphome::desk_firmware_export {
class FirmwareExport : public Component, public AsyncWebHandler {
 public:
  void set_desk(desk_uart::DeskUART *desk) { desk_ = desk; }
  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::WIFI; }
  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;
  bool start_read() { return enqueue_(Command::Start); }
  bool cancel_read() { return enqueue_(Command::Cancel); }
  bool clear_image() { return enqueue_(Command::Clear); }
  std::string status_text();
  std::string error_text();
  std::string sha256_text();
  float progress();
  bool image_ready();
 protected:
  using Receiver = desk_protocol::FirmwareExport;
  enum class Command { None, Start, Cancel, Clear };
  struct Snapshot {
    const char *state{"idle"};
    const char *error{nullptr};
    size_t received{0}, expected{0}, image_size{0};
    unsigned bad{0}, duplicates{0};
    bool busy{false}, ready{false}, closed{false};
    char hash[65]{};
  };
  static constexpr size_t MIN_FREE_HEAP = 48 * 1024;
  static FirmwareExport *instance_;
  static uint8_t *allocate_(void *context, size_t size);
  static void IRAM_ATTR uart_notify_(uart_port_t port, uart_select_notif_t event, BaseType_t *woken);
  static bool transmit_(void *context, const uint8_t *bytes, size_t size);
  uint32_t errors_();
  bool enqueue_(Command command);
  void start_();
  void finish_();
  void update_snapshot_();
  void clear_();
  desk_uart::DeskUART *desk_{nullptr};
  uart_port_t port_{UART_NUM_1};
  Receiver receiver_;
  std::shared_ptr<uint8_t> buffer_;
  std::shared_ptr<uint8_t> receiving_buffer_;
  std::mutex mutex_;
  Snapshot snapshot_;
  Command command_{Command::None};
  portMUX_TYPE error_mux_ = portMUX_INITIALIZER_UNLOCKED;
  volatile uint32_t uart_errors_{0};
  uint32_t error_baseline_{0}, finished_at_{0};
  bool owned_{false}, finishing_{false}, cancelled_{false};
  HighFrequencyLoopRequester fast_loop_;
};
}  // namespace esphome::desk_firmware_export
