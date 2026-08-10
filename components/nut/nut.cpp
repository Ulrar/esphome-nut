#include "nut.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

#include "esphome/core/log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "usb/usb_host.h"
#include "usb/usb_types_ch9.h"

namespace esphome {
namespace nut {

static const char *const TAG = "nut";
static constexpr size_t NUT_LINE_LENGTH = 256;
static constexpr uint8_t USB_HID_DESCRIPTOR_TYPE = 0x21;
static constexpr uint8_t USB_HID_REPORT_DESCRIPTOR_TYPE = 0x22;
static constexpr uint16_t MAX_HID_REPORT_DESCRIPTOR_LENGTH = 2048;
static constexpr uint8_t HID_REPORT_TYPE_INPUT = 1;
static constexpr uint8_t HID_REPORT_TYPE_FEATURE = 3;

struct UsbClientContext {
  Nut *component;
  usb_host_client_handle_t client;
};

struct TransferContext {
  volatile bool complete;
};

static uint8_t preferred_collection_mask(UpsSignal signal) {
  switch (signal) {
    case UpsSignal::INPUT_VOLTAGE:
    case UpsSignal::INPUT_CURRENT:
    case UpsSignal::INPUT_FREQUENCY:
      return COLLECTION_INPUT;
    case UpsSignal::OUTPUT_VOLTAGE:
    case UpsSignal::OUTPUT_CURRENT:
    case UpsSignal::OUTPUT_FREQUENCY:
    case UpsSignal::OUTPUT_APPARENT_POWER:
    case UpsSignal::OUTPUT_ACTIVE_POWER:
    case UpsSignal::LOAD_PERCENT:
      return COLLECTION_OUTPUT;
    case UpsSignal::BATTERY_CHARGE:
    case UpsSignal::RUNTIME_SECONDS:
    case UpsSignal::BATTERY_VOLTAGE:
    case UpsSignal::AC_PRESENT:
    case UpsSignal::CHARGING:
    case UpsSignal::DISCHARGING:
      return COLLECTION_POWER_SUMMARY;
    case UpsSignal::NONE:
      return 0;
  }
  return 0;
}

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

  BaseType_t created = xTaskCreate(
      nut_server_task_, "nut_server", 6144, this, 4, nullptr);
  if (created != pdPASS) {
    ESP_LOGE(TAG, "Unable to start the NUT server task");
    this->mark_failed();
  }
}

void Nut::dump_config() {
  ESP_LOGCONFIG(TAG, "NUT:");
  ESP_LOGCONFIG(TAG, "  UPS name: %s", this->ups_name_.c_str());
  ESP_LOGCONFIG(TAG, "  Description: %s", this->description_.c_str());
  ESP_LOGCONFIG(TAG, "  NUT TCP port: %u", this->port_);
  ESP_LOGCONFIG(TAG, "  USB host: %s", this->usb_host_started_ ? "started" : "not started");
}

void Nut::start_usb_host_() {
  BaseType_t daemon_created = xTaskCreate(
      usb_daemon_task_, "eaton_usb", 4096, this, 5, nullptr);
  BaseType_t client_created = xTaskCreate(
      usb_client_task_, "eaton_usb_client", 4096, this, 5, nullptr);

  if (daemon_created != pdPASS || client_created != pdPASS) {
    ESP_LOGE(TAG, "Unable to start USB host tasks");
    this->mark_failed();
    return;
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

  ESP_LOGI(TAG, "USB host is ready; connect the Eaton USB-B port to the S3 host port");
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
  if (event->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
    context->component->log_usb_device_(context->client, event->new_dev.address);
  }
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
    const uint8_t device_address = addresses[index];
    usb_device_handle_t device = nullptr;
    if (usb_host_device_open(client, device_address, &device) == ESP_OK) {
      usb_host_device_close(client, device);
      this->log_usb_device_(client, device_address);
    }
  }
}

void Nut::log_usb_device_(usb_host_client_handle_t client, uint8_t device_address) {
  usb_device_handle_t device = nullptr;
  const esp_err_t open_result = usb_host_device_open(client, device_address, &device);
  if (open_result != ESP_OK) {
    ESP_LOGW(TAG, "Unable to open USB device %u: %s", device_address, esp_err_to_name(open_result));
    return;
  }

  const usb_device_desc_t *descriptor = nullptr;
  const esp_err_t descriptor_result = usb_host_get_device_descriptor(device, &descriptor);
  if (descriptor_result != ESP_OK || descriptor == nullptr) {
    ESP_LOGW(TAG, "Unable to read USB device descriptor: %s", esp_err_to_name(descriptor_result));
    usb_host_device_close(client, device);
    return;
  }

  ESP_LOGI(TAG, "USB device %u: VID=%04X PID=%04X configurations=%u", device_address, descriptor->idVendor,
           descriptor->idProduct, descriptor->bNumConfigurations);

  const usb_config_desc_t *config_descriptor = nullptr;
  const esp_err_t config_result = usb_host_get_active_config_descriptor(device, &config_descriptor);
  if (config_result != ESP_OK || config_descriptor == nullptr) {
    ESP_LOGW(TAG, "Unable to read active USB configuration descriptor: %s", esp_err_to_name(config_result));
    usb_host_device_close(client, device);
    return;
  }

  const uint8_t *config_data = reinterpret_cast<const uint8_t *>(config_descriptor);
  const uint16_t config_length = config_descriptor->wTotalLength;
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
    if (!this->report_descriptor_captured_ || this->discovered_device_address_ != device_address) {
      this->discovered_device_address_ = device_address;
      this->hid_interface_number_ = hid_interface;
      this->report_descriptor_captured_ =
          this->capture_hid_report_descriptor_(client, device, hid_interface, report_length);
    }
    if (this->report_descriptor_captured_) {
      this->poll_hid_reports_(client, device);
    }
  }
  usb_host_device_close(client, device);
}

