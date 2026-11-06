# CrowPanel DHE04005D

Standalone MeshCore and Meshtastic touchscreen firmware for the Elecrow
CrowPanel Advanced 5-inch ESP32-P4. A boot selector switches between the two
firmwares without reflashing.

<p align="center">
  <img src="docs/images/Meshcore_channels.png" width="240" alt="MeshCore chats">
  <img src="docs/images/Meshtastic_channels.png" width="240" alt="Meshtastic channels">
</p>

[Installation](docs/installation.md) | [Documentation](docs/README.md) |
[Screenshots](docs/screenshots.md) | [Contributing](CONTRIBUTING.md) |
[Release Checklist](docs/publishing.md)

## Firmware

| Project | Purpose | PlatformIO environment |
| --- | --- | --- |
| [MeshCore](meshcore/README.md) | Standalone mesh chat and repeater management | `dhe04005d-5inch-P4-meshcore` |
| [Meshtastic](meshtastic/README.md) | Custom CrowPanel chat, nodes, maps and settings UI | `dhe04005d-5inch-P4-meshtastic` |
| [Selector](selector/platformio.ini) | Touchscreen boot menu and remembered selection | `dhe04005d-5inch-P4-selector` |

MeshCore is based on [companion-v1.17.1](meshcore/docs/crowpanel_upstream.md).
Its current identifier is `1.17.1-p4-rc1`; this is a development build, not a
claim that every feature has passed hardware qualification.

### Meshtastic Features

- Standalone LoRa chat, custom channels, searchable contacts and node actions.
- Bidirectional MQTT with text-focused filtering and optional contact discovery.
- Persistent chat history, node records, identity and radio configuration.
- SD-backed offline maps, color emoji, EN/EL/DE input and copy/paste.
- Continuous SX1262 receive with boosted gain and idle-time health checks.

See the [Meshtastic guide](meshtastic/README.md) for board-specific behavior and
broker limitations. Its experimental CPU/RGB power-saving paths are reverted;
the backlight-only screen timeout remains.

### MeshCore Features

- Direct and public/private channel messaging, persistent history and delivery status.
- English, Greek and German keyboards, text selection, cut/copy/paste and swipe-to-reply.
- SD-backed color emoji and a 139-character message composer.
- Repeater discovery, remote management and remembered-password controls.
- Offline maps, clustered markers, optional labels and neighbour links for known locations.
- HTTP dashboard, Wi-Fi/NTP and automatic or manual translation.
- Persistent brightness/settings, battery status and screen-off CPU light sleep.
- Quiet serial RX diagnostics and persistent reboot evidence.

MeshCore's P4 Telegram and OTA APIs are compatibility stubs, not working services.
The dashboard has no authentication and belongs on a trusted LAN. Translation
sends message text to an external service; see the [security notes](meshcore/docs/crowpanel.md#feature-limits-and-security).

## Hardware

This port targets **DHE04005D**, not every CrowPanel model. It uses an ESP32-P4,
16 MB flash, 32 MB PSRAM, an 800x480 RGB display, GT911 touch, STC8 backlight/battery
controller, ESP32-C6 Wi-Fi coprocessor and SX1262 LoRa module.

See the [board guide](meshcore/variants/crowpanel_dhe04005d/README.md) for pin
assignments and the [installation guide](docs/installation.md#optional-rtc) for
the optional external RTC. Match the radio module and antenna to your band.

## Build And Check

Install Python 3.11, Git, PlatformIO and a C++17 compiler. The pinned P4 SDK
and firmware dependencies are downloaded by PlatformIO on the first build.

From the repository root:

```sh
python tools/check_repository.py
platformio run -d selector -e dhe04005d-5inch-P4-selector
platformio run -d meshcore -e dhe04005d-5inch-P4-meshcore
platformio run -d meshtastic -e dhe04005d-5inch-P4-meshtastic
```

The checks do not access a device. Without a host compiler, the check runner can
use an installed PlatformIO RISC-V compiler. `--python-only` deliberately skips
the C++ policy checks; it is not a substitute for a complete build.

For a routine firmware update, use the affected project's normal PlatformIO
Upload action after verifying the installed partition layout. **Do not erase
flash or upload a filesystem image as part of routine updates.** Full-device
installation and flash-plan validation are documented separately in
[Installation](docs/installation.md).

## Repository Layout

```text
meshcore/       MeshCore source, board drivers, UI, tests and asset tools
meshtastic/     Meshtastic source and CrowPanel customization
selector/       Dual-boot selector and shared partition CSV
docs/           User guides, screenshots and release emoji atlases
tools/          Repository checks and portable development examples
test/           Tests for repository and installation tooling
.github/        CI checks, issue forms and pull-request template
flash_all.py    Full-device installation helper; not a routine update tool
partitions.bin  Distributed dual-boot partition table
```

Upstream examples, alternate board variants and vendored libraries retain their
structure and license notices. Local build output, device logs and private
configuration are excluded from Git; they are not release assets.

## Contributing And Support

Read [Contributing](CONTRIBUTING.md) before submitting a change. Report bugs with
the firmware/environment, build identifier, reproduction steps and sanitized
logs. Report vulnerabilities privately as described in [Security](SECURITY.md).
See [CHANGELOG.md](CHANGELOG.md) for changes and upgrade notes.

## Attribution And License

Built on [MeshCore](https://github.com/meshcore-dev/MeshCore),
[Meshtastic](https://meshtastic.org/), [LVGL](https://lvgl.io/), and Elecrow's
CrowPanel hardware. Offline maps use OpenStreetMap data; emoji assets derive
from Noto Color Emoji. Keep their attribution and licensing when redistributing.

The repository includes GPL-3.0 code; see [LICENSE](LICENSE). Bundled components
retain their own licenses, including [MeshCore's MIT license](meshcore/license.txt)
and vendored library notices. See [component notes](docs/README.md#third-party-code).
