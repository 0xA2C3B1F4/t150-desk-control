#include "../components/desk_provisioning/desk_provisioning.h"
#include "provisioning_stubs/stub.h"
#include <cstdlib>
#include <cstring>
#include <iostream>
namespace fake {
State state;
}
namespace esphome {
namespace logger {
Logger logger;
Logger* global_logger = &logger;
}  // namespace logger
}  // namespace esphome
using namespace esphome;
unsigned checks = 0;
#define CHECK(x)                             \
  do {                                       \
    ++checks;                                \
    if (!(x)) {                              \
      std::cerr << __LINE__ << ": " #x "\n"; \
      std::exit(1);                          \
    }                                        \
  } while (false)
struct Fixture {
  api::APIServer api;
  web_server::WebServer web;
  web_server_base::WebServerBase base;
  desk_uart::DeskUART desk;
  desk_provisioning::DeskProvisioning provisioning;
  Fixture() {
    provisioning.set_api(&api);
    provisioning.set_web(&web);
    provisioning.set_base(&base);
    provisioning.set_desk(&desk);
  }
  void tick(uint32_t now, int button) {
    fake::state.now = now;
    fake::state.button = button;
    provisioning.loop();
  }
  void hold(uint32_t start) {
    tick(start, 0);
    tick(start + 60, 0);
    tick(start + 2060, 0);
  }
  void failed() {
    CHECK(api.state == 3 && web.state == 3 && desk.state == 3 && provisioning.state == 3);
    CHECK(fake::state.uart.empty());
    CHECK(!fake::state.entropy);
  }
};
__attribute__((noinline)) void churn_stack() {
  volatile unsigned char bytes[4096];
  for (unsigned i = 0; i < sizeof(bytes); ++i) {
    bytes[i] = static_cast<unsigned char>(i);
  }
  CHECK(bytes[255] == 255);
}
int main() {
  Fixture good;
  good.provisioning.setup();
  CHECK(good.api.sets == 1 && good.api.noise.has_psk());
  churn_stack();
  CHECK(std::string(good.base.user) == "admin-a1b2c3" && std::strlen(good.base.password) == 22);
  CHECK(std::memcmp(good.api.noise.psk, fake::state.stored.data() + 8, 32) == 0);
  CHECK(fake::state.rng == 1 && fake::state.entropy_order && !fake::state.entropy);
  CHECK(fake::state.opens == 3 && fake::state.closes == 3);
  CHECK(fake::state.uart.empty());
  good.tick(60, 1);
  good.hold(100);
  CHECK(fake::state.uart_writes == 1);
  CHECK(fake::state.uart.find(good.base.password) != std::string::npos);
  auto key = base64_encode(good.api.noise.psk, 32);
  CHECK(fake::state.uart.find(key) != std::string::npos);
  for (const auto& log : fake::state.logs) {
    CHECK(log.find(key) == std::string::npos);
    CHECK(log.find(good.base.password) == std::string::npos);
  }
  good.tick(10000, 0);
  CHECK(fake::state.uart_writes == 1);
  auto saved = fake::state.stored;
  fake::state = fake::State{};
  fake::state.exists = true;
  fake::state.stored = saved;
  Fixture reboot;
  const uint8_t stale_key[32] = {99};
  reboot.api.noise.psk = stale_key;
  reboot.provisioning.setup();
  CHECK(fake::state.rng == 0 && fake::state.writes == 0);
  churn_stack();
  CHECK(std::strcmp(reboot.base.password, good.base.password) == 0 &&
        std::memcmp(reboot.api.noise.psk, good.api.noise.psk, 32) == 0);
  for (unsigned mode = 0; mode < 25; ++mode) {
    fake::state = fake::State{};
    Fixture bad;
    switch (mode) {
      case 0:
        fake::state.open_fail = true;
        break;
      case 1:
        fake::state.read_fail = true;
        break;
      case 2:
        fake::state.write_fail = true;
        break;
      case 3:
        fake::state.commit_fail = true;
        break;
      case 4:
        fake::state.corrupt_readback = true;
        break;
      case 5:
        fake::state.random_byte = 0;
        break;
      case 6:
        fake::state.exists = true;
        fake::state.stored.fill(255);
        break;
      case 7:
        fake::state.mac_fail = true;
        break;
      case 8:
        fake::state.random_byte = 255;
        break;
      case 9:
        fake::state.gpio_fail = true;
        break;
      case 10:
        bad.desk.override_priority = 900;
        break;
    }
    if (mode == 11) {
      logger::global_logger = nullptr;
    }
    if (mode == 12) {
      fake::state.logger_uart = 1;
    }
    if (mode == 13) {
      bad.provisioning.set_base(nullptr);
    }
    if (mode == 14) {
      bad.api.state = 1;
    }
    if (mode == 15) {
      bad.web.state = 1;
    }
    if (mode == 16) {
      fake::state.exists = true;
      fake::state.stored = saved;
      fake::state.blob_size = 59;
    }
    if (mode == 17) {
      bad.provisioning.override_priority = 700;
    }
    if (mode == 18) {
      bad.api.override_priority = 900;
    }
    if (mode == 19) {
      bad.web.override_priority = 800;
    }
    if (mode == 20) {
      fake::state.open_fail_at = 2;
    }
    if (mode == 21) {
      fake::state.open_fail_at = 3;
    }
    if (mode == 22) {
      fake::state.read_fail_at = 2;
    }
    if (mode == 23) {
      fake::state.readback_missing = true;
    }
    if (mode == 24) {
      fake::state.invalid_length = true;
    }
    bad.provisioning.setup();
    logger::global_logger = &logger::logger;
    bad.failed();
    CHECK(bad.api.sets == 0 && bad.base.sets == 0);
    if (mode == 0 || mode == 1 || mode == 24) {
      CHECK(fake::state.rng == 0 && fake::state.writes == 0);
    }
  }
  fake::state = fake::State{};
  fake::state.button = 0;
  Fixture held;
  held.provisioning.setup();
  held.tick(5000, 0);
  CHECK(fake::state.uart_writes == 0);
  held.tick(5100, 1);
  held.tick(5160, 1);
  fake::state.uart_result = 0;
  held.hold(5200);
  CHECK(fake::state.uart_writes == 1);
  CHECK(std::any_of(fake::state.logs.begin(), fake::state.logs.end(), [](const std::string& log) {
    return log.find("USB credential write failed") != std::string::npos;
  }));
  held.tick(10000, 0);
  CHECK(fake::state.uart_writes == 1);
  for (const auto& log : fake::state.logs) {
    CHECK(log.find(held.base.password) == std::string::npos);
  }
  std::cout << checks << " production adapter assertions passed (simulated IDF and services)\n";
}