void Nut::usb_transfer_callback_(usb_transfer_t *transfer) {
  auto *context = static_cast<TransferContext *>(transfer->context);
  context->complete = true;
}

bool Nut::capture_hid_report_descriptor_(usb_host_client_handle_t client, usb_device_handle_t device,
                                         uint8_t interface_number, uint16_t report_length) {
  usb_transfer_t *transfer = nullptr;
  const esp_err_t allocation_result =
      usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + report_length, 0, &transfer);
  if (allocation_result != ESP_OK) {
    ESP_LOGW(TAG, "Unable to allocate HID report transfer: %s", esp_err_to_name(allocation_result));
    return false;
  }

  TransferContext context{false};
  usb_setup_packet_t setup{};
  setup.bmRequestType = USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_STANDARD |
                        USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
  setup.bRequest = USB_B_REQUEST_GET_DESCRIPTOR;
  setup.wValue = static_cast<uint16_t>(USB_HID_REPORT_DESCRIPTOR_TYPE << 8);
  setup.wIndex = interface_number;
  setup.wLength = report_length;

  memset(transfer->data_buffer, 0, transfer->data_buffer_size);
  memcpy(transfer->data_buffer, &setup, sizeof(setup));
  transfer->num_bytes = static_cast<int>(sizeof(usb_setup_packet_t) + report_length);
  transfer->bEndpointAddress = 0;
  transfer->device_handle = device;
  transfer->callback = usb_transfer_callback_;
  transfer->context = &context;
  const esp_err_t submit_result = usb_host_transfer_submit_control(client, transfer);
  if (submit_result != ESP_OK) {
    ESP_LOGW(TAG, "Unable to request HID report descriptor: %s", esp_err_to_name(submit_result));
    usb_host_transfer_free(transfer);
    return false;
  }

  for (uint8_t attempts = 0; attempts < 20 && !context.complete; attempts++) {
    usb_host_client_handle_events(client, pdMS_TO_TICKS(100));
  }

  const bool complete = context.complete && transfer->status == USB_TRANSFER_STATUS_COMPLETED;
  if (!complete) {
    ESP_LOGW(TAG, "HID report descriptor transfer did not complete: status=%d", transfer->status);
    usb_host_transfer_free(transfer);
    return false;
  }

  const uint8_t *report = transfer->data_buffer + sizeof(usb_setup_packet_t);
  size_t transferred_data_bytes = 0;
  if (transfer->actual_num_bytes > static_cast<int>(sizeof(usb_setup_packet_t))) {
    transferred_data_bytes = static_cast<size_t>(transfer->actual_num_bytes - sizeof(usb_setup_packet_t));
  }
  const size_t actual_length = std::min(transferred_data_bytes, static_cast<size_t>(report_length));
  ESP_LOGI(TAG, "HID report descriptor: interface=%u length=%u", interface_number, actual_length);
  for (size_t offset = 0; offset < actual_length; offset += 16) {
    char line[64];
    size_t position = 0;
    position += snprintf(line + position, sizeof(line) - position, "HID %03u:", offset);
    const size_t line_length = std::min(static_cast<size_t>(16), actual_length - offset);
    for (size_t index = 0; index < line_length && position + 4 < sizeof(line); index++) {
      position += snprintf(line + position, sizeof(line) - position, " %02X", report[offset + index]);
    }
    ESP_LOGI(TAG, "%s", line);
  }

  const bool parsed = this->parse_hid_report_descriptor_(report, actual_length);
  ESP_LOGI(TAG, "HID mapping fields=%u reports=%u", this->report_field_count_, this->report_request_count_);

  usb_host_transfer_free(transfer);
  return actual_length > 0 && parsed;
}

