# ESPHome Eaton 5PX NUT server

An ESPHome external component intended to expose USB HID UPS devices as NUT
servers over TCP/3493. It is designed for an ESP32-S3 using the native USB-OTG
interface in host mode. The Eaton 5PX 1500i RT2U G2 is the initial target.

## Current milestone

The component starts an authenticated NUT protocol endpoint and initializes
the ESP-IDF USB host. It deliberately reports `ups.status` as `WAIT` and
does not expose control commands until a capture of the UPS HID report
descriptor has been taken from the target UPS. It will not issue power,
battery-test, or beeper commands in this state.

This is the safe foundation for the hardware-specific implementation, not a
replacement for a working NUT server yet.

## Hardware

The ESP32-S3 board must expose the native USB-OTG D+/D- signals, usually
GPIO20/GPIO19, and supply VBUS in host mode. A USB-to-UART bridge port is not
sufficient. Connect the UPS USB-B port to the ESP32-S3 host port with an
appropriate OTG/host adapter or host-capable USB-C configuration.

The S3 USB PHY cannot be shared with USB Serial/JTAG. The example moves
ESPHome logging to `UART0`; after initial flashing use ESPHome OTA.

## Configuration

Copy `example/eaton-5px.yaml` into an ESPHome configuration directory, replace
its local `external_components` source with your repository URL, and provide
these `secrets.yaml` values:

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/YOUR_ACCOUNT/esp32-nut
      ref: initial
    components: [nut]
```

`initial` is the testing branch. Once it is merged, use `main`; for
repeatable production builds, use a tag or commit SHA.

```yaml
wifi_ssid: your-wifi-name
wifi_password: your-wifi-password
nut_username: a-non-default-nut-user
nut_password: a-long-random-password
```

Once connected, NUT clients can query:

```sh
upsc eaton@ESP_IP
```

The server does not use TLS. Restrict TCP/3493 to a trusted LAN or VLAN and
use a unique long password.

## Design

The project uses the ESP32 NUT proof of concept by
[`banoz/nut`](https://github.com/banoz/nut/tree/esp32-alpha) as a reference
for task separation and ESP-IDF USB hosting. It intentionally does not carry
that fork's hard-coded Wi-Fi credentials, writable FAT configuration,
POSIX compatibility shims, or standalone application lifecycle.

NUT's existing [`mge-hid` mapping](https://github.com/networkupstools/nut/blob/master/drivers/mge-hid.c)
is the reference for the Eaton 5PX HID fields and commands. The next hardware
milestone is to capture and parse the report descriptor for the connected
UPS firmware before enabling telemetry and instant commands.

`UpsDriver` defines the UPS-specific boundary. `Eaton5pxDriver` is the first
implementation; additional HID drivers should implement that interface rather
than extending the NUT server or ESPHome lifecycle code.
