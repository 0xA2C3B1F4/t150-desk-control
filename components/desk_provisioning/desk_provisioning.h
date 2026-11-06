#pragma once
#include "credentials.h"
#include "esphome/components/api/api_server.h"
#include "esphome/components/desk_uart/desk_uart.h"
#include "esphome/components/web_server/web_server.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/core/component.h"

namespace esphome {
namespace desk_provisioning {
class DeskProvisioning : public Component {
 public:
  void set_api(api::APIServer* value) { api_ = value; }
  void set_web(web_server::WebServer* value) { web_ = value; }
  void set_base(web_server_base::WebServerBase* value) { base_ = value; }
  void set_desk(desk_uart::DeskUART* value) { desk_ = value; }
  float get_setup_priority() const override { return setup_priority::HARDWARE; }
  void setup() override;
  void loop() override;

 protected:
  void fail_();
  void print_credentials_();
  api::APIServer* api_{nullptr};
  web_server::WebServer* web_{nullptr};
  web_server_base::WebServerBase* base_{nullptr};
  desk_uart::DeskUART* desk_{nullptr};
  Record record_{};
  HoldButton button_;
  uint32_t last_poll_{0};
  std::string username_;
  char web_password_[23]{};
};
}  // namespace desk_provisioning
}  // namespace esphome