bool Nut::parse_hid_report_descriptor_(const uint8_t *descriptor, size_t descriptor_length) {
  this->report_field_count_ = 0;
  this->report_request_count_ = 0;
  this->has_ac_present_field_ = false;

  struct {
    uint16_t usage_page{0};
    int32_t logical_min{0};
    int32_t logical_max{0};
    uint32_t report_size{0};
    uint32_t report_count{0};
    uint8_t report_id{0};
    int8_t exponent{0};
    uint32_t unit{0};
  } globals;
  uint8_t collection_stack[8]{};
  uint8_t collection_depth = 0;
  uint32_t local_usages[16]{};
  uint8_t local_usage_count = 0;
  uint32_t local_usage_min = 0;
  uint32_t local_usage_max = 0;
  bool has_usage_range = false;
  uint16_t bit_offsets[4][256]{};

  auto reset_local = [&]() {
    local_usage_count = 0;
    has_usage_range = false;
    local_usage_min = 0;
    local_usage_max = 0;
  };

  for (size_t index = 0; index < descriptor_length;) {
    const uint8_t prefix = descriptor[index++];
    if (prefix == 0xFE) {
      if (index + 1 >= descriptor_length) {
        break;
      }
      const uint8_t item_size = descriptor[index++];
      index++;  // long-item tag
      index += std::min(static_cast<size_t>(item_size), descriptor_length - index);
      continue;
    }

    uint8_t item_size = prefix & 0x03;
    if (item_size == 3) {
      item_size = 4;
    }
    const uint8_t item_type = (prefix >> 2) & 0x03;
    const uint8_t item_tag = (prefix >> 4) & 0x0F;
    if (index + item_size > descriptor_length) {
      break;
    }

    uint32_t value = 0;
    for (uint8_t b = 0; b < item_size; b++) {
      value |= static_cast<uint32_t>(descriptor[index + b]) << (8 * b);
    }
    const bool signed_value = item_size > 0 && ((descriptor[index + item_size - 1] & 0x80) != 0);
    int32_t svalue = static_cast<int32_t>(value);
    if (signed_value && item_size < 4) {
      svalue |= static_cast<int32_t>(~0u << (item_size * 8));
    }
    index += item_size;

    if (item_type == 1) {  // global
      if (item_tag == 0x0) {
        globals.usage_page = static_cast<uint16_t>(value);
      } else if (item_tag == 0x1) {
        globals.logical_min = svalue;
      } else if (item_tag == 0x2) {
        globals.logical_max = svalue;
      } else if (item_tag == 0x7) {
        globals.report_size = value;
      } else if (item_tag == 0x8) {
        globals.report_id = static_cast<uint8_t>(value);
      } else if (item_tag == 0x9) {
        globals.report_count = value;
      } else if (item_tag == 0x5) {
        if (item_size == 1 && value <= 0x0F) {
          globals.exponent = (value & 0x08) ? static_cast<int8_t>(value) - 16 : static_cast<int8_t>(value);
        } else {
          globals.exponent = static_cast<int8_t>(svalue);
        }
      } else if (item_tag == 0x6) {
        globals.unit = value;
      }
      continue;
    }

    if (item_type == 2) {  // local
      if (item_tag == 0x0 && local_usage_count < sizeof(local_usages) / sizeof(local_usages[0])) {
        local_usages[local_usage_count++] = value;
      } else if (item_tag == 0x1) {
        local_usage_min = value;
        has_usage_range = true;
      } else if (item_tag == 0x2) {
        local_usage_max = value;
        has_usage_range = true;
      }
      continue;
    }

    if (item_type != 0) {
      continue;
    }

    if (item_tag == 0xA) {  // collection
      if (collection_depth < sizeof(collection_stack)) {
        uint16_t usage = 0;
        if (local_usage_count > 0) {
            usage = static_cast<uint16_t>(local_usages[local_usage_count - 1] & 0xFFFF);
        } else if (has_usage_range) {
            usage = static_cast<uint16_t>(local_usage_min & 0xFFFF);
        }
        collection_stack[collection_depth++] = static_cast<uint8_t>(usage);
      }
      reset_local();
      continue;
    }

    if (item_tag == 0xC) {  // end collection
      if (collection_depth > 0) {
        collection_depth--;
      }
      continue;
    }

    if (item_tag != 0x8 && item_tag != 0xB) {  // input/feature only
      reset_local();
      continue;
    }

    const uint8_t report_type = item_tag == 0x8 ? HID_REPORT_TYPE_INPUT : HID_REPORT_TYPE_FEATURE;
    const bool is_constant = (value & 0x01) != 0;
    const uint32_t count = std::max<uint32_t>(1, globals.report_count);
    const uint32_t bits = globals.report_size;
    uint8_t collection_mask = 0;
    for (uint8_t depth = 0; depth < collection_depth; depth++) {
      if (collection_stack[depth] == 0x1A) {
        collection_mask |= COLLECTION_INPUT;
      } else if (collection_stack[depth] == 0x1C) {
        collection_mask |= COLLECTION_OUTPUT;
      } else if (collection_stack[depth] == 0x24) {
        collection_mask |= COLLECTION_POWER_SUMMARY;
      } else if (collection_stack[depth] == 0x10) {
        collection_mask |= COLLECTION_BATTERY_SYSTEM;
      }
    }

    for (uint32_t item_index = 0; item_index < count; item_index++) {
      const uint32_t usage_value =
          item_index < local_usage_count
              ? local_usages[item_index]
              : (has_usage_range ? (local_usage_min + item_index) : 0);
      uint16_t usage_page = globals.usage_page;
      uint16_t usage = static_cast<uint16_t>(usage_value & 0xFFFF);
      if (usage_value > 0xFFFF) {
        usage_page = static_cast<uint16_t>((usage_value >> 16) & 0xFFFF);
      }
      const UpsSignal signal = this->driver_.classify_field(usage_page, usage, collection_mask);
      const uint16_t offset = bit_offsets[report_type][globals.report_id];
      bit_offsets[report_type][globals.report_id] += bits;
      const bool ignore_constant = is_constant && report_type == HID_REPORT_TYPE_INPUT;
      if (ignore_constant || signal == UpsSignal::NONE || bits == 0 || this->report_field_count_ >= 48 || bits > 32) {
        continue;
      }
      this->report_fields_[this->report_field_count_++] = {
          globals.report_id,
          report_type,
          offset,
          static_cast<uint8_t>(bits),
          globals.logical_min < 0,
          globals.exponent,
          globals.unit,
          globals.logical_min,
          globals.logical_max,
          collection_mask,
          signal,
      };
      ESP_LOGD(TAG,
               "Mapped usage %04X/%04X -> signal=%u report(type=%u id=0x%02X off=%u bits=%u exp=%d unit=%08X mask=0x%02X)",
               usage_page, usage, static_cast<unsigned>(signal), report_type, globals.report_id, offset,
               static_cast<unsigned>(bits), static_cast<int>(globals.exponent), static_cast<unsigned>(globals.unit),
               collection_mask);
      if (signal == UpsSignal::AC_PRESENT) {
        this->has_ac_present_field_ = true;
      }

      bool found = false;
      for (uint8_t request_index = 0; request_index < this->report_request_count_; request_index++) {
        auto &request = this->report_requests_[request_index];
        if (request.report_id == globals.report_id && request.report_type == report_type) {
          request.required_bits = std::max<uint16_t>(request.required_bits, static_cast<uint16_t>(offset + bits));
          found = true;
          break;
        }
      }
      if (!found && this->report_request_count_ < 16) {
        this->report_requests_[this->report_request_count_++] = {
            globals.report_id,
            report_type,
            static_cast<uint16_t>(offset + bits),
        };
      }
    }
    reset_local();
  }

  if (this->report_field_count_ == 0) {
    ESP_LOGW(TAG, "No UPS-relevant HID fields were mapped from descriptor");
  }

  if (this->report_field_count_ > 0) {
    ReportFieldMapping selected[48]{};
    uint8_t selected_count = 0;
    constexpr uint8_t signal_limit = static_cast<uint8_t>(UpsSignal::DISCHARGING) + 1;
    int16_t best_index_for_signal[signal_limit];
    for (uint8_t i = 0; i < signal_limit; i++) {
      best_index_for_signal[i] = -1;
    }

    // Pass 1: strict path-style selection using collection ancestry.
    for (uint8_t i = 0; i < this->report_field_count_; i++) {
      const auto &field = this->report_fields_[i];
      const uint8_t signal_index = static_cast<uint8_t>(field.signal);
      if (signal_index >= signal_limit || field.signal == UpsSignal::NONE) {
        continue;
      }
      const uint8_t preferred = preferred_collection_mask(field.signal);
      if (preferred != 0 && (field.collection_mask & preferred) != 0 && best_index_for_signal[signal_index] < 0) {
        best_index_for_signal[signal_index] = i;
      }
    }

    // Pass 2 fallback: first descriptor-mapped field per signal.
    for (uint8_t i = 0; i < this->report_field_count_; i++) {
      const auto &field = this->report_fields_[i];
      const uint8_t signal_index = static_cast<uint8_t>(field.signal);
      if (signal_index >= signal_limit || field.signal == UpsSignal::NONE) {
        continue;
      }
      if (best_index_for_signal[signal_index] < 0) {
        best_index_for_signal[signal_index] = i;
      }
    }

    for (uint8_t signal_index = 0; signal_index < signal_limit; signal_index++) {
      const int16_t chosen = best_index_for_signal[signal_index];
      if (chosen < 0) {
        continue;
      }
      selected[selected_count++] = this->report_fields_[chosen];
    }

    this->report_field_count_ = selected_count;
    for (uint8_t i = 0; i < selected_count; i++) {
      this->report_fields_[i] = selected[i];
    }
  }

  this->report_request_count_ = 0;
  for (uint8_t field_index = 0; field_index < this->report_field_count_; field_index++) {
    const auto &field = this->report_fields_[field_index];
    const uint16_t required_bits = static_cast<uint16_t>(field.bit_offset + field.bit_size);
    bool found = false;
    for (uint8_t request_index = 0; request_index < this->report_request_count_; request_index++) {
      auto &request = this->report_requests_[request_index];
      if (request.report_id == field.report_id && request.report_type == field.report_type) {
        request.required_bits = std::max<uint16_t>(request.required_bits, required_bits);
        found = true;
        break;
      }
    }
    if (!found && this->report_request_count_ < 16) {
      this->report_requests_[this->report_request_count_++] = {field.report_id, field.report_type, required_bits};
    }
  }

  if (this->report_request_count_ == 0) {
    ESP_LOGW(TAG, "No HID reports selected for polling");
  } else {
    for (uint8_t request_index = 0; request_index < this->report_request_count_; request_index++) {
      const auto &request = this->report_requests_[request_index];
      ESP_LOGD(TAG, "Polling plan: type=%u id=0x%02X bytes=%u", request.report_type, request.report_id,
               static_cast<unsigned>((request.required_bits + 7) / 8));
    }
  }

  return this->report_field_count_ > 0 && this->report_request_count_ > 0;
}

