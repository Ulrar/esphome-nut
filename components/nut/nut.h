#pragma once

#include <cstdint>
#include <string>

#include "esphome/core/component.h"
#include "usb/usb_host.h"

#include "eaton_5px_driver.h"

namespace esphome {
namespace nut {

class Nut : public Component {
 public:
  void set_ups_name(const std::string &ups_name) { this->ups_name_ = ups_name; }
  void set_username(const std::string &username) { this->username_ = username; }
  void set_password(const std::string &password) { this->password_ = password; }
  void set_description(const std::string &description) { this->description_ = description; }
  void set_port(uint16_t port) { this->port_ = port; }

  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

 protected:
  static void usb_daemon_task_(void *argument);
  static void usb_client_task_(void *argument);
  static void usb_client_event_callback_(const usb_host_client_event_msg_t *event, void *argument);
  static void nut_server_task_(void *argument);

  void start_usb_host_();
  void discover_usb_devices_(usb_host_client_handle_t client) const;
  void serve_nut_client_(int client_fd) const;
  void handle_nut_command_(int client_fd, const std::string &line, bool *authenticated) const;
  bool is_ups_name_(const std::string &name) const;
  void send_line_(int client_fd, const std::string &line) const;
  void log_usb_device_(usb_host_client_handle_t client, uint8_t device_address) const;

  std::string ups_name_;
  std::string username_;
  std::string password_;
  std::string description_;
  uint16_t port_{3493};
  volatile bool usb_host_started_{false};
  Eaton5pxDriver driver_;
};

}  // namespace nut
}  // namespace esphome
