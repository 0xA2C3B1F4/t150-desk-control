#include "desk_provisioning.h"
#include "esphome/components/logger/logger.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include <bootloader_random.h>
#include <cstdio>
#include <cstring>
#include <driver/gpio.h>
#include <driver/uart.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <nvs.h>

#if !defined(USE_ESP32_VARIANT_ESP32C3) || !defined(USE_ESP_IDF) || \
    !defined(USE_API_NOISE_PSK_FROM_YAML) || !defined(USE_WEBSERVER_AUTH)
#error "desk_provisioning requires the reviewed Stamp-C3 distribution configuration"
#endif

namespace esphome {
namespace desk_provisioning {
static const char* const TAG = "desk_provisioning";
namespace {
class NvsHandle {
 public:
  NvsHandle() { error = nvs_open("t150_prov", NVS_READWRITE, &handle); }
  ~NvsHandle() {
    if (error == ESP_OK) {
      nvs_close(handle);
    }
  }
  nvs_handle_t handle{};
  esp_err_t error;
};
class EntropyScope {
 public:
  EntropyScope() { bootloader_random_enable(); }
  ~EntropyScope() { bootloader_random_disable(); }
};
class NvsBackend : public Backend {
 public:
  ReadResult read(Record& record) override {
    NvsHandle nvs;
    if (nvs.error != ESP_OK) {
      return ReadResult::ERROR;
    }
    size_t size = record.size();
    auto err = nvs_get_blob(nvs.handle, "credentials", record.data(), &size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
      return ReadResult::MISSING;
    }
    return err == ESP_OK && size == record.size() ? ReadResult::FOUND : ReadResult::ERROR;
  }
  bool save(const Record& record) override {
    NvsHandle nvs;
    return nvs.error == ESP_OK &&
           nvs_set_blob(nvs.handle, "credentials", record.data(), record.size()) == ESP_OK &&
           nvs_commit(nvs.handle) == ESP_OK;
  }
  bool random(uint8_t* data, size_t size) override {
    EntropyScope entropy;
    esp_fill_random(data, size);
    return true;
  }
};
std::string web_password(const Record& record) {
  auto value = base64_encode(record.data() + 40, 16);
  std::replace(value.begin(), value.end(), '+', '-');
  std::replace(value.begin(), value.end(), '/', '_');
  while (!value.empty() && value.back() == '=') {
    value.pop_back();
  }
  return value;
}
bool waiting(Component* component) {
  return component != nullptr &&
         component->get_actual_setup_priority() < setup_priority::HARDWARE &&
         (component->get_component_state() & COMPONENT_STATE_MASK) == COMPONENT_STATE_CONSTRUCTION;
}
}  // namespace
void DeskProvisioning::fail_() {
  if (api_ != nullptr) {
    api_->mark_failed();
  }
  if (web_ != nullptr) {
    web_->mark_failed();
  }
  if (desk_ != nullptr) {
    desk_->mark_failed();
  }
  record_.fill(0);
  ESP_LOGE(TAG, "Credential setup failed; desk and management services disabled");
  this->mark_failed();
}
void DeskProvisioning::setup() {
  if (this->get_actual_setup_priority() != setup_priority::HARDWARE || !waiting(api_) ||
      !waiting(web_) || !waiting(desk_) || base_ == nullptr || logger::global_logger == nullptr ||
      logger::global_logger->get_uart_num() != UART_NUM_0) {
    fail_();
    return;
  }
  NvsBackend backend;
  auto result = initialize(backend, record_);
  if (result == InitResult::FAILED) {
    fail_();
    return;
  }
  uint8_t mac[6];
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
    fail_();
    return;
  }
  char username[20];
  int count = snprintf(username, sizeof(username), "admin-%02x%02x%02x", mac[3], mac[4], mac[5]);
  auto password = web_password(record_);
  if (count != 12 || password.size() != 22) {
    fail_();
    return;
  }
  gpio_config_t io{};
  io.pin_bit_mask = 1ULL << 3;
  io.mode = GPIO_MODE_INPUT;
  io.pull_up_en = GPIO_PULLUP_ENABLE;
  io.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io.intr_type = GPIO_INTR_DISABLE;
  if (gpio_config(&io) != ESP_OK) {
    fail_();
    return;
  }
  button_.update(gpio_get_level(GPIO_NUM_3) == 0, millis());
  // All fallible initialization is complete. ESPHome borrows these member buffers.
  // They remain unchanged for the lifetime of the component after binding.
  username_ = username;
  std::memcpy(web_password_, password.c_str(), sizeof(web_password_));
  api_->set_noise_psk(record_.data() + 8);
  base_->set_auth_username(username_.c_str());
  base_->set_auth_password(web_password_);
  ESP_LOGI(TAG, "Credentials %s; hold the Stamp user button in USB Logs to read them",
           result == InitResult::CREATED ? "created" : "loaded");
}
void DeskProvisioning::loop() {
  const uint32_t now = millis();
  if (uint32_t(now - last_poll_) < 20) {
    return;
  }
  last_poll_ = now;
  if (button_.update(gpio_get_level(GPIO_NUM_3) == 0, now)) {
    print_credentials_();
  }
}
void DeskProvisioning::print_credentials_() {
  const auto password = web_password(record_);
  const auto key = base64_encode(record_.data() + 8, 32);
  char output[384];
  int size = snprintf(output, sizeof(output),
                      "\r\nT150 local credentials\r\nWeb user: %s\r\nWeb password: %s\r\nHome "
                      "Assistant API key: %s\r\nKeep these credentials private.\r\n",
                      username_.c_str(), password.c_str(), key.c_str());
  if (size <= 0 || size >= static_cast<int>(sizeof(output))) {
    return;
  }
  // Never pass this buffer to ESP_LOG: API subscribers receive those messages.
  int written = uart_write_bytes(UART_NUM_0, output, size);
  if (written != size) {
    ESP_LOGW(TAG, "USB credential write failed; release and press again");
  }
}
}  // namespace desk_provisioning
}  // namespace esphome