void Nut::poll_hid_reports_(usb_host_client_handle_t client, usb_device_handle_t device) {
  if (this->report_request_count_ == 0) {
    return;
  }
  uint8_t report_buffer[128]{};
  bool any_ok = false;
  for (uint8_t request_index = 0; request_index < this->report_request_count_; request_index++) {
    const auto &request = this->report_requests_[request_index];
    const uint16_t bytes = static_cast<uint16_t>(std::min<uint16_t>(127, (request.required_bits + 7) / 8));
    size_t actual = 0;
    if (!this->request_hid_report_(client, device, this->hid_interface_number_, request.report_type, request.report_id, bytes,
                                   report_buffer, &actual)) {
      ESP_LOGD(TAG, "GET_REPORT failed for type=%u id=0x%02X", request.report_type, request.report_id);
      continue;
    }
    ESP_LOGD(TAG, "GET_REPORT ok type=%u id=0x%02X bytes=%u", request.report_type, request.report_id,
             static_cast<unsigned>(actual));
    any_ok = true;
    this->apply_report_data_(request.report_type, request.report_id, report_buffer, actual);
  }

  if (any_ok && !this->has_ac_present_field_) {
    this->driver_.set_ac_present(true);
  }
}

bool Nut::request_hid_report_(usb_host_client_handle_t client, usb_device_handle_t device, uint8_t interface_number,
                              uint8_t report_type, uint8_t report_id, uint16_t report_length, uint8_t *buffer,
                              size_t *buffer_length) {
  usb_transfer_t *transfer = nullptr;
  const esp_err_t allocation_result =
      usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + report_length + 1, 0, &transfer);
  if (allocation_result != ESP_OK) {
    ESP_LOGD(TAG, "GET_REPORT alloc failed: %s", esp_err_to_name(allocation_result));
    return false;
  }

  TransferContext context{false};
  usb_setup_packet_t setup{};
  setup.bmRequestType = USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_CLASS | USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
  setup.bRequest = 0x01;  // GET_REPORT
  setup.wValue = static_cast<uint16_t>((report_type << 8) | report_id);
  setup.wIndex = interface_number;
  setup.wLength = report_length + 1;

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
  payload = std::min(payload, static_cast<size_t>(report_length + 1));
  memcpy(buffer, transfer->data_buffer + sizeof(usb_setup_packet_t), payload);
  *buffer_length = payload;
  usb_host_transfer_free(transfer);
  return payload > 0;
}

