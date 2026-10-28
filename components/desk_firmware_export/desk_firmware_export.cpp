#include "desk_firmware_export.h"
#include "firmware_profile.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>
#include <esp_heap_caps.h>
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/components/sha256/sha256.h"

namespace esphome::desk_firmware_export {
static const char *const TAG = "desk_firmware_export";
FirmwareExport *FirmwareExport::instance_ = nullptr;
bool FirmwareExport::enqueue_(Command command) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (is_failed() || command_ != Command::None ||
      (command != Command::Cancel && snapshot_.busy)) {
    ESP_LOGW(TAG, "Firmware action refused: reader busy or unavailable");
    return false;
  }
  command_ = command;
  return true;
}
std::string FirmwareExport::status_text() {
  std::lock_guard<std::mutex> lock(mutex_);
  const char *state = snapshot_.state;
  if (!std::strcmp(state, "waiting_boot")) return "Waiting for desk power cycle";
  if (!std::strcmp(state, "waiting_prompt")) return "Waiting for bootloader";
  if (!std::strcmp(state, "metadata")) return "Receiving file header";
  if (!std::strcmp(state, "receiving")) return "Reading firmware";
  if (!std::strcmp(state, "closing")) return "Finishing transfer";
  if (!std::strcmp(state, "complete")) return "Complete";
  if (!std::strcmp(state, "complete_with_warning")) return "Complete with closing warning";
  if (!std::strcmp(state, "cancelled")) return "Cancelled";
  if (!std::strcmp(state, "error")) return "Failed";
  return "Idle";
}
std::string FirmwareExport::error_text() {
  std::lock_guard<std::mutex> lock(mutex_);
  return snapshot_.error ? snapshot_.error : "None";
}
std::string FirmwareExport::sha256_text() {
  std::lock_guard<std::mutex> lock(mutex_);
  return snapshot_.hash;
}
std::string FirmwareExport::profile_status_text() {
  std::lock_guard<std::mutex> lock(mutex_);
  return snapshot_.profile.status_text();
}
std::string FirmwareExport::profile_values_text() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!snapshot_.profile.extracted()) return "Unavailable";
  const auto &values = snapshot_.profile.scalars;
  char text[192];
  const int length = std::snprintf(text, sizeof(text),
      "Lower seed %u; upper seed %u; offset %u.%u cm; high addend %ld; high display %u.%u cm",
      unsigned(values.lower_seed), unsigned(values.upper_seed),
      unsigned(values.display_offset / 10), unsigned(values.display_offset % 10),
      static_cast<long>(values.high_test_addend),
      unsigned(values.high_display / 10), unsigned(values.high_display % 10));
  return length >= 0 && size_t(length) < sizeof(text) ? text : "Unavailable";
}
float FirmwareExport::progress() {
  std::lock_guard<std::mutex> lock(mutex_);
  return snapshot_.expected ? 100.0f * snapshot_.received / snapshot_.expected : 0.0f;
}
bool FirmwareExport::image_ready() {
  std::lock_guard<std::mutex> lock(mutex_);
  return snapshot_.ready;
}

