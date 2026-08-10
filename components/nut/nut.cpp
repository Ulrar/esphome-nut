/* ESPHome NUT server component for USB HID UPS devices.
 *
 * The HID report descriptor parsing, path resolution and value scaling
 * come from Network UPS Tools (drivers/hidparser.c, drivers/libhid.c
 * and drivers/mge-hid.c), vendored under upstream/. NUT is licensed
 * under GPL-2.0-or-later; see the headers of the vendored files.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "nut.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <utility>

#include "esphome/core/log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "usb/usb_host.h"
#include "usb/usb_types_ch9.h"

#include "nut_libhid.h"

namespace esphome {
namespace nut {

static const char *const TAG = "nut";
static constexpr size_t NUT_LINE_LENGTH = 256;
static constexpr uint8_t USB_HID_DESCRIPTOR_TYPE = 0x21;
static constexpr uint8_t USB_HID_REPORT_DESCRIPTOR_TYPE = 0x22;
static constexpr uint16_t MAX_HID_REPORT_DESCRIPTOR_LENGTH = REPORT_DSC_SIZE;
static constexpr uint8_t HID_REPORT_TYPE_INPUT = 1;
static constexpr uint8_t HID_REPORT_TYPE_FEATURE = 3;

struct UsbClientContext {
  Nut *component;
  usb_host_client_handle_t client;
};

struct TransferContext {
  volatile bool complete;
};

static std::string trim_crlf(std::string line) {
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
    line.pop_back();
  }
  return line;
}

static std::string first_word(const std::string &line) {
  const auto delimiter = line.find(' ');
  return line.substr(0, delimiter);
}

static std::string after_first_word(const std::string &line) {
  const auto delimiter = line.find(' ');
  if (delimiter == std::string::npos) {
    return "";
  }
  return line.substr(delimiter + 1);
}

void Nut::setup() {
  if (this->ups_name_.empty() || this->username_.empty() || this->password_.empty()) {
    ESP_LOGE(TAG, "ups_name, username, and password must not be empty");
    this->mark_failed();
    return;
  }

  this->start_usb_host_();

  BaseType_t created = xTaskCreate(nut_server_task_, "nut_server", 6144, this, 4, nullptr);
  if (created != pdPASS) {
    ESP_LOGE(TAG, "Unable to start the NUT server task");
    this->mark_failed();
  }
}

void Nut::dump_config() {
  ESP_LOGCONFIG(TAG, "NUT:");
  ESP_LOGCONFIG(TAG, "  UPS name: %s", this->ups_name_.c_str());
  ESP_LOGCONFIG(TAG, "  Device: %s %s", this->device_mfr_.c_str(), this->device_model_.c_str());
  ESP_LOGCONFIG(TAG, "  NUT TCP port: %u", this->port_);
  ESP_LOGCONFIG(TAG, "  USB host: %s", this->usb_host_started_ ? "started" : "not started");
}

void Nut::start_usb_host_() {
  BaseType_t daemon_created = xTaskCreate(usb_daemon_task_, "nut_usb", 6144, this, 5, nullptr);
  BaseType_t client_created = xTaskCreate(usb_client_task_, "nut_usb_client", 12288, this, 5, nullptr);

  if (daemon_created != pdPASS || client_created != pdPASS) {
    ESP_LOGE(TAG, "Unable to start USB host tasks");
    this->mark_failed();
  }
}

void Nut::usb_daemon_task_(void *argument) {
  auto *component = static_cast<Nut *>(argument);
  usb_host_config_t config{};
  config.intr_flags = ESP_INTR_FLAG_LEVEL1;

  const esp_err_t install_result = usb_host_install(&config);
  if (install_result != ESP_OK) {
    ESP_LOGE(TAG, "USB host install failed: %s", esp_err_to_name(install_result));
    component->usb_host_started_ = false;
    vTaskDelete(nullptr);
    return;
  }

  ESP_LOGI(TAG, "USB host is ready; connect the UPS USB port to the S3 host port");
  component->usb_host_started_ = true;
  while (true) {
    uint32_t event_flags = 0;
    const esp_err_t result = usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
    if (result != ESP_OK) {
      ESP_LOGW(TAG, "USB host event handling failed: %s", esp_err_to_name(result));
    }
  }
}

void Nut::usb_client_task_(void *argument) {
  auto *component = static_cast<Nut *>(argument);

  // Let the daemon own USB library installation before registering this client.
  for (uint8_t attempts = 0; attempts < 20 && !component->usb_host_started_; attempts++) {
    vTaskDelay(pdMS_TO_TICKS(250));
  }
  if (!component->usb_host_started_) {
    ESP_LOGE(TAG, "USB host did not become ready");
    vTaskDelete(nullptr);
    return;
  }

  usb_host_client_config_t config{};
  config.is_synchronous = false;
  config.max_num_event_msg = 5;
  UsbClientContext context{component, nullptr};
  config.async.client_event_callback = usb_client_event_callback_;
  config.async.callback_arg = &context;
  usb_host_client_handle_t client = nullptr;
  const esp_err_t register_result = usb_host_client_register(&config, &client);
  if (register_result != ESP_OK) {
    ESP_LOGE(TAG, "USB client registration failed: %s", esp_err_to_name(register_result));
    vTaskDelete(nullptr);
    return;
  }
  context.client = client;
  component->usb_events_ready_ = true;
  component->discover_usb_devices_(client);

  while (true) {
    const esp_err_t result = usb_host_client_handle_events(client, pdMS_TO_TICKS(5000));
    if (result != ESP_OK && result != ESP_ERR_TIMEOUT) {
      ESP_LOGW(TAG, "USB client event handling failed: %s", esp_err_to_name(result));
    }
    component->discover_usb_devices_(client);
  }
}

void Nut::usb_client_event_callback_(const usb_host_client_event_msg_t *event, void *argument) {
  auto *context = static_cast<UsbClientContext *>(argument);
  ESP_LOGD(TAG, "USB client event: %d", event->event);
  // The periodic scan in usb_client_task_ discovers devices; the event
  // callback must not open/close the device concurrently (assert in
  // usb_host_device_close when the scan already holds it).
  (void) context;
  (void) event;
}

void Nut::discover_usb_devices_(usb_host_client_handle_t client) {
  ESP_LOGD(TAG, "Scanning USB addresses for already attached devices");
  uint8_t addresses[16]{};
  int device_count = 0;
  const esp_err_t list_result =
      usb_host_device_addr_list_fill(static_cast<int>(sizeof(addresses)), addresses, &device_count);
  if (list_result != ESP_OK) {
    ESP_LOGW(TAG, "Unable to list USB device addresses: %s", esp_err_to_name(list_result));
    return;
  }

  for (int index = 0; index < device_count; index++) {
    // log_usb_device_ opens and closes the device itself; do not open it
    // here first (double open/close asserts in usb_host_device_close).
    this->log_usb_device_(client, addresses[index]);
  }
}

void Nut::log_usb_device_(usb_host_client_handle_t client, uint8_t device_address) {
  // Skip devices we already hold open.
  if (this->ups_device_ != nullptr && device_address == this->discovered_device_address_ &&
      this->hid_desc_ != nullptr) {
    // Already captured; just poll.
    this->poll_hid_reports_(client, this->ups_device_);
    return;
  }

  usb_device_handle_t device = this->ups_device_;
  if (device == nullptr) {
    const esp_err_t open_result = usb_host_device_open(client, device_address, &device);
    if (open_result != ESP_OK) {
      ESP_LOGW(TAG, "Unable to open USB device %u: %s", device_address, esp_err_to_name(open_result));
      return;
    }
    this->ups_device_ = device;
    this->discovered_device_address_ = device_address;
  }

  const usb_device_desc_t *descriptor = nullptr;
  const esp_err_t descriptor_result = usb_host_get_device_descriptor(device, &descriptor);
  if (descriptor_result != ESP_OK || descriptor == nullptr) {
    ESP_LOGW(TAG, "Unable to read USB device descriptor: %s", esp_err_to_name(descriptor_result));
    return;
  }

  ESP_LOGI(TAG, "USB device %u: VID=%04X PID=%04X bcdDevice=%04X configurations=%u", device_address,
           descriptor->idVendor, descriptor->idProduct, descriptor->bcdDevice, descriptor->bNumConfigurations);

  // Device identity from USB string descriptors (like upstream's
  // format_mfr/format_serial; model adds iModel from HID later).
  {
    char text[64];
    if (descriptor->iManufacturer != 0 &&
        this->get_string_descriptor_(client, device, descriptor->iManufacturer, text, sizeof(text))) {
      this->device_mfr_ = text;
    }
    if (descriptor->iProduct != 0 &&
        this->get_string_descriptor_(client, device, descriptor->iProduct, text, sizeof(text))) {
      this->device_model_ = text;
    }
    if (descriptor->iSerialNumber != 0 &&
        this->get_string_descriptor_(client, device, descriptor->iSerialNumber, text, sizeof(text))) {
      this->device_serial_ = text;
    }
  }

  // Fetch the configuration descriptor with a manual control transfer;
  // usb_host_get_active_config_descriptor() hangs on this device.
  uint8_t config_buffer[512]{};
  size_t config_actual = 0;
  if (!this->get_descriptor_(client, device, USB_B_DESCRIPTOR_TYPE_CONFIGURATION, 0, 0, config_buffer, 9,
                             &config_actual) ||
      config_actual < 9) {
    ESP_LOGW(TAG, "Unable to read USB configuration descriptor header");
    return;
  }
  const uint16_t config_total = static_cast<uint16_t>(config_buffer[2]) |
                                (static_cast<uint16_t>(config_buffer[3]) << 8);
  if (!this->get_descriptor_(client, device, USB_B_DESCRIPTOR_TYPE_CONFIGURATION, 0, 0, config_buffer,
                             std::min<uint16_t>(config_total, sizeof(config_buffer)), &config_actual)) {
    ESP_LOGW(TAG, "Unable to read full USB configuration descriptor (%u bytes)", config_total);
    return;
  }
  const uint8_t *config_data = config_buffer;
  const uint16_t config_length = static_cast<uint16_t>(std::min<size_t>(config_actual, sizeof(config_buffer)));

  uint8_t hid_interface = 0;
  uint16_t report_length = 0;
  bool hid_interface_found = false;
  for (uint16_t offset = 0; offset + 2 <= config_length;) {
    const uint8_t length = config_data[offset];
    const uint8_t type = config_data[offset + 1];
    if (length < 2 || offset + length > config_length) {
      ESP_LOGW(TAG, "Malformed USB configuration descriptor at byte %u", offset);
      break;
    }

    if (type == USB_B_DESCRIPTOR_TYPE_INTERFACE && length >= 9) {
      hid_interface_found = config_data[offset + 5] == USB_CLASS_HID;
      hid_interface = config_data[offset + 2];
    } else if (hid_interface_found && type == USB_HID_DESCRIPTOR_TYPE && length >= 9 &&
               config_data[offset + 6] == USB_HID_REPORT_DESCRIPTOR_TYPE) {
      report_length = static_cast<uint16_t>(config_data[offset + 7]) |
                      (static_cast<uint16_t>(config_data[offset + 8]) << 8);
      break;
    }
    offset += length;
  }

  if (report_length == 0) {
    ESP_LOGW(TAG, "No HID report descriptor found in the active USB configuration");
  } else if (report_length > MAX_HID_REPORT_DESCRIPTOR_LENGTH) {
    ESP_LOGW(TAG, "HID report descriptor is too large: %u bytes", report_length);
  } else {
    if (this->hid_desc_ == nullptr || this->discovered_device_address_ != device_address) {
      this->discovered_device_address_ = device_address;
      this->hid_interface_number_ = hid_interface;
      this->capture_hid_report_descriptor_(client, device, hid_interface, report_length);
    }
    if (this->hid_desc_ != nullptr) {
      this->poll_hid_reports_(client, device);
    }
  }
  // Device stays open for the lifetime of the attachment.
}

void Nut::usb_transfer_callback_(usb_transfer_t *transfer) {
  auto *context = static_cast<TransferContext *>(transfer->context);
  context->complete = true;
}

bool Nut::get_descriptor_(usb_host_client_handle_t client, usb_device_handle_t device, uint8_t type, uint8_t index,
                          uint16_t windex, uint8_t *buffer, uint16_t length, size_t *actual) {
  usb_transfer_t *transfer = nullptr;
  const esp_err_t allocation_result =
      usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + length, 0, &transfer);
  if (allocation_result != ESP_OK) {
    ESP_LOGW(TAG, "Unable to allocate descriptor transfer: %s", esp_err_to_name(allocation_result));
    return false;
  }

  TransferContext context{false};
  usb_setup_packet_t setup{};
  const uint8_t recipient = type == 0x03 ? USB_BM_REQUEST_TYPE_RECIP_DEVICE : USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
  setup.bmRequestType = USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_STANDARD | recipient;
  setup.bRequest = USB_B_REQUEST_GET_DESCRIPTOR;
  setup.wValue = static_cast<uint16_t>((type << 8) | index);
  setup.wIndex = windex;
  setup.wLength = length;

  memset(transfer->data_buffer, 0, transfer->data_buffer_size);
  memcpy(transfer->data_buffer, &setup, sizeof(setup));
  transfer->num_bytes = static_cast<int>(sizeof(usb_setup_packet_t) + length);
  transfer->bEndpointAddress = 0;
  transfer->device_handle = device;
  transfer->callback = usb_transfer_callback_;
  transfer->context = &context;
  const esp_err_t submit_result = usb_host_transfer_submit_control(client, transfer);
  if (submit_result != ESP_OK) {
    ESP_LOGW(TAG, "Unable to request descriptor: %s", esp_err_to_name(submit_result));
    usb_host_transfer_free(transfer);
    return false;
  }
  ESP_LOGD(TAG, "Descriptor transfer submitted (type=0x%02X len=%u)", type, length);

  for (uint8_t attempts = 0; attempts < 20 && !context.complete; attempts++) {
    usb_host_client_handle_events(client, pdMS_TO_TICKS(100));
  }
  ESP_LOGD(TAG, "Descriptor transfer wait done: complete=%d status=%d", context.complete, transfer->status);

  const bool complete = context.complete && transfer->status == USB_TRANSFER_STATUS_COMPLETED;
  if (!complete) {
    ESP_LOGW(TAG, "Descriptor transfer did not complete: status=%d", transfer->status);
    usb_host_transfer_free(transfer);
    return false;
  }

  size_t payload = 0;
  if (transfer->actual_num_bytes > static_cast<int>(sizeof(usb_setup_packet_t))) {
    payload = static_cast<size_t>(transfer->actual_num_bytes - sizeof(usb_setup_packet_t));
  }
  payload = std::min(payload, static_cast<size_t>(length));
  memcpy(buffer, transfer->data_buffer + sizeof(usb_setup_packet_t), payload);
  *actual = payload;
  usb_host_transfer_free(transfer);
  return true;
}

bool Nut::get_string_descriptor_(usb_host_client_handle_t client, usb_device_handle_t device, uint8_t index,
                                 char *buffer, size_t buffer_length) {
  // USB string descriptors: device-level GET_DESCRIPTOR, wIndex=LANGID.
  // Use English (0x0409) like upstream; fall back to whatever we get.
  uint8_t raw[255];
  size_t actual = 0;
  // First request just the header to learn the real length.
  if (!this->get_descriptor_(client, device, 0x03, index, 0x0409, raw, 4, &actual) || actual < 2) {
    return false;
  }
  const uint8_t total = raw[0];
  if (total < 2) {
    return false;
  }
  if (!this->get_descriptor_(client, device, 0x03, index, 0x0409, raw, total, &actual) || actual < 2) {
    return false;
  }
  // UTF-16LE -> ASCII, drop the header.
  size_t out = 0;
  for (size_t i = 2; i + 1 < actual && out + 1 < buffer_length; i += 2) {
    const uint16_t ch = static_cast<uint16_t>(raw[i]) | (static_cast<uint16_t>(raw[i + 1]) << 8);
    if (ch == 0) {
      break;
    }
    buffer[out++] = ch < 128 ? static_cast<char>(ch) : '?';
  }
  buffer[out] = '\0';
  return out > 0;
}

bool Nut::capture_hid_report_descriptor_(usb_host_client_handle_t client, usb_device_handle_t device,
                                         uint8_t interface_number, uint16_t report_length) {
  auto *scratch = static_cast<uint8_t *>(malloc(REPORT_DSC_SIZE));
  auto *best_raw = static_cast<uint8_t *>(malloc(REPORT_DSC_SIZE));
  if (scratch == nullptr || best_raw == nullptr) {
    ESP_LOGW(TAG, "Unable to allocate the HID descriptor buffers");
    free(scratch);
    free(best_raw);
    return false;
  }

  // NUT uses hid_desc_index=1 for Eaton VID 0463 with bcdDevice 0x0202:
  // those devices expose a reduced descriptor at index 0 and a richer one
  // at index 1. Try both, keep whichever parses to more items.
  HIDDesc_t *best_desc = nullptr;
  size_t best_len = 0;
  for (uint8_t try_index = 1;; try_index--) {
    size_t length = 0;
    if (this->get_descriptor_(client, device, USB_HID_REPORT_DESCRIPTOR_TYPE, try_index, interface_number,
                              scratch, REPORT_DSC_SIZE, &length) &&
        length > 0) {
      HIDDesc_t *parsed = Parse_ReportDesc(scratch, length);
      if (parsed != nullptr) {
        ESP_LOGI(TAG, "Report descriptor index %u: %u bytes, %u items", try_index,
                 static_cast<unsigned>(length), static_cast<unsigned>(parsed->nitems));
        if (best_desc == nullptr || parsed->nitems > best_desc->nitems) {
          if (best_desc != nullptr) {
            Free_ReportDesc(best_desc);
          }
          best_desc = parsed;
          best_len = length;
          memcpy(best_raw, scratch, length);
        } else {
          Free_ReportDesc(parsed);
        }
      }
    }
    if (try_index == 0) {
      break;
    }
  }

  free(scratch);

  if (best_desc == nullptr) {
    ESP_LOGW(TAG, "No usable HID report descriptor on the device");
    free(best_raw);
    return false;
  }

  if (this->hid_desc_ != nullptr) {
    Free_ReportDesc(this->hid_desc_);
  }
  this->hid_desc_ = best_desc;
  if (this->raw_desc_ != nullptr) {
    free(this->raw_desc_);
  }
  this->raw_desc_ = best_raw;
  this->raw_desc_len_ = best_len;

  ESP_LOGI(TAG, "Using HID report descriptor: %u bytes, %u items", static_cast<unsigned>(best_len),
           static_cast<unsigned>(best_desc->nitems));

  // Report the UPS USB personality mode (UPS.System.USB.Mode) when present.
  HIDData_t *mode_item = nut_hid_find_object(this->hid_desc_, "UPS.System.USB.Mode");
  if (mode_item != nullptr) {
    uint8_t report[8] = {0};
    size_t actual = 0;
    if (this->request_hid_report_(client, device, this->hid_interface_number_, HID_REPORT_TYPE_FEATURE,
                                  mode_item->ReportID, sizeof(report), report, &actual) &&
        actual > 0) {
      if (report[0] != mode_item->ReportID) {
        memmove(report + 1, report, std::min<size_t>(actual, sizeof(report) - 1));
        report[0] = mode_item->ReportID;
      }
      long mode = 0;
      GetValue(report, mode_item, &mode);
      ESP_LOGI(TAG, "USB.Mode = %ld (report 0x%02X)", mode, mode_item->ReportID);
    }
  }

  this->resolve_hid_paths_();
  return !this->report_requests_.empty();
}

void Nut::resolve_hid_paths_() {
  this->vars_.clear();
  this->bools_.clear();
  this->report_requests_.clear();

  // Full HID tree dump at debug level, like upstream HIDDumpTree().
  if (this->hid_desc_ != nullptr) {
    char path[160];
    for (size_t i = 0; i < this->hid_desc_->nitems; i++) {
      const HIDData_t *item = &this->hid_desc_->item[i];
      if (nut_path_to_string(path, sizeof(path), &item->Path) > 0) {
        ESP_LOGD(TAG, "Path: %s, Type: %s, ReportID: 0x%02X, Offset: %u, Size: %u", path,
                 item->Type == ITEM_FEATURE ? "Feature" : item->Type == ITEM_INPUT ? "Input" : "Output",
                 item->ReportID, item->Offset, item->Size);
      }
    }
  }

  // NUT variables: walk the mge-hid table in order, first path that
  // resolves wins, exactly like upsdrv_initinfo() in usbhid-ups.
  for (size_t i = 0; i < MGE_READ_MAP_COUNT; i++) {
    const auto &entry = MGE_READ_MAP[i];
    bool already = false;
    for (const auto &var : this->vars_) {
      if (strcmp(var.name, entry.nut_var) == 0) {
        already = true;
        break;
      }
    }
    if (already) {
      continue;
    }
    HIDData_t *item = nut_hid_find_object(this->hid_desc_, entry.hid_path);
    if (item == nullptr) {
      continue;
    }
    this->vars_.push_back({entry.nut_var, entry.format, entry.convert, item, 0.0, false});
    ESP_LOGD(TAG, "Mapped %s -> %s (report 0x%02X)", entry.nut_var, entry.hid_path, item->ReportID);
  }

  // Enum-valued vars: the HID value indexes a lookup string, like
  // upstream test_read_info (usbhid-ups.c). convert=5 items are internal
  // ABM inputs used to synthesize battery.charger.status.
  static const struct {
    const char *nut_var;
    const char *hid_path;
    int convert;
  } ENUM_VARS[] = {
      {"ups.test.result", "UPS.BatterySystem.Battery.Test", 4},
      {"battery.charger.abm.status", "UPS.BatterySystem.Charger.ABMEnable", 5},
      {"battery.charger.mode.status", "UPS.BatterySystem.Charger.Mode", 5},
      {"battery.charger.type.status", "UPS.BatterySystem.Charger.Status", 5},
  };
  for (const auto &entry : ENUM_VARS) {
    HIDData_t *item = nut_hid_find_object(this->hid_desc_, entry.hid_path);
    if (item == nullptr) {
      continue;
    }
    ResolvedVar var{};
    var.name = entry.nut_var;
    var.convert = entry.convert;
    var.item = item;
    var.is_string = true;
    this->vars_.push_back(var);
    ESP_LOGD(TAG, "Mapped enum %s -> %s (report 0x%02X)", entry.nut_var, entry.hid_path, item->ReportID);
  }

  // String-index vars (stringid_conversion in upstream): the HID value
  // is a USB string descriptor index.
  static const struct {
    const char *nut_var;
    const char *hid_path;
  } STRING_VARS[] = {
      {"ups.firmware", "UPS.PowerSummary.iVersion"},
      {"battery.type", "UPS.PowerSummary.iDeviceChemistry"},
      {"ups.model", "UPS.PowerSummary.iModel"},
  };
  for (const auto &entry : STRING_VARS) {
    HIDData_t *item = nut_hid_find_object(this->hid_desc_, entry.hid_path);
    if (item == nullptr) {
      continue;
    }
    ResolvedVar var{};
    var.name = entry.nut_var;
    var.convert = 3;
    var.item = item;
    var.is_string = true;
    this->vars_.push_back(var);
    ESP_LOGD(TAG, "Mapped string %s -> %s (report 0x%02X)", entry.nut_var, entry.hid_path, item->ReportID);
  }

  for (size_t i = 0; i < MGE_BOOL_MAP_COUNT; i++) {
    const auto &entry = MGE_BOOL_MAP[i];
    HIDData_t *item = nut_hid_find_object(this->hid_desc_, entry.hid_path);
    if (item == nullptr) {
      continue;
    }
    this->bools_.push_back({entry.status_set, entry.status_clear, item, false});
    ESP_LOGD(TAG, "Mapped status %s", entry.hid_path);
  }

  // Instant commands, values from upstream mge-hid.c HU_TYPE_CMD entries.
  this->commands_.clear();
  static const struct {
    const char *name;
    const char *hid_path;
    long value;
  } COMMANDS[] = {
      {"test.battery.start.quick", "UPS.BatterySystem.Battery.Test", 1},
      {"test.battery.start.deep", "UPS.BatterySystem.Battery.Test", 2},
      {"test.battery.stop", "UPS.BatterySystem.Battery.Test", 3},
      {"load.off.delay", "UPS.PowerSummary.DelayBeforeShutdown", 20},   // DEFAULT_OFFDELAY
      {"load.on.delay", "UPS.PowerSummary.DelayBeforeStartup", 30},     // DEFAULT_ONDELAY
      {"shutdown.stop", "UPS.PowerSummary.DelayBeforeShutdown", -1},
      {"shutdown.reboot", "UPS.PowerSummary.DelayBeforeReboot", 10},
      {"beeper.off", "UPS.PowerSummary.AudibleAlarmControl", 1},
      {"beeper.on", "UPS.PowerSummary.AudibleAlarmControl", 2},
      {"beeper.mute", "UPS.PowerSummary.AudibleAlarmControl", 3},
      {"beeper.mute", "UPS.BatterySystem.Battery.AudibleAlarmControl", 3},
      {"beeper.disable", "UPS.PowerSummary.AudibleAlarmControl", 1},
      {"beeper.disable", "UPS.BatterySystem.Battery.AudibleAlarmControl", 1},
      {"beeper.enable", "UPS.PowerSummary.AudibleAlarmControl", 2},
      {"beeper.enable", "UPS.BatterySystem.Battery.AudibleAlarmControl", 2},
  };
  for (const auto &entry : COMMANDS) {
    HIDData_t *item = nut_hid_find_object(this->hid_desc_, entry.hid_path);
    if (item == nullptr) {
      continue;
    }
    bool already = false;
    for (const auto &cmd : this->commands_) {
      if (strcmp(cmd.name, entry.name) == 0) {
        already = true;
        break;
      }
    }
    if (already) {
      continue;
    }
    this->commands_.push_back({entry.name, entry.hid_path, entry.value, item});
    ESP_LOGI(TAG, "Mapped command %s -> %s (report 0x%02X)", entry.name, entry.hid_path, item->ReportID);
  }

  // Poll plan: one request per (report ID, report type) covering all
  // resolved items; report length comes from the parsed descriptor.
  auto add_request = [&](const HIDData_t *item) {
    const uint16_t length = static_cast<uint16_t>(this->hid_desc_->replen[item->ReportID]);
    for (auto &request : this->report_requests_) {
      if (request.report_id == item->ReportID && request.report_type == item->Type) {
        return;
      }
    }
    this->report_requests_.push_back({item->ReportID, item->Type, length});
  };
  for (const auto &var : this->vars_) {
    add_request(var.item);
  }
  for (const auto &status : this->bools_) {
    add_request(status.item);
  }

  ESP_LOGI(TAG, "Resolved %u vars, %u status bits, %u reports to poll",
           static_cast<unsigned>(this->vars_.size()), static_cast<unsigned>(this->bools_.size()),
           static_cast<unsigned>(this->report_requests_.size()));
}

void Nut::poll_hid_reports_(usb_host_client_handle_t client, usb_device_handle_t device) {
  this->run_pending_commands_(client, device);
  if (this->report_requests_.empty()) {
    return;
  }

  // Report buffers include the report ID byte, like upstream rbuf.
  uint8_t report_buffer[129]{};
  for (const auto &request : this->report_requests_) {
    size_t actual = 0;
    const uint16_t length = std::min<uint16_t>(128, request.length + 1);
    if (!this->request_hid_report_(client, device, this->hid_interface_number_, request.report_type,
                                   request.report_id, length, report_buffer, &actual) || actual == 0) {
      ESP_LOGD(TAG, "GET_REPORT failed for type=%u id=0x%02X", request.report_type, request.report_id);
      continue;
    }

    // Ensure the first byte is the report ID (GetValue expects it).
    const uint8_t *payload = report_buffer;
    if (report_buffer[0] != request.report_id && actual >= 1) {
      memmove(report_buffer + 1, report_buffer, std::min<size_t>(actual, 128));
      report_buffer[0] = request.report_id;
      actual = std::min<size_t>(actual + 1, 129);
      payload = report_buffer;
    }

    for (auto &var : this->vars_) {
      if (var.item->ReportID != request.report_id || var.item->Type != request.report_type) {
        continue;
      }
      long logical = 0;
      GetValue(payload, var.item, &logical);
      if (var.is_string) {
        if (var.convert == 5) {
          // Internal ABM input: stash the number, publish nothing.
          var.value = logical;
          var.valid = true;
          continue;
        }
        if (var.convert == 4) {
          // test_read_info lookup (values 1-7, 0 = no entry).
          static const char *const TEST_RESULTS[] = {
              nullptr,           // 0
              "Done and passed", // 1
              "Done and warning",
              "Done and error",
              "Aborted",
              "In progress",
              "No test initiated",
              "Test scheduled",  // 7
          };
          if (logical >= 1 && logical <= 7) {
            strlcpy(var.text, TEST_RESULTS[logical], sizeof(var.text));
            var.valid = true;
          }
          continue;
        }
        if (logical > 0 && logical < 255) {
          char text[sizeof(var.text)];
          if (this->get_string_descriptor_(client, device, static_cast<uint8_t>(logical), text, sizeof(text))) {
            strlcpy(var.text, text, sizeof(var.text));
            var.valid = true;
            // Upstream mge-hid builds the model as "<iProduct> <iModel>".
            if (strcmp(var.name, "ups.model") == 0 && text[0] != '\0' &&
                this->device_model_.find(text) == std::string::npos) {
              this->device_model_ += " ";
              this->device_model_ += text;
            }
          }
        }
        continue;
      }
      double value = nut_hid_scale_value(var.item, logical);
      if (var.convert == 1) {
        value -= 273.15;  // kelvin_celsius_conversion
      } else if (var.convert == 2) {
        value /= 3600.0;  // mge_battery_capacity: As -> Ah
      }
      var.value = value;
      var.valid = true;
    }
    for (auto &status : this->bools_) {
      if (status.item->ReportID != request.report_id || status.item->Type != request.report_type) {
        continue;
      }
      long logical = 0;
      GetValue(payload, status.item, &logical);
      status.valid = logical != 0;
    }
  }
}

bool Nut::request_hid_report_(usb_host_client_handle_t client, usb_device_handle_t device, uint8_t interface_number,
                              uint8_t report_type, uint8_t report_id, uint16_t report_length, uint8_t *buffer,
                              size_t *buffer_length) {
  usb_transfer_t *transfer = nullptr;
  const esp_err_t allocation_result =
      usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + report_length, 0, &transfer);
  if (allocation_result != ESP_OK) {
    ESP_LOGD(TAG, "GET_REPORT alloc failed: %s", esp_err_to_name(allocation_result));
    return false;
  }

  TransferContext context{false};
  usb_setup_packet_t setup{};
  setup.bmRequestType = USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_CLASS |
                        USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
  setup.bRequest = 0x01;  // GET_REPORT
  setup.wValue = static_cast<uint16_t>((report_type << 8) | report_id);
  setup.wIndex = interface_number;
  setup.wLength = report_length;

  memset(transfer->data_buffer, 0, transfer->data_buffer_size);
  memcpy(transfer->data_buffer, &setup, sizeof(setup));
  transfer->num_bytes = static_cast<int>(sizeof(usb_setup_packet_t) + setup.wLength);
  transfer->bEndpointAddress = 0;
  transfer->device_handle = device;
  transfer->callback = usb_transfer_callback_;
  transfer->context = &context;

  const esp_err_t submit_result = usb_host_transfer_submit_control(client, transfer);
  if (submit_result != ESP_OK) {
    ESP_LOGD(TAG, "GET_REPORT submit failed: %s", esp_err_to_name(submit_result));
    usb_host_transfer_free(transfer);
    return false;
  }

  for (uint8_t attempts = 0; attempts < 10 && !context.complete; attempts++) {
    usb_host_client_handle_events(client, pdMS_TO_TICKS(50));
  }

  if (!context.complete || transfer->status != USB_TRANSFER_STATUS_COMPLETED) {
    ESP_LOGD(TAG, "GET_REPORT incomplete status=%d complete=%d", transfer->status, context.complete);
    usb_host_transfer_free(transfer);
    return false;
  }

  size_t payload = 0;
  if (transfer->actual_num_bytes > static_cast<int>(sizeof(usb_setup_packet_t))) {
    payload = static_cast<size_t>(transfer->actual_num_bytes - sizeof(usb_setup_packet_t));
  }
  payload = std::min(payload, static_cast<size_t>(report_length));
  memcpy(buffer, transfer->data_buffer + sizeof(usb_setup_packet_t), payload);
  *buffer_length = payload;
  usb_host_transfer_free(transfer);
  return payload > 0;
}

bool Nut::send_hid_report_(usb_host_client_handle_t client, usb_device_handle_t device, const HIDData_t *item,
                           long value) {
  const uint16_t length = static_cast<uint16_t>(this->hid_desc_->replen[item->ReportID] + 1);
  usb_transfer_t *transfer = nullptr;
  if (usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + length, 0, &transfer) != ESP_OK) {
    return false;
  }

  // Build the report: report ID byte, then the field at its bit offset.
  uint8_t *payload = transfer->data_buffer + sizeof(usb_setup_packet_t);
  memset(payload, 0, length);
  payload[0] = item->ReportID;
  SetValue(item, payload, value);

  TransferContext context{false};
  usb_setup_packet_t setup{};
  setup.bmRequestType = USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_CLASS |
                        USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
  setup.bRequest = 0x09;  // SET_REPORT
  setup.wValue = static_cast<uint16_t>((HID_REPORT_TYPE_FEATURE << 8) | item->ReportID);
  setup.wIndex = this->hid_interface_number_;
  setup.wLength = length;

  memcpy(transfer->data_buffer, &setup, sizeof(setup));
  transfer->num_bytes = static_cast<int>(sizeof(usb_setup_packet_t) + length);
  transfer->bEndpointAddress = 0;
  transfer->device_handle = device;
  transfer->callback = usb_transfer_callback_;
  transfer->context = &context;

  const esp_err_t submit_result = usb_host_transfer_submit_control(client, transfer);
  if (submit_result != ESP_OK) {
    usb_host_transfer_free(transfer);
    return false;
  }
  for (uint8_t attempts = 0; attempts < 10 && !context.complete; attempts++) {
    usb_host_client_handle_events(client, pdMS_TO_TICKS(50));
  }
  const bool ok = context.complete && transfer->status == USB_TRANSFER_STATUS_COMPLETED;
  usb_host_transfer_free(transfer);
  return ok;
}

void Nut::run_pending_commands_(usb_host_client_handle_t client, usb_device_handle_t device) {
  const int index = this->pending_command_;
  if (index < 0 || index >= static_cast<int>(this->commands_.size())) {
    return;
  }
  const auto &command = this->commands_[index];
  ESP_LOGI(TAG, "Executing command %s (value %ld)", command.name, command.value);
  this->command_result_ = this->send_hid_report_(client, device, command.item, command.value) ? 0 : 1;
  this->pending_command_ = -1;
}

void Nut::execute_instant_command_(int client_fd, const std::string &command) {
  for (size_t i = 0; i < this->commands_.size(); i++) {
    if (command == this->commands_[i].name) {
      this->command_result_ = 0;
      this->pending_command_ = static_cast<int>(i);
      // Wait for the USB task to pick it up (runs on the next poll cycle).
      for (uint8_t attempts = 0; attempts < 40 && this->pending_command_ >= 0; attempts++) {
        vTaskDelay(pdMS_TO_TICKS(100));
      }
      if (this->pending_command_ >= 0) {
        this->pending_command_ = -1;
        this->send_line_(client_fd, "ERR TEMPORARY-FAILURE");
        return;
      }
      this->send_line_(client_fd, this->command_result_ == 0 ? "OK" : "ERR TEMPORARY-FAILURE");
      return;
    }
  }
  this->send_line_(client_fd, "ERR CMD-NOT-SUPPORTED");
}

const ResolvedVar *Nut::find_var_(const char *name) const {
  if (strcmp(name, "battery.charger.status") == 0) {
    return this->charger_status_var_();
  }
  for (const auto &var : this->vars_) {
    if (strcmp(var.name, name) == 0 && var.valid && var.convert != 5) {
      return &var;
    }
  }
  return nullptr;
}

const ResolvedVar *Nut::find_raw_var_(const char *name) const {
  for (const auto &var : this->vars_) {
    if (strcmp(var.name, name) == 0 && var.valid) {
      return &var;
    }
  }
  return nullptr;
}

// Synthesize battery.charger.status from the ABM inputs, following
// eaton_abm_* in upstream mge-hid.c: ABMEnable gates publication; Mode and
// Status are alternative data paths with different value tables.
const ResolvedVar *Nut::charger_status_var_() const {
  const auto *abm = this->find_raw_var_("battery.charger.abm.status");
  if (abm == nullptr || abm->value != 1) {  // ABM not enabled: unpublished upstream
    return nullptr;
  }
  const auto *mode = this->find_raw_var_("battery.charger.mode.status");
  const auto *status = this->find_raw_var_("battery.charger.type.status");

  const char *text = nullptr;
  // Prefer the Status path when it reports a known state (upstream picks
  // whichever path first delivered data; Status exists on newer firmware).
  if (status != nullptr) {
    switch (static_cast<int>(status->value)) {
      case 1: text = "charging"; break;
      case 2: text = "floating"; break;
      case 3: text = "resting"; break;
      case 4: text = "discharging"; break;
      case 6: text = "off"; break;
      default: break;
    }
  }
  if (text == nullptr && mode != nullptr) {
    switch (static_cast<int>(mode->value)) {
      case 1: text = "charging"; break;
      case 2: text = "discharging"; break;
      case 3: text = "floating"; break;
      case 4: text = "resting"; break;
      case 6: text = "off"; break;
      default: break;
    }
  }
  if (text == nullptr) {
    return nullptr;
  }
  strlcpy(this->charger_status_cache_.text, text, sizeof(this->charger_status_cache_.text));
  this->charger_status_cache_.valid = true;
  return &this->charger_status_cache_;
}

std::string Nut::ups_status_() const {
  std::string status;
  for (const auto &entry : this->bools_) {
    const char *token = entry.valid ? entry.status_set : entry.status_clear;
    if (token == nullptr) {
      continue;
    }
    // Keep first occurrence of each token (matches upstream dedup).
    if (status.find(token) != std::string::npos) {
      continue;
    }
    if (!status.empty()) {
      status += ' ';
    }
    status += token;
  }
  if (status.empty()) {
    return "WAIT";
  }
  return status;
}

void Nut::get_var_value_(int client_fd, const std::string &variable) const {
  if (variable == "device.mfr") {
    this->send_line_(client_fd, "VAR " + this->ups_name_ + " device.mfr \"" + this->device_mfr_ + "\"");
  } else if (variable == "device.model") {
    this->send_line_(client_fd, "VAR " + this->ups_name_ + " device.model \"" + this->device_model_ + "\"");
  } else if (variable == "device.serial") {
    if (this->device_serial_.empty()) {
      this->send_line_(client_fd, "ERR VAR-NOT-SUPPORTED");
      return;
    }
    this->send_line_(client_fd, "VAR " + this->ups_name_ + " device.serial \"" + this->device_serial_ + "\"");
  } else if (variable == "device.type") {
    this->send_line_(client_fd, "VAR " + this->ups_name_ + " device.type \"ups\"");
  } else if (variable == "driver.name") {
    this->send_line_(client_fd, "VAR " + this->ups_name_ + " driver.name \"usbhid-ups\"");
  } else if (variable == "ups.status") {
    this->send_line_(client_fd, "VAR " + this->ups_name_ + " ups.status \"" + this->ups_status_() + "\"");
  } else {
    const ResolvedVar *var = this->find_var_(variable.c_str());
    if (var == nullptr) {
      this->send_line_(client_fd, "ERR VAR-NOT-SUPPORTED");
      return;
    }
    if (var->is_string) {
      this->send_line_(client_fd, "VAR " + this->ups_name_ + " " + variable + " \"" + var->text + "\"");
      return;
    }
    char value_text[32];
    snprintf(value_text, sizeof(value_text), var->format, var->value);
    this->send_line_(client_fd,
                     "VAR " + this->ups_name_ + " " + variable + " \"" + value_text + "\"");
  }
}

void Nut::nut_server_task_(void *argument) {
  auto *component = static_cast<Nut *>(argument);

  const int server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
  if (server_fd < 0) {
    ESP_LOGE(TAG, "Cannot create NUT socket");
    vTaskDelete(nullptr);
    return;
  }

  int reuse_address = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse_address, sizeof(reuse_address));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(component->port_);
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(server_fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
      listen(server_fd, 4) != 0) {
    ESP_LOGE(TAG, "Cannot listen on NUT port %u", component->port_);
    close(server_fd);
    vTaskDelete(nullptr);
    return;
  }

  ESP_LOGI(TAG, "NUT server listening on TCP/%u", component->port_);

  // Single-task select() multiplexing: one thread owns all sockets, so
  // long-lived clients (upsmon) coexist with one-shot ones (upsc) without
  // concurrent lwIP/W5500 transmit paths, which crash on this stack.
  struct ClientState {
    int fd{-1};
    bool authenticated{false};
    std::string pending;
  };
  ClientState clients[4];
  std::array<char, NUT_LINE_LENGTH> buffer{};

  while (true) {
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(server_fd, &read_fds);
    int max_fd = server_fd;
    for (auto &client : clients) {
      if (client.fd >= 0) {
        FD_SET(client.fd, &read_fds);
        max_fd = std::max(max_fd, client.fd);
      }
    }

    if (select(max_fd + 1, &read_fds, nullptr, nullptr, nullptr) < 0) {
      continue;
    }

    if (FD_ISSET(server_fd, &read_fds)) {
      const int client_fd = accept(server_fd, nullptr, nullptr);
      if (client_fd >= 0) {
        bool accepted = false;
        for (auto &client : clients) {
          if (client.fd < 0) {
            client.fd = client_fd;
            client.authenticated = false;
            client.pending.clear();
            accepted = true;
            ESP_LOGI(TAG, "NUT client connected (fd=%d)", client_fd);
            break;
          }
        }
        if (!accepted) {
          ESP_LOGW(TAG, "NUT connection slots full; dropping fd=%d", client_fd);
          close(client_fd);
        }
      }
    }

    for (auto &client : clients) {
      if (client.fd < 0 || !FD_ISSET(client.fd, &read_fds)) {
        continue;
      }
      const ssize_t length = recv(client.fd, buffer.data(), buffer.size() - 1, 0);
      if (length <= 0) {
        ESP_LOGI(TAG, "NUT client disconnected (fd=%d)", client.fd);
        close(client.fd);
        client.fd = -1;
        continue;
      }
      buffer[static_cast<size_t>(length)] = '\0';
      client.pending.append(buffer.data(), static_cast<size_t>(length));
      size_t line_end = 0;
      while ((line_end = client.pending.find('\n')) != std::string::npos) {
        const std::string line = trim_crlf(client.pending.substr(0, line_end));
        client.pending.erase(0, line_end + 1);
        if (!line.empty()) {
          ESP_LOGD(TAG, "NUT fd=%d << %s", client.fd, line.c_str());
          component->handle_nut_command_(client.fd, line, &client.authenticated);
        }
      }
    }
  }
}

bool Nut::is_ups_name_(const std::string &name) const {
  return name == this->ups_name_;
}

void Nut::send_line_(int client_fd, const std::string &line) const {
  ESP_LOGD(TAG, "NUT fd=%d >> %s", client_fd, line.c_str());
  const std::string message = line + "\n";
  send(client_fd, message.c_str(), message.size(), 0);
}

void Nut::handle_nut_command_(int client_fd, const std::string &line, bool *authenticated) {
  const std::string command = first_word(line);
  const std::string arguments = after_first_word(line);

  if (command == "VER") {
    this->send_line_(client_fd, "Network UPS Tools upsd 2.8.2-esp-home");
  } else if (command == "PING") {
    this->send_line_(client_fd, "PONG");
  } else if (command == "USERNAME") {
    *authenticated = arguments == this->username_;
    this->send_line_(client_fd, *authenticated ? "OK" : "ERR ACCESS-DENIED");
  } else if (command == "PASSWORD") {
    *authenticated = *authenticated && arguments == this->password_;
    this->send_line_(client_fd, *authenticated ? "OK" : "ERR ACCESS-DENIED");
  } else if (command == "LOGIN") {
    // Like upsd: LOGIN just registers interest in the UPS; credentials
    // only gate privileged commands (INSTCMD), not data access.
    this->send_line_(client_fd, this->is_ups_name_(arguments) ? "OK" : "ERR UNKNOWN-UPS");
  } else if (command == "LOGOUT") {
    *authenticated = false;
    this->send_line_(client_fd, "OK Goodbye");
  } else if (line == "DUMPDESC") {
    // Debug: stream the raw HID report descriptor as hex lines.
    char line[56];
    for (size_t off = 0; off < this->raw_desc_len_; off += 16) {
      size_t pos = snprintf(line, sizeof(line), "%04x:", static_cast<unsigned>(off));
      const size_t n = std::min<size_t>(16, this->raw_desc_len_ - off);
      for (size_t i = 0; i < n && pos + 4 < sizeof(line); i++) {
        pos += snprintf(line + pos, sizeof(line) - pos, " %02x", this->raw_desc_[off + i]);
      }
      this->send_line_(client_fd, line);
    }
    this->send_line_(client_fd, "END");
  } else if (line == "LIST UPS") {
    this->send_line_(client_fd, "BEGIN LIST UPS");
    this->send_line_(client_fd, "UPS " + this->ups_name_ + " \"" + this->device_model_ + "\"");
    this->send_line_(client_fd, "END LIST UPS");
  } else if (line == "LIST VAR " + this->ups_name_) {
    this->send_line_(client_fd, "BEGIN LIST VAR " + this->ups_name_);
    this->get_var_value_(client_fd, "device.mfr");
    this->get_var_value_(client_fd, "device.model");
    this->get_var_value_(client_fd, "device.serial");
    this->get_var_value_(client_fd, "device.type");
    this->get_var_value_(client_fd, "driver.name");
    this->get_var_value_(client_fd, "ups.status");
    this->get_var_value_(client_fd, "battery.charger.status");
    for (const auto &var : this->vars_) {
      if (var.valid && var.convert != 5) {
        this->get_var_value_(client_fd, var.name);
      }
    }
    this->send_line_(client_fd, "END LIST VAR " + this->ups_name_);
  } else if (command == "GET") {
    const std::string expected_prefix = "VAR " + this->ups_name_ + " ";
    if (arguments.rfind(expected_prefix, 0) != 0) {
      this->send_line_(client_fd, "ERR VAR-NOT-SUPPORTED");
      return;
    }
    this->get_var_value_(client_fd, arguments.substr(expected_prefix.size()));
  } else if (line == "LIST CMD " + this->ups_name_) {
    this->send_line_(client_fd, "BEGIN LIST CMD " + this->ups_name_);
    for (const auto &cmd : this->commands_) {
      this->send_line_(client_fd, "CMD " + this->ups_name_ + " " + cmd.name);
    }
    this->send_line_(client_fd, "END LIST CMD " + this->ups_name_);
  } else if (command == "INSTCMD") {
    if (!*authenticated) {
      this->send_line_(client_fd, "ERR ACCESS-DENIED");
      return;
    }
    // INSTCMD <upsname> <command>
    const std::string expected_prefix = this->ups_name_ + " ";
    if (arguments.rfind(expected_prefix, 0) != 0) {
      this->send_line_(client_fd, "ERR CMD-NOT-SUPPORTED");
      return;
    }
    this->execute_instant_command_(client_fd, arguments.substr(expected_prefix.size()));
  } else {
    this->send_line_(client_fd, "ERR UNKNOWN-COMMAND");
  }
}

}  // namespace nut
}  // namespace esphome
