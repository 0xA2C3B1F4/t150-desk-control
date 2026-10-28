#pragma once
// Test-only stand-ins for platform calls used by the unchanged IDF adapter.
// UART is an in-memory queue. HTTP pointer responses are non-owning and send
// synchronously, matching ESPHome 2026.4.2 web_server_idf.h. The actual ESPHome
// SHA256 and HashBase implementations are compiled with USE_HOST/OpenSSL.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#define IRAM_ATTR
using BaseType_t = int;
using portMUX_TYPE = std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
#define portENTER_CRITICAL(mux) (mux)->lock()
#define portEXIT_CRITICAL(mux) (mux)->unlock()
#define portENTER_CRITICAL_ISR(mux) (mux)->lock()
#define portEXIT_CRITICAL_ISR(mux) (mux)->unlock()
constexpr int ESP_OK = 0;
constexpr unsigned MALLOC_CAP_8BIT = 1;
enum uart_port_t { UART_NUM_0 = 0, UART_NUM_1 = 1 };
enum uart_select_notif_t { UART_SELECT_READ_NOTIF, UART_SELECT_ERROR_NOTIF };
using uart_select_callback = void (*)(uart_port_t, uart_select_notif_t, BaseType_t *);

namespace export_host {
inline uint32_t now = 100;
inline std::deque<uint8_t> rx;
inline std::vector<uint8_t> tx;
inline bool fail_malloc = false;
inline uart_select_callback uart_callback = nullptr;
inline std::mutex allocation_mutex;
inline std::map<const void *, size_t> allocations;
inline size_t free_count = 0;
constexpr size_t HEAP_SIZE = 1024 * 1024;

inline bool is_live(const void *pointer) {
  std::lock_guard<std::mutex> lock(allocation_mutex);
  return allocations.count(pointer) != 0;
}
inline size_t live_allocations() {
  std::lock_guard<std::mutex> lock(allocation_mutex);
  return allocations.size();
}
inline size_t live_bytes() {
  std::lock_guard<std::mutex> lock(allocation_mutex);
  size_t bytes = 0;
  for (const auto &entry : allocations) bytes += entry.second;
  return bytes;
}
inline size_t free_calls() {
  std::lock_guard<std::mutex> lock(allocation_mutex);
  return free_count;
}
inline void reset() {
  std::lock_guard<std::mutex> lock(allocation_mutex);
  if (!allocations.empty()) throw std::runtime_error("Fixture reset with live image allocations");
  now = 100; rx.clear(); tx.clear(); fail_malloc = false;
  uart_callback = nullptr; free_count = 0;
}
inline void log(const char *, const char *, ...) {}
}  // namespace export_host

#define ESP_LOGI(...) ::export_host::log(__VA_ARGS__)
#define ESP_LOGW(...) ::export_host::log(__VA_ARGS__)
#define ESP_LOGE(...) ::export_host::log(__VA_ARGS__)

inline size_t heap_caps_get_free_size(unsigned) {
  return export_host::HEAP_SIZE - export_host::live_bytes();
}
inline size_t heap_caps_get_largest_free_block(unsigned capability) {
  return heap_caps_get_free_size(capability);
}
inline void *heap_caps_malloc(size_t size, unsigned) {
  if (export_host::fail_malloc) return nullptr;
  void *pointer = std::malloc(size);
  if (pointer) {
    std::lock_guard<std::mutex> lock(export_host::allocation_mutex);
    export_host::allocations.emplace(pointer, size);
  }
  return pointer;
}
inline void heap_caps_free(void *pointer) {
  if (!pointer) return;
  {
    std::lock_guard<std::mutex> lock(export_host::allocation_mutex);
    if (export_host::allocations.erase(pointer) != 1)
      throw std::runtime_error("Free of untracked image allocation");
    ++export_host::free_count;
  }
  std::free(pointer);
}
inline int uart_set_select_notif_callback(uart_port_t, uart_select_callback callback) {
  export_host::uart_callback = callback; return ESP_OK;
}
inline int uart_flush_input(uart_port_t) { export_host::rx.clear(); return ESP_OK; }
inline int uart_write_bytes(uart_port_t, const void *bytes, size_t size) {
  const auto *data = static_cast<const uint8_t *>(bytes);
  export_host::tx.insert(export_host::tx.end(), data, data + size);
  return static_cast<int>(size);
}
inline int uart_get_buffered_data_len(uart_port_t, size_t *size) {
  *size = export_host::rx.size(); return ESP_OK;
}
inline int uart_read_bytes(uart_port_t, void *buffer, size_t size, unsigned) {
  auto *data = static_cast<uint8_t *>(buffer);
  const size_t count = std::min(size, export_host::rx.size());
  for (size_t index = 0; index < count; ++index) {
    data[index] = export_host::rx.front(); export_host::rx.pop_front();
  }
  return static_cast<int>(count);
}

