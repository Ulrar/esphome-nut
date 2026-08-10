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
      ref: main
    refresh: 0s
    components: [nut]

nut:
  ups_name: eaton
  username: !secret nut_username
  password: !secret nut_password
```

`main` tracks the latest state. For repeatable builds, pin a commit SHA.

The server speaks plain-text NUT without TLS: restrict TCP/3493 to a trusted
LAN/VLAN and use a unique long password.

## Known limitations

- Instant commands are executed without interlock checks — `load.off.delay`,
  `shutdown.*` and the `outlet.N.load.*` commands really do power down the
  load, and there is no tracking (`OK TRACKING`) support.

## Differences from upstream NUT

The goal is behavioral parity with `usbhid-ups` + `upsd` on this hardware,
but the ESP port carries some deliberate adaptations. Keeping them listed
here so they can be upstreamed or reworked:

- **HID report descriptor index**: Eaton 5PX G2 (VID 0463, bcdDevice 0x0202)
  exposes a reduced descriptor at index 0 and the full one (input metrics,
  outlet telemetry, command paths) at index 1. The component tries both and
  keeps whichever parses to more items; Linux NUT gets this via
  `hid_desc_index=1` driver logic.
- **String/enum conversions** (`beeper_info`, `test_read_info`, yes/no,
  on/off) are reimplemented inline in `nut.cpp` rather than vendored
  `usbhid-ups.c` lookup tables, since those depend on NUT's `dstate`
  machinery.
- **ABM charger status** (`battery.charger.status` from
  `Charger.Mode`/`Charger.Status`) is synthesized in `nut.cpp` instead of
  using upstream's `eaton_abm_*` state machine.
- **`outlet.N.load.cycle`** is implemented as DelayBeforeShutdown(0)
  followed by DelayBeforeStartup(0) — NUT convention for power-cycle — and
  is listed so Home Assistant discovers its per-outlet restart button.
- **NUT protocol** is a subset: `LIST UPS/VAR/CMD`, `GET VAR/CMDDESC`,
  `INSTCMD`, `USERNAME/PASSWORD/LOGIN/LOGOUT`, `VER/PING`, plus a
  non-standard `DUMPDESC` debug command. No TLS, no `SET`, no tracking.
- The device handle is kept open after claiming and all control transfers
  are completed via timeout + semaphore; upstream can freely open/close.

## A note on authorship

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
