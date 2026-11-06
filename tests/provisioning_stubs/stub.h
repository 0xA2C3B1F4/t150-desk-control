#pragma once
#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
using esp_err_t = int;
using nvs_handle_t = unsigned;
constexpr int ESP_OK = 0, ESP_ERR_NVS_NOT_FOUND = 1, ESP_ERR_NVS_INVALID_LENGTH = 3,
              NVS_READWRITE = 0, ESP_MAC_WIFI_STA = 0;
constexpr int UART_NUM_0 = 0, GPIO_NUM_3 = 3, GPIO_MODE_INPUT = 1, GPIO_PULLUP_ENABLE = 1,
              GPIO_PULLDOWN_DISABLE = 0, GPIO_INTR_DISABLE = 0;
struct gpio_config_t {
  uint64_t pin_bit_mask;
  int mode, pull_up_en, pull_down_en, intr_type;
};
namespace fake {
struct State {
  bool exists = false, open_fail = false, read_fail = false, write_fail = false,
       commit_fail = false, corrupt_readback = false, entropy = false, entropy_order = true,
       mac_fail = false, gpio_fail = false, readback_missing = false, invalid_length = false;
  uint8_t random_byte = 17;
  std::array<uint8_t, 60> stored{};
  unsigned open_fail_at = 0, read_fail_at = 0;
  unsigned opens = 0, closes = 0, reads = 0, writes = 0, commits = 0, rng = 0, uart_writes = 0;
  uint32_t now = 0;
  int button = 1, uart_result = -2, logger_uart = 0;
  size_t blob_size = 60;
  std::string uart;
  std::vector<std::string> logs;
};
extern State state;
inline void log(const char* fmt, ...) {
  char out[512];
  va_list args;
  va_start(args, fmt);
  vsnprintf(out, sizeof(out), fmt, args);
  va_end(args);
  state.logs.emplace_back(out);
}
}  // namespace fake
#define ESP_LOGE(tag, ...) ((void)(tag), fake::log(__VA_ARGS__))
#define ESP_LOGW(tag, ...) ((void)(tag), fake::log(__VA_ARGS__))
#define ESP_LOGI(tag, ...) ((void)(tag), fake::log(__VA_ARGS__))
inline int nvs_open(const char* name, int, nvs_handle_t* h) {
  ++fake::state.opens;
  *h = fake::state.opens;
  return std::string(name) == "t150_prov" && !fake::state.open_fail &&
                 fake::state.open_fail_at != fake::state.opens
             ? 0
             : 2;
}
inline void nvs_close(nvs_handle_t) {
  ++fake::state.closes;
}
inline int nvs_get_blob(nvs_handle_t, const char*, void* out, size_t* size) {
  auto& s = fake::state;
  ++s.reads;
  if (s.read_fail || s.read_fail_at == s.reads) {
    return 2;
  }
  if (s.invalid_length) {
    return ESP_ERR_NVS_INVALID_LENGTH;
  }
  if (!s.exists || (s.readback_missing && s.reads > 1)) {
    return ESP_ERR_NVS_NOT_FOUND;
  }
  if (*size != s.stored.size()) {
    return 2;
  }
  memcpy(out, s.stored.data(), *size);
  *size = s.blob_size;
  if (s.corrupt_readback && s.reads > 1) {
    static_cast<uint8_t*>(out)[10] ^= 1;
  }
  return 0;
}
inline int nvs_set_blob(nvs_handle_t, const char*, const void* in, size_t size) {
  auto& s = fake::state;
  ++s.writes;
  if (s.write_fail || size != s.stored.size()) {
    return 2;
  }
  memcpy(s.stored.data(), in, size);
  s.exists = true;
  return 0;
}
inline int nvs_commit(nvs_handle_t) {
  ++fake::state.commits;
  return fake::state.commit_fail ? 2 : 0;
}
inline void bootloader_random_enable() {
  fake::state.entropy = true;
}
inline void bootloader_random_disable() {
  fake::state.entropy = false;
}
inline void esp_fill_random(void* out, size_t n) {
  auto& s = fake::state;
  ++s.rng;
  s.entropy_order &= s.entropy;
  memset(out, s.random_byte, n);
}
inline int esp_read_mac(uint8_t* out, int) {
  const uint8_t mac[] = {1, 2, 3, 0xA1, 0xB2, 0xC3};
  memcpy(out, mac, 6);
  return fake::state.mac_fail ? 2 : 0;
}
inline int gpio_config(const gpio_config_t*) {
  return fake::state.gpio_fail ? 2 : 0;
}
inline int gpio_get_level(int) {
  return fake::state.button;
}
inline int uart_write_bytes(int uart, const void* data, size_t size) {
  auto& s = fake::state;
  if (uart != 0) {
    std::abort();
  }
  ++s.uart_writes;
  s.uart.append(static_cast<const char*>(data), size);
  return s.uart_result == -2 ? int(size) : s.uart_result;
}
namespace esphome {
constexpr uint8_t COMPONENT_STATE_MASK = 7, COMPONENT_STATE_CONSTRUCTION = 0,
                  COMPONENT_STATE_FAILED = 3;
namespace setup_priority {
constexpr float HARDWARE = 800, DATA = 600;
}
class Component {
 public:
  virtual ~Component() = default;
  virtual void setup() {};
  virtual void loop() {};
  virtual float get_setup_priority() const { return 600; }
  float get_actual_setup_priority() const {
    return override_priority < 0 ? get_setup_priority() : override_priority;
  }
  uint8_t get_component_state() const { return state; }
  void mark_failed() { state = 3; }
  uint8_t state = 0;
  float override_priority = -1;
};
inline uint32_t millis() {
  return fake::state.now;
}
inline std::string base64_encode(const uint8_t* p, size_t n) {
  const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = uint32_t(p[i]) << 16;
    if (i + 1 < n) {
      v |= uint32_t(p[i + 1]) << 8;
    }
    if (i + 2 < n) {
      v |= p[i + 2];
    }
    out += chars[(v >> 18) & 63];
    out += chars[(v >> 12) & 63];
    out += i + 1 < n ? chars[(v >> 6) & 63] : '=';
    out += i + 2 < n ? chars[v & 63] : '=';
  }
  return out;
}
namespace api {
struct Noise {
  const uint8_t* psk{nullptr};
  bool has_psk() const { return psk != nullptr; }
};
class APIServer : public Component {
 public:
  Noise noise;
  unsigned sets = 0;
  float get_setup_priority() const override { return 200; }
  void set_noise_psk(const uint8_t* p) {
    ++sets;
    noise.psk = p;
  }
  Noise& get_noise_ctx() { return noise; }
};
}  // namespace api
namespace web_server {
class WebServer : public Component {
 public:
  float get_setup_priority() const override { return 249; }
};
}  // namespace web_server
namespace web_server_base {
class WebServerBase {
 public:
  const char *user{nullptr}, *password{nullptr};
  unsigned sets = 0;
  void set_auth_username(const char* x) {
    user = x;
    ++sets;
  }
  void set_auth_password(const char* x) {
    password = x;
    ++sets;
  }
};
}  // namespace web_server_base
namespace desk_uart {
class DeskUART : public Component {};
}  // namespace desk_uart
namespace logger {
class Logger {
 public:
  int get_uart_num() { return fake::state.logger_uart; }
};
extern Logger* global_logger;
}  // namespace logger
}  // namespace esphome
