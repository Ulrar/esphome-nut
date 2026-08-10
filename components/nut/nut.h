#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "esphome/core/component.h"
#include "usb/usb_host.h"

#include "nut_hidparser.h"
#include "mge_map.h"

namespace esphome {
namespace nut {

// A NUT variable resolved against the parsed HID report descriptor.
struct ResolvedVar {
  const char *name;    // NUT variable name, e.g. "input.voltage"
  const char *format;  // printf format from the mge-hid table
  int convert;         // 0=plain, 1=Kelvin->Celsius, 2=As->Ah, 3=string index, 4=test result enum
  HIDData_t *item;     // resolved descriptor item
  double value{0};
  char text[32]{};
  bool valid{false};
  bool is_string{false};
};

// A BOOL status path resolved against the descriptor.
struct ResolvedBool {
  const char *status_set;
  const char *status_clear;
  HIDData_t *item;
  bool valid{false};
};

struct ReportRequest {
  uint8_t report_id;
  uint8_t report_type;
  uint16_t length;  // payload bytes (without report ID byte)
};

// Instant command resolved against the descriptor (subset of the
// HU_TYPE_CMD entries in upstream mge-hid.c).
struct InstantCommand {
  const char *name;
  const char *hid_path;
  long value;
  HIDData_t *item;
};

class Nut : public Component {
 public:
  void set_ups_name(const std::string &ups_name) { this->ups_name_ = ups_name; }
  void set_username(const std::string &username) { this->username_ = username; }
  void set_password(const std::string &password) { this->password_ = password; }
  void set_port(uint16_t port) { this->port_ = port; }

  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

 protected:
  static void usb_daemon_task_(void *argument);
  static void usb_client_task_(void *argument);
  static void usb_client_event_callback_(const usb_host_client_event_msg_t *event, void *argument);
  static void usb_transfer_callback_(usb_transfer_t *transfer);
  static void nut_server_task_(void *argument);

  void start_usb_host_();
  void discover_usb_devices_(usb_host_client_handle_t client);
  void serve_nut_client_(int client_fd);
  void handle_nut_command_(int client_fd, const std::string &line, bool *authenticated);
  bool is_ups_name_(const std::string &name) const;
  void send_line_(int client_fd, const std::string &line) const;
  void log_usb_device_(usb_host_client_handle_t client, uint8_t device_address);
  bool capture_hid_report_descriptor_(usb_host_client_handle_t client, usb_device_handle_t device,
                                      uint8_t interface_number, uint16_t report_length);
  void resolve_hid_paths_();
  void poll_hid_reports_(usb_host_client_handle_t client, usb_device_handle_t device);
  bool request_hid_report_(usb_host_client_handle_t client, usb_device_handle_t device, uint8_t interface_number,
                           uint8_t report_type, uint8_t report_id, uint16_t report_length, uint8_t *buffer,
                           size_t *buffer_length);
  bool send_hid_report_(usb_host_client_handle_t client, usb_device_handle_t device, const HIDData_t *item,
                        long value);
  void run_pending_commands_(usb_host_client_handle_t client, usb_device_handle_t device);
  void execute_instant_command_(int client_fd, const std::string &command);
  bool get_descriptor_(usb_host_client_handle_t client, usb_device_handle_t device, uint8_t type, uint8_t index,
                       uint16_t windex, uint8_t *buffer, uint16_t length, size_t *actual);
  bool get_string_descriptor_(usb_host_client_handle_t client, usb_device_handle_t device, uint8_t index,
                              char *buffer, size_t buffer_length);
  const ResolvedVar *find_var_(const char *name) const;
  void get_var_value_(int client_fd, const std::string &variable) const;
  std::string ups_status_() const;

  std::string ups_name_;
  std::string username_;
  std::string password_;
  std::string device_mfr_{"Eaton"};
  std::string device_model_{"UPS"};
  std::string device_serial_;
  uint16_t port_{3493};
  volatile bool usb_host_started_{false};
  volatile bool usb_events_ready_{false};
  uint8_t discovered_device_address_{0};
  uint8_t hid_interface_number_{0};

  HIDDesc_t *hid_desc_{nullptr};
  uint8_t *raw_desc_{nullptr};
  size_t raw_desc_len_{0};
  std::vector<InstantCommand> commands_;
  volatile int pending_command_{-1};  // index into commands_, -1 = none
  volatile int command_result_{0};    // 0 = idle/ok, 1 = failed
  std::vector<ResolvedVar> vars_;
  std::vector<ResolvedBool> bools_;
  std::vector<ReportRequest> report_requests_;
};

}  // namespace nut
}  // namespace esphome
