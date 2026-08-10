#include "nut.h"

#include <algorithm>
#include <array>
#include <cctype>
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

struct UsbClientContext {
  Nut *component;
  usb_host_client_handle_t client;
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

  while (true) {
    const esp_err_t result = usb_host_client_handle_events(client, portMAX_DELAY);
    if (result != ESP_OK) {
      ESP_LOGW(TAG, "USB client event handling failed: %s", esp_err_to_name(result));
    }
  }
}

void Nut::usb_client_event_callback_(const usb_host_client_event_msg_t *event, void *argument) {
  auto *context = static_cast<UsbClientContext *>(argument);
  if (event->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
    context->component->log_usb_device_(context->client, event->new_dev.address);
  }
}

void Nut::log_usb_device_(usb_host_client_handle_t client, uint8_t device_address) const {
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
  ESP_LOGI(TAG, "HID report parsing is disabled until the report descriptor is captured");
  usb_host_device_close(client, device);
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