uint64_t Nut::extract_bits_(const uint8_t *data, size_t data_length, uint16_t bit_offset, uint8_t bit_size) {
  uint64_t value = 0;
  for (uint8_t bit = 0; bit < bit_size; bit++) {
    const uint16_t absolute = static_cast<uint16_t>(bit_offset + bit);
    const size_t byte_index = absolute / 8;
    if (byte_index >= data_length) {
      break;
    }
    const uint8_t mask = static_cast<uint8_t>(1u << (absolute % 8));
    if ((data[byte_index] & mask) != 0) {
      value |= static_cast<uint64_t>(1) << bit;
    }
  }
  return value;
}

float Nut::apply_exponent_(float value, int8_t exponent) {
  if (exponent == 0) {
    return value;
  }
  return value * std::pow(10.0f, static_cast<float>(exponent));
}

void Nut::apply_report_data_(uint8_t report_type, uint8_t report_id, const uint8_t *report, size_t report_length) {
  size_t payload_offset = 0;
  if (report_length > 1 && report[0] == report_id) {
    payload_offset = 1;
  }
  const uint8_t *payload = report + payload_offset;
  const size_t payload_length = report_length - payload_offset;

  for (uint8_t field_index = 0; field_index < this->report_field_count_; field_index++) {
    const auto &field = this->report_fields_[field_index];
    if (field.report_id != report_id || field.report_type != report_type) {
      continue;
    }
    uint64_t raw = extract_bits_(payload, payload_length, field.bit_offset, field.bit_size);
    int64_t signed_raw = static_cast<int64_t>(raw);
    if (field.is_signed && field.bit_size < 64 && ((raw >> (field.bit_size - 1)) & 1u)) {
      signed_raw |= (~0ULL << field.bit_size);
    }
    int8_t effective_exponent = field.exponent;
    if ((field.signal == UpsSignal::INPUT_VOLTAGE || field.signal == UpsSignal::OUTPUT_VOLTAGE ||
         field.signal == UpsSignal::OUTPUT_APPARENT_POWER || field.signal == UpsSignal::OUTPUT_ACTIVE_POWER ||
         field.signal == UpsSignal::BATTERY_VOLTAGE) &&
        field.unit != 0) {
      // HID power-voltage units encode an additional base-10 scale in unit metadata.
      // NUT effectively normalizes by subtracting that built-in factor.
      effective_exponent = static_cast<int8_t>(effective_exponent - 7);
    }
    float value = apply_exponent_(static_cast<float>(field.is_signed ? signed_raw : raw), effective_exponent);
    if (field.signal == UpsSignal::AC_PRESENT) {
      this->driver_.set_ac_present(value > 0.5f);
    } else {
      this->driver_.apply_signal_value(field.signal, value);
    }
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
  while (true) {
    sockaddr_in client_address{};
    socklen_t client_address_length = sizeof(client_address);
    const int client_fd = accept(server_fd, reinterpret_cast<sockaddr *>(&client_address), &client_address_length);
    if (client_fd >= 0) {
      component->serve_nut_client_(client_fd);
      close(client_fd);
    }
  }
}

void Nut::serve_nut_client_(int client_fd) const {
  bool authenticated = false;
  std::array<char, NUT_LINE_LENGTH> buffer{};
  std::string pending;

  while (true) {
    const ssize_t length = recv(client_fd, buffer.data(), buffer.size() - 1, 0);
    if (length <= 0) {
      return;
    }

    buffer[static_cast<size_t>(length)] = '\0';
    pending.append(buffer.data(), static_cast<size_t>(length));
    size_t line_end = 0;
    while ((line_end = pending.find('\n')) != std::string::npos) {
      const std::string line = trim_crlf(pending.substr(0, line_end));
      pending.erase(0, line_end + 1);
      if (!line.empty()) {
        this->handle_nut_command_(client_fd, line, &authenticated);
      }
    }
  }
}

bool Nut::is_ups_name_(const std::string &name) const {
  return name == this->ups_name_;
}

void Nut::send_line_(int client_fd, const std::string &line) const {
  const std::string message = line + "\n";
  send(client_fd, message.c_str(), message.size(), 0);
}

void Nut::handle_nut_command_(int client_fd, const std::string &line, bool *authenticated) const {
  const std::string command = first_word(line);
  const std::string arguments = after_first_word(line);
  const auto &readings = this->driver_.readings();
  auto send_float_var = [&](const char *name, float value) {
    char value_text[24];
    snprintf(value_text, sizeof(value_text), "%.2f", value);
    this->send_line_(client_fd, "VAR " + this->ups_name_ + " " + name + " \"" + value_text + "\"");
  };

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
    this->send_line_(client_fd, (*authenticated && this->is_ups_name_(arguments)) ? "OK" : "ERR ACCESS-DENIED");
  } else if (command == "LOGOUT") {
    *authenticated = false;
    this->send_line_(client_fd, "OK Goodbye");
  } else if (line == "LIST UPS") {
    this->send_line_(client_fd, "BEGIN LIST UPS");
    this->send_line_(client_fd, "UPS " + this->ups_name_ + " \"" + this->description_ + "\"");
    this->send_line_(client_fd, "END LIST UPS");
  } else if (line == "LIST VAR " + this->ups_name_) {
    this->send_line_(client_fd, "BEGIN LIST VAR " + this->ups_name_);
    this->send_line_(client_fd, "VAR " + this->ups_name_ + " device.mfr \"" + this->driver_.manufacturer() + "\"");
    this->send_line_(client_fd, "VAR " + this->ups_name_ + " device.model \"" + this->driver_.model() + "\"");
    this->send_line_(client_fd, "VAR " + this->ups_name_ + " device.firmware \"" + this->driver_.firmware() + "\"");
    this->send_line_(client_fd, "VAR " + this->ups_name_ + " driver.name \"nut\"");
    this->send_line_(client_fd, "VAR " + this->ups_name_ + " ups.status \"" + this->driver_.status() + "\"");
    if (readings.has_input_voltage) {
      send_float_var("input.voltage", readings.input_voltage);
    }
    if (readings.has_input_frequency) {
      send_float_var("input.frequency", readings.input_frequency);
    }
    if (readings.has_input_current) {
      send_float_var("input.current", readings.input_current);
    }
    if (readings.has_output_voltage) {
      send_float_var("output.voltage", readings.output_voltage);
    }
    if (readings.has_output_frequency) {
      send_float_var("output.frequency", readings.output_frequency);
    }
    if (readings.has_output_current) {
      send_float_var("output.current", readings.output_current);
    }
    if (readings.has_output_apparent_power) {
      send_float_var("ups.power", readings.output_apparent_power);
    }
    if (readings.has_output_active_power) {
      send_float_var("ups.realpower", readings.output_active_power);
    }
    if (readings.has_load_percent) {
      send_float_var("ups.load", readings.load_percent);
    }
    if (readings.has_battery_charge) {
      send_float_var("battery.charge", readings.battery_charge);
    }
    if (readings.has_runtime_seconds) {
      send_float_var("battery.runtime", readings.runtime_seconds);
    }
    if (readings.has_battery_voltage) {
      send_float_var("battery.voltage", readings.battery_voltage);
    }
    this->send_line_(client_fd, "END LIST VAR " + this->ups_name_);
  } else if (command == "GET") {
    const std::string expected_prefix = "VAR " + this->ups_name_ + " ";
    if (arguments.rfind(expected_prefix, 0) != 0) {
      this->send_line_(client_fd, "ERR VAR-NOT-SUPPORTED");
      return;
    }
    const std::string variable = arguments.substr(expected_prefix.size());
    if (variable == "device.mfr") {
      this->send_line_(client_fd, "VAR " + this->ups_name_ + " device.mfr \"" + this->driver_.manufacturer() + "\"");
    } else if (variable == "device.model") {
      this->send_line_(client_fd, "VAR " + this->ups_name_ + " device.model \"" + this->driver_.model() + "\"");
    } else if (variable == "device.firmware") {
      this->send_line_(client_fd, "VAR " + this->ups_name_ + " device.firmware \"" + this->driver_.firmware() + "\"");
    } else if (variable == "driver.name") {
      this->send_line_(client_fd, "VAR " + this->ups_name_ + " driver.name \"nut\"");
    } else if (variable == "ups.status") {
      this->send_line_(client_fd, "VAR " + this->ups_name_ + " ups.status \"" + this->driver_.status() + "\"");
    } else if (variable == "input.voltage" && readings.has_input_voltage) {
      send_float_var("input.voltage", readings.input_voltage);
    } else if (variable == "input.frequency" && readings.has_input_frequency) {
      send_float_var("input.frequency", readings.input_frequency);
    } else if (variable == "input.current" && readings.has_input_current) {
      send_float_var("input.current", readings.input_current);
    } else if (variable == "output.voltage" && readings.has_output_voltage) {
      send_float_var("output.voltage", readings.output_voltage);
    } else if (variable == "output.frequency" && readings.has_output_frequency) {
      send_float_var("output.frequency", readings.output_frequency);
    } else if (variable == "output.current" && readings.has_output_current) {
      send_float_var("output.current", readings.output_current);
    } else if (variable == "ups.power" && readings.has_output_apparent_power) {
      send_float_var("ups.power", readings.output_apparent_power);
    } else if (variable == "ups.realpower" && readings.has_output_active_power) {
      send_float_var("ups.realpower", readings.output_active_power);
    } else if (variable == "ups.load" && readings.has_load_percent) {
      send_float_var("ups.load", readings.load_percent);
    } else if (variable == "battery.charge" && readings.has_battery_charge) {
      send_float_var("battery.charge", readings.battery_charge);
    } else if (variable == "battery.runtime" && readings.has_runtime_seconds) {
      send_float_var("battery.runtime", readings.runtime_seconds);
    } else if (variable == "battery.voltage" && readings.has_battery_voltage) {
      send_float_var("battery.voltage", readings.battery_voltage);
    } else {
      this->send_line_(client_fd, "ERR VAR-NOT-SUPPORTED");
    }
  } else if (line == "LIST CMD " + this->ups_name_) {
    this->send_line_(client_fd, "BEGIN LIST CMD " + this->ups_name_);
    this->send_line_(client_fd, "END LIST CMD " + this->ups_name_);
  } else if (command == "INSTCMD") {
    this->send_line_(client_fd, *authenticated ? "ERR CMD-NOT-SUPPORTED" : "ERR ACCESS-DENIED");
  } else {
    this->send_line_(client_fd, "ERR UNKNOWN-COMMAND");
  }
}

}  // namespace nut
}  // namespace esphome
