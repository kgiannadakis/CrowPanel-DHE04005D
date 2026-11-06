# MeshCore For CrowPanel DHE04005D

Custom ESP32-P4 touchscreen firmware based on MeshCore
[companion-v1.17.1](docs/crowpanel_upstream.md). The active application is
`examples/crowpanel_p4_lvgl_chat/`; this checkout does not configure all the
upstream companion/repeater build environments.

## Quick Start

Run from this directory:

```sh
python tools/check_crowpanel.py
platformio run -e dhe04005d-5inch-P4-meshcore
platformio device monitor -e dhe04005d-5inch-P4-meshcore --port YOUR_PORT
```

There is one active environment. The low-power configuration is already
integrated; no temporary test environment is needed. Preserve the pinned
platform and radio/network dependencies in `platformio.ini`.

Build output is `.pio/build/dhe04005d-5inch-P4-meshcore/firmware.bin`. Use the
[installation guide](../docs/installation.md) for flashing and data-preservation
notes. Do not erase the device for a normal firmware update.

## Documentation

| Guide | Contents |
| --- | --- |
| [Development](docs/crowpanel.md) | Module layout, locking, checks and feature limitations |
| [Board](variants/crowpanel_dhe04005d/README.md) | Wiring, peripherals and driver ownership |
| [Upstream Integration](docs/crowpanel_upstream.md) | MeshCore/RadioLib revisions and local extensions |
| [Reboot Evidence](docs/crash_history.md) | Persistent diagnostics and watchdog investigation |
| [RX Diagnostics](docs/rx_diagnostics.md) | Packet-path counters and receive troubleshooting |
| [Power Saving](docs/crowpanel_power_saving.md) | Active settings and historical experiments |
| [Battery](docs/crowpanel_battery.md) | Voltage sampling and percentage filtering |
| [Neighbour Maps](docs/neighbour_map.md) | Matching known contacts and drawing links |
| [Repeater Passwords](docs/repeater_passwords.md) | Remember/show behavior and storage limitations |
| [Emoji Assets](tools/README-emoji.md) | SD atlases and generation |
| [Map Assets](tools/README-maps.md) | Offline tile preparation |
| [Protocol Reference](docs/index.md) | Bundled upstream protocol and CLI documentation |

The [repository checks](../tools/check_repository.py) also validate publishing
hygiene, local documentation links and installation tooling. See
[test coverage](test/README.md) for the difference between source guards,
compile-time assertions, emulated tests and board testing.

## Scope And Attribution

`src/` is the shared protocol/routing core. `variants/` contains board support;
`examples/` contains applications. Alternate variants still reference the legacy
CrowPanel UI, so it is retained rather than mistaken for unused P4 source.
Vendored libraries and upstream examples retain their original licenses.

MeshCore is a portable LoRa mesh-routing library. Its official project is
[meshcore-dev/MeshCore](https://github.com/meshcore-dev/MeshCore); this CrowPanel
fork is not the official universal flasher. MeshCore is MIT-licensed; see
[license.txt](license.txt). For this fork, use the repository's
[contribution guide](../CONTRIBUTING.md), [security policy](../SECURITY.md) and
[release checklist](../docs/publishing.md).
