#pragma once

#include <cstdint>
#include <string>

#include "esphome/core/component.h"
#include "usb/usb_host.h"

#include "eaton_5px_driver.h"

namespace esphome {
namespace nut {

struct ReportFieldMapping {
  uint8_t report_id;
  uint8_t report_type;
  uint16_t bit_offset;
  uint8_t bit_size;
  bool is_signed;
  int8_t exponent;
  uint32_t unit;
  int32_t logical_min;
  int32_t logical_max;
  uint8_t collection_mask;
  UpsSignal signal;
};

struct ReportRequest {
  uint8_t report_id;
  uint8_t report_type;
  uint16_t required_bits;
};

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
  static void usb_transfer_callback_(usb_transfer_t *transfer);
  static void nut_server_task_(void *argument);

  void start_usb_host_();
  void discover_usb_devices_(usb_host_client_handle_t client);
  void serve_nut_client_(int client_fd) const;
  void handle_nut_command_(int client_fd, const std::string &line, bool *authenticated) const;
  bool is_ups_name_(const std::string &name) const;
  void send_line_(int client_fd, const std::string &line) const;
  void log_usb_device_(usb_host_client_handle_t client, uint8_t device_address);
  bool capture_hid_report_descriptor_(usb_host_client_handle_t client, usb_device_handle_t device,
                                      uint8_t interface_number, uint16_t report_length);
  bool parse_hid_report_descriptor_(const uint8_t *descriptor, size_t descriptor_length);
  void poll_hid_reports_(usb_host_client_handle_t client, usb_device_handle_t device);
  bool request_hid_report_(usb_host_client_handle_t client, usb_device_handle_t device, uint8_t interface_number,
                           uint8_t report_type, uint8_t report_id, uint16_t report_length, uint8_t *buffer,
                           size_t *buffer_length);
  static uint64_t extract_bits_(const uint8_t *data, size_t data_length, uint16_t bit_offset, uint8_t bit_size);
  static float apply_exponent_(float value, int8_t exponent);
  void apply_report_data_(uint8_t report_type, uint8_t report_id, const uint8_t *report, size_t report_length);

  std::string ups_name_;
  std::string username_;
  std::string password_;
  std::string description_;
  uint16_t port_{3493};
  volatile bool usb_host_started_{false};
  uint8_t discovered_device_address_{0};
  uint8_t hid_interface_number_{0};
  bool report_descriptor_captured_{false};
  bool has_ac_present_field_{false};
  uint8_t report_field_count_{0};
  uint8_t report_request_count_{0};
  ReportFieldMapping report_fields_[48]{};
  ReportRequest report_requests_[16]{};
  ReportFieldMapping parse_selected_fields_[48]{};
  uint16_t parse_bit_offsets_[4][256]{};
  Eaton5pxDriver driver_;
};

}  // namespace nut
}  // namespace esphome