void FirmwareExport::setup() {
  auto *uart = static_cast<uart::IDFUARTComponent *>(desk_->firmware_export_uart());
  if (uart->is_failed() || uart->get_rx_buffer_size() < 2048 || uart->get_baud_rate() != 115200) {
    snapshot_.state = "error"; snapshot_.error = "Requires working 115200 UART with at least 2048-byte RX buffer";
    mark_failed(); return;
  }
  port_ = static_cast<uart_port_t>(uart->get_hw_serial_number());
  instance_ = this;
  uart_set_select_notif_callback(port_, uart_notify_);
  web_server_base::global_web_server_base->add_handler(this);
}
void IRAM_ATTR FirmwareExport::uart_notify_(uart_port_t port, uart_select_notif_t event, BaseType_t *woken) {
  auto *self = instance_;
  if (!self || port != self->port_) return;
  if (event == UART_SELECT_ERROR_NOTIF) {
    portENTER_CRITICAL_ISR(&self->error_mux_);
    self->uart_errors_ = self->uart_errors_ + 1;
    portEXIT_CRITICAL_ISR(&self->error_mux_);
  }
  if (event == UART_SELECT_READ_NOTIF || event == UART_SELECT_ERROR_NOTIF) Application::wake_loop_isrsafe(woken);
}
uint32_t FirmwareExport::errors_() {
  portENTER_CRITICAL(&error_mux_);
  const auto value = uart_errors_;
  portEXIT_CRITICAL(&error_mux_);
  return value;
}
bool FirmwareExport::transmit_(void *context, const uint8_t *bytes, size_t size) {
  auto *self = static_cast<FirmwareExport *>(context);
  return self->owned_ && self->errors_() == self->error_baseline_ &&
         uart_write_bytes(self->port_, bytes, size) == static_cast<int>(size);
}
void FirmwareExport::clear_() {
  // HTTP responses hold their own shared ownership until send() has finished.
  std::lock_guard<std::mutex> lock(mutex_);
  buffer_.reset(); snapshot_ = {};
}
uint8_t *FirmwareExport::allocate_(void *context, size_t size) {
  auto *self = static_cast<FirmwareExport *>(context);
  const size_t free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  if (!size || size > Receiver::MAX_IMAGE_SIZE || free < size + MIN_FREE_HEAP || largest < size) {
    ESP_LOGW(TAG, "Cannot allocate image: size=%u free=%u largest=%u", unsigned(size), unsigned(free), unsigned(largest));
    return nullptr;
  }
  uint8_t *raw = static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_8BIT));
  if (!raw) return nullptr;
  std::shared_ptr<uint8_t> image(raw, heap_caps_free);
  // Recheck after allocation in case another task consumed memory meanwhile.
  if (heap_caps_get_free_size(MALLOC_CAP_8BIT) < MIN_FREE_HEAP) return nullptr;
  self->receiving_buffer_ = std::move(image);
  return raw;
}
void FirmwareExport::start_() {
  if (owned_) return;
  if (!desk_->begin_firmware_export()) {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.error = "Desk must be connected and settled with all operations complete; retry shortly"; return;
  }
  receiving_buffer_.reset();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // Keep the completed image, hash and profile available during a new read.
    snapshot_.received = snapshot_.expected = 0;
    snapshot_.bad = snapshot_.duplicates = 0;
    snapshot_.closed = false;
  }
  owned_ = true; finishing_ = cancelled_ = false;
  receiver_.begin(allocate_, transmit_, this, millis());
  if (uart_flush_input(port_) != ESP_OK) receiver_.fail("UART flush failed");
  error_baseline_ = errors_();
  fast_loop_.start();
  update_snapshot_();
  ESP_LOGI(TAG, "Read armed; waiting for physical desk power cycle. Free heap=%u", unsigned(heap_caps_get_free_size(MALLOC_CAP_8BIT)));
}
void FirmwareExport::finish_() {
  finishing_ = true; finished_at_ = millis();
  // No more protocol traffic; keep normal commands paused for loader exit.
  fast_loop_.stop();
  char hash[65]{};
  desk_protocol::firmware_profile::Result profile;
  if (receiver_.data_complete() && !cancelled_) {
    {
      sha256::SHA256 digest;
      digest.init(); digest.add(receiving_buffer_.get(), receiver_.expected_size()); digest.calculate(); digest.get_hex(hash);
    }
    profile = identify_firmware_profile(receiving_buffer_.get(), receiver_.expected_size());
  }
  update_snapshot_();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (receiver_.data_complete() && !cancelled_) {
      buffer_ = std::move(receiving_buffer_);
      snapshot_.image_size = receiver_.expected_size();
      std::memcpy(snapshot_.hash, hash, sizeof(hash)); snapshot_.profile = profile; snapshot_.ready = true;
      snapshot_.state = receiver_.termination_complete() ? "complete" : "complete_with_warning";
    } else {
      receiving_buffer_.reset();
      snapshot_.state = cancelled_ ? "cancelled" : "error";
    }
  }
  ESP_LOGI(TAG, "Read ended: bytes=%u closing=%s error=%s", unsigned(receiver_.size()),
           receiver_.termination_complete() ? "complete" : "incomplete", receiver_.error() ? receiver_.error() : "none");
}
void FirmwareExport::update_snapshot_() {
  std::lock_guard<std::mutex> lock(mutex_);
  snapshot_.received = receiver_.size(); snapshot_.expected = receiver_.expected_size();
  snapshot_.bad = receiver_.bad_packets();
  snapshot_.duplicates = receiver_.duplicates(); snapshot_.error = receiver_.error();
  snapshot_.busy = owned_; snapshot_.closed = receiver_.termination_complete();
  using State = Receiver::State;
  switch (receiver_.state()) {
    case State::Boot: snapshot_.state = "waiting_boot"; break;
    case State::Prompt: snapshot_.state = "waiting_prompt"; break;
    case State::Metadata: snapshot_.state = "metadata"; break;
    case State::Data: snapshot_.state = "receiving"; break;
    case State::FirstEot: case State::EmptyMetadata: case State::FinalEot: snapshot_.state = "closing"; break;
    default: break;
  }
}
void FirmwareExport::loop() {
  Command command;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    command = command_; command_ = Command::None;
  }
  if (command == Command::Start) start_();
  if (command == Command::Clear && !owned_) clear_();
  if (command == Command::Cancel && owned_ && !finishing_) {
    cancelled_ = true; receiver_.fail("Cancelled; power-cycle the desk if it remains in its loader");
  }
  if (!owned_) return;
  if (finishing_) {
    // Allow the bootloader's application-entry timeout before normal polling.
    if (uint32_t(millis()-finished_at_) >= 3000) {
      uart_flush_input(port_);
      desk_->end_firmware_export(); owned_ = false; finishing_ = false;
      std::lock_guard<std::mutex> lock(mutex_); snapshot_.busy = false;
    }
    return;
  }
  receiver_.tick(millis());
  if (errors_() != error_baseline_) receiver_.fail("UART overflow, frame or parity error");
  size_t waiting = 0;
  if (uart_get_buffered_data_len(port_, &waiting) != ESP_OK) receiver_.fail("UART availability failed");
  if (waiting >= 3584) receiver_.fail("UART receive backlog too large");
  for (size_t budget=0; budget<2048 && receiver_.active(); ++budget) {
    uint8_t byte;
    const int count = uart_read_bytes(port_, &byte, 1, 0);
    if (count == 0) break;
    if (count != 1) { receiver_.fail("UART read failed"); break; }
    receiver_.feed(byte, millis());
  }
  if (!receiver_.active()) finish_();
  else update_snapshot_();
}
bool FirmwareExport::canHandle(AsyncWebServerRequest *request) const {
  char path[AsyncWebServerRequest::URL_BUF_SIZE];
  const auto url = request->url_to(path);
  return request->method() == HTTP_GET &&
         (url == "/desk-firmware/status" || url == "/desk-firmware/download");
}
void FirmwareExport::handleRequest(AsyncWebServerRequest *request) {
  char path[AsyncWebServerRequest::URL_BUF_SIZE];
  const auto url = request->url_to(path);
  if (request->method() == HTTP_GET && url == "/desk-firmware/status") {
    Snapshot value;
    { std::lock_guard<std::mutex> lock(mutex_); value = snapshot_; }
    // State, error and recognition strings are fixed literals. Scalars are
    // bounded integers from a recognized image, never text from the file header.
    char scalars[256]{};
    if (value.profile.extracted()) {
      const auto &fields = value.profile.scalars;
      const int length = std::snprintf(scalars, sizeof(scalars),
          ",\"scalars\":{\"lower_seed\":%u,\"upper_seed\":%u,\"display_offset\":%u,\"high_test_addend\":%ld,\"high_display\":%u}",
          unsigned(fields.lower_seed), unsigned(fields.upper_seed), unsigned(fields.display_offset),
          static_cast<long>(fields.high_test_addend), unsigned(fields.high_display));
      if (length < 0 || size_t(length) >= sizeof(scalars)) {
        request->send(500, "text/plain", "Firmware profile response too large"); return;
      }
    }
    char json[1024];
    const int length = std::snprintf(json, sizeof(json),
      "{\"state\":\"%s\",\"error\":\"%s\",\"received\":%u,\"size\":%u,\"image_size\":%u,\"max_size\":%u,\"busy\":%s,\"ready\":%s,\"closing_complete\":%s,\"sha256\":\"%s\",\"bad_packets\":%u,\"duplicates\":%u,\"free_heap\":%u,\"largest_block\":%u,\"image_profile\":{\"status\":\"%s\",\"extraction_supported\":%s,\"control_approved\":false%s}}",
      value.state, value.error ? value.error : "", unsigned(value.received), unsigned(value.expected),
      unsigned(value.image_size), unsigned(Receiver::MAX_IMAGE_SIZE), value.busy ? "true":"false", value.ready ? "true":"false",
      value.closed ? "true":"false", value.hash, value.bad, value.duplicates,
      unsigned(heap_caps_get_free_size(MALLOC_CAP_8BIT)), unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
      value.profile.status_text(), value.profile.extracted() ? "true" : "false", scalars);
    if (length < 0 || size_t(length) >= sizeof(json)) {
      request->send(500, "text/plain", "Firmware status response too large"); return;
    }
    auto *response = request->beginResponse(200, "application/json", std::string(json));
    response->addHeader("Cache-Control", "no-store"); request->send(response); return;
  }
  if (request->method() == HTTP_GET && url == "/desk-firmware/download") {
    std::shared_ptr<uint8_t> image;
    size_t size = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (snapshot_.ready) { image = buffer_; size = snapshot_.image_size; }
    }
    if (!image) { request->send(409, "text/plain", "No complete firmware image in RAM"); return; }
    // IDF sends this pointer synchronously, without copying the image into a string.
    // The local shared_ptr protects it against concurrent clear/start operations.
    auto *response = request->beginResponse(200, "application/octet-stream", image.get(), size);
    response->addHeader("Content-Disposition", "attachment; filename=UploadedFlashImage.bin");
    response->addHeader("Cache-Control", "no-store"); request->send(response); return;
  }

}
}  // namespace esphome::desk_firmware_export