namespace esphome {
inline uint32_t millis() { return export_host::now; }
namespace setup_priority { constexpr float WIFI = 250.0f; }
class Component {
 public:
  virtual ~Component() = default;
  virtual void setup() {}
  virtual void loop() {}
  virtual float get_setup_priority() const { return 0; }
  bool is_failed() const { return failed_; }
  void mark_failed() { failed_ = true; }
 private:
  bool failed_{false};
};
class HighFrequencyLoopRequester {
 public:
  void start() { active = true; }
  void stop() { active = false; }
  bool active{false};
};
class Application {
 public:
  static void wake_loop_isrsafe(BaseType_t *) {}
};
// HashBase's formatting helpers are independent of its actual SHA algorithm.
inline void format_hex_to(char *out, size_t capacity, const uint8_t *data, size_t size) {
  if (capacity < size * 2 + 1) throw std::runtime_error("Hex output buffer too small");
  constexpr char digits[] = "0123456789abcdef";
  for (size_t index = 0; index < size; ++index) {
    out[2 * index] = digits[data[index] >> 4];
    out[2 * index + 1] = digits[data[index] & 15];
  }
  out[size * 2] = '\0';
}
inline bool parse_hex(const char *text, uint8_t *out, size_t size) {
  if (std::strlen(text) != size * 2) return false;
  auto digit = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t index = 0; index < size; ++index) {
    const int high = digit(text[2 * index]), low = digit(text[2 * index + 1]);
    if (high < 0 || low < 0) return false;
    out[index] = static_cast<uint8_t>(high * 16 + low);
  }
  return true;
}
namespace uart {
class IDFUARTComponent : public Component {
 public:
  size_t get_rx_buffer_size() const { return 4096; }
  unsigned get_baud_rate() const { return 115200; }
  int get_hw_serial_number() const { return UART_NUM_1; }
};
}  // namespace uart
namespace desk_uart {
class DeskUART {
 public:
  uart::IDFUARTComponent *firmware_export_uart() { return &uart; }
  bool begin_firmware_export() { ++begin_count; return allow_begin; }
  void end_firmware_export() { ++end_count; }
  bool allow_begin{true};
  unsigned begin_count{0}, end_count{0};
  uart::IDFUARTComponent uart;
};
}  // namespace desk_uart
}  // namespace esphome

constexpr int HTTP_GET = 0;
constexpr int HTTP_POST = 1;
class AsyncWebServerResponse {
 public:
  void addHeader(const char *name, const char *value) { headers[name] = value; }
  std::string storage;
  const uint8_t *data{nullptr};
  size_t size{0};
  bool pointer_response{false};
  std::map<std::string, std::string> headers;
};
class AsyncWebServerRequest {
 public:
  static constexpr size_t URL_BUF_SIZE = 513;
  explicit AsyncWebServerRequest(std::string path, int method = HTTP_GET)
      : path_(std::move(path)), method_(method) {}
  int method() const { return method_; }
  std::string url_to(char (&buffer)[URL_BUF_SIZE]) const {
    if (path_.size() >= URL_BUF_SIZE) throw std::runtime_error("Fixture URL too long");
    std::memcpy(buffer, path_.c_str(), path_.size() + 1);
    return buffer;
  }
  AsyncWebServerResponse *beginResponse(int code, const char *type, const std::string &content) {
    initialize_(code, type);
    response_->storage = content;
    response_->data = reinterpret_cast<const uint8_t *>(response_->storage.data());
    response_->size = response_->storage.size();
    return response_.get();
  }
  AsyncWebServerResponse *beginResponse(int code, const char *type, const uint8_t *data, size_t size) {
    initialize_(code, type);
    response_->data = data; response_->size = size; response_->pointer_response = true;
    response_data = data; response_size = size; pointer_response = true;
    return response_.get();
  }
  void send(AsyncWebServerResponse *response) {
    response_data = response->data; response_size = response->size;
    pointer_response = response->pointer_response;
    headers = response->headers;
    // Intentionally neither copy nor own image bytes before the hook. A blocked
    // hook models synchronous IDF send while another task clears the image.
    if (before_copy) before_copy();
    if (response->size) body.assign(response->data, response->data + response->size);
    else body.clear();
  }
  void send(int code, const char *type, const char *content) {
    send(beginResponse(code, type, std::string(content ? content : "")));
  }
  int response_code{0};
  std::string content_type;
  std::map<std::string, std::string> headers;
  std::vector<uint8_t> body;
  const uint8_t *response_data{nullptr};
  size_t response_size{0};
  bool pointer_response{false};
  std::function<void()> before_copy;
 private:
  void initialize_(int code, const char *type) {
    response_code = code; content_type = type ? type : "";
    response_ = std::make_unique<AsyncWebServerResponse>();
  }
  std::string path_;
  int method_;
  std::unique_ptr<AsyncWebServerResponse> response_;
};
class AsyncWebHandler {
 public:
  virtual ~AsyncWebHandler() = default;
  virtual bool canHandle(AsyncWebServerRequest *) const { return false; }
  virtual void handleRequest(AsyncWebServerRequest *) {}
};
namespace esphome::web_server_base {
class WebServerBase {
 public:
  void add_handler(AsyncWebHandler *handler) { handlers.push_back(handler); }
  std::vector<AsyncWebHandler *> handlers;
};
inline WebServerBase server;
inline WebServerBase *global_web_server_base = &server;
}  // namespace esphome::web_server_base
