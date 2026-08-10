# ESPHome NUT server for USB HID UPS devices

An ESPHome external component that turns an ESP32-S3 into a NUT (Network UPS
Tools) server on TCP/3493, talking to a USB HID UPS directly over the S3's
native USB-OTG host interface. Developed against an Eaton 5PX 1500i RT2U G2,
but the mapping tables cover the whole `mge-hid` (Eaton/MGE) family.

## How it works

The component vendors the actual NUT HID parser (`hidparser.c`), the
`libhid.c` path/scaling helpers, and the `mge-hid.c` variable mapping tables
from [networkupstools/nut](https://github.com/networkupstools/nut) (GPL-2.0+,
see `components/nut/LICENSE-GPL2-upstream`). At runtime it:

1. Captures the UPS HID report descriptor over USB.
2. Resolves official NUT HID paths (`UPS.PowerSummary.RemainingCapacity`,
   `UPS.PowerConverter.Output.Voltage`, ...) against the descriptor using the
   vendored parser — exactly what `usbhid-ups` does on a Linux host.
3. Polls the resolved reports and serves the values with upstream NUT
   variable names, formats and scaling on an authenticated NUT protocol
   endpoint.

Any NUT client (`upsc`, `upsmon`, Home Assistant, Proxmox, ...) can connect:

```sh
upsc eaton@ESP_IP
```

## Hardware

The ESP32-S3 board must expose the native USB-OTG D+/D- signals (usually
GPIO19/GPIO20) and supply VBUS in host mode. A USB-to-UART bridge port is
not sufficient. Connect the UPS USB-B port to the S3 host port with a
host-capable adapter.

The S3 USB PHY cannot be shared with USB Serial/JTAG, so the example moves
ESPHome logging to `UART0`; after initial flashing use OTA.

## Configuration

See `example/eaton-5px.yaml`. The component is sourced directly from git:

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/Ulrar/esphome-nut
      ref: initial
    refresh: 0s
    components: [nut]

nut:
  ups_name: eaton
  username: !secret nut_username
  password: !secret nut_password
```

`initial` is the development branch. For repeatable builds, pin a commit SHA.

The server speaks plain-text NUT without TLS: restrict TCP/3493 to a trusted
LAN/VLAN and use a unique long password.

## Known limitations

- The 5PX G2 HID descriptor does not expose live `input.voltage` /
  `input.current` / `input.frequency` (only the nominal `Flow.[1].Config*`
  values), nor `AudibleAlarmControl` — so no live input metrics and no
  beeper commands, matching upstream `usbhid-ups` on the same hardware.
- Outlet groups are named (`Outlet.[1..3].iDesignator`) but expose no
  switch/delay control paths over USB HID. Eaton's own software likely uses
  the vendor COPIBridge reports (0xFE/0xFF); reverse-engineering those is
  possible future work.
- Instant commands are executed without interlock checks — `load.off.delay`
  and `shutdown.*` really do power down the load.

## A note on authorship

This project was built by an LLM coding assistant (GitHub Copilot CLI) under
human direction — "vibe coded", including the ESPHome glue, with the NUT
parser and mapping tables vendored from upstream. Treat it accordingly:
it works on the developer's hardware, but review before trusting it with
production power control.

## Debugging

- The `DUMPDESC` command on the NUT port streams the raw HID report
  descriptor as hex (e.g. `echo DUMPDESC | nc ESP_IP 3493`).
- At DEBUG log level the full parsed HID path tree is dumped on attach.
- The example config includes heap debug sensors to watch RAM headroom.

## License

GPL-2.0-or-later, following the vendored NUT driver code.
