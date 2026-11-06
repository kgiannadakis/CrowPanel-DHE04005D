# CrowPanel Development Guide

This is the active MeshCore port for the CrowPanel DHE04005D: ESP32-P4,
800x480 RGB display, GT911 touch, STC8 backlight/battery controller, ESP32-C6
Wi-Fi coprocessor and SX1262 LoRa radio. The MeshCore base is
[companion-v1.17.1](crowpanel_upstream.md).

## Build

Open `meshcore/` as the PlatformIO project, or run from that directory:

```sh
platformio run -e dhe04005d-5inch-P4-meshcore
platformio device monitor -e dhe04005d-5inch-P4-meshcore --port YOUR_PORT
```

There is one active environment. The previously tested low-power configuration
is already part of it; there is no separate low-power test environment.
The pioarduino platform and RadioLib/network-library commits are pinned in
`platformio.ini`. Retain those revisions when making an unrelated change.

Build output is `.pio/build/dhe04005d-5inch-P4-meshcore/firmware.bin`.
For installation and the complete dual-boot flashing procedure, see the
[repository README](../../README.md). Do not erase flash or overwrite a data
partition for a routine application update. Check the installed partition
layout before using raw flash offsets.

## Source Layout

| Area | Responsibility |
| --- | --- |
| `examples/crowpanel_p4_lvgl_chat/main.cpp` | Mesh callbacks, startup, deferred work, radio servicing and CPU sleep |
| `display.cpp`, `display.h` | RGB framebuffers, LVGL task/locking, touch input and display wake |
| `chat_ui.cpp`, `home_ui.cpp`, `keyboard_helpers.cpp` | Chats, conversation lists, text editing and EN/EL/DE keyboards |
| `ui_*.c`, `ui_*.h`, `features_ui.c` | Screen construction, shared widgets and UI declarations |
| `map_view.cpp`, `neighbour_*`, `advert_location_cache.h` | Offline map, read-only neighbour links and legacy placeholder cleanup |
| `persistence.cpp`, `repeater_credentials.cpp` | Chat/settings/contact storage and remembered repeater passwords |
| `wifi_ntp.cpp`, `web_dashboard.cpp`, `translate.cpp` | Wi-Fi/time, HTTP dashboard and translation |
| `unsupported_features.cpp` | Compatibility no-ops for OTA and Telegram; not working feature implementations |
| `*_policy.h`, `discovery_results.h` | Small testable state/timing/data helpers |
| `variants/crowpanel_dhe04005d/` | Board pins, I2C, SX1262, RTC and STC8/GT911 drivers |
| `src/` | MeshCore protocol/routing core and shared helpers; not CrowPanel-only code |
| `test/` | CrowPanel policy/source checks plus separate upstream GoogleTest suites |
| `tools/` | Development checks and SD map/emoji asset generators |

Bare filenames in the table refer to `examples/crowpanel_p4_lvgl_chat/`.
The older `examples/crowpanel_lvgl_chat/` is a separate legacy implementation;
it is not compiled into the active P4 environment. Vendored libraries and
upstream examples are retained with their original structure and licenses.

## Ownership and Locking

LVGL runs on its own task. Mutate widgets only with the LVGL lock, never from
a radio ISR. Mesh callbacks enqueue deferred UI work. If both locks are
needed, take the LVGL lock before the mesh mutex; do not invert this order.

The Arduino task services MeshCore. Translation's HTTP-only helper never
accesses the radio, widgets or chat files; the Arduino task services the radio
while waiting for its result. See [receive servicing](crowpanel_rx_health.md).

Do not change RGB timing, framebuffer ownership, the backlight startup pulse,
or the LoRa DIO sleep/wake latch as part of cosmetic cleanup. These paths have
hardware-specific recovery requirements.

## Checks

Run from `meshcore/`:

```sh
python tools/check_crowpanel.py
```

The script runs the Python configuration/source checks and compile-time C++17
policy assertions. It looks for a host `g++`/`clang++` or the PlatformIO ESP32
RISC-V compiler. An explicit path can be supplied with `--compiler PATH`.
Use `--python-only` to deliberately run just the Python subset. These checks
do not execute firmware or the separate GoogleTest suites.

Then build the active environment. For changes affecting the selector or
Meshtastic, also build their environments from their own project directories.
Keep those changes separate from MeshCore-only work.

From the repository root, `python tools/check_repository.py` also checks the
installation helper, publishable-file hygiene and local documentation links.
See the [test guide](../test/README.md) for coverage boundaries and the
[publishing checklist](../../docs/publishing.md) before creating a release.

Hardware verification should cover cold boot, large-chat entry, EN/EL/DE input,
translation while receiving, contact persistence, neighbour maps, and repeated
touch/message wake cycles. Recheck consumption under the same Wi-Fi and
brightness conditions after power-related changes.

## Assets and Local Files

Release emoji atlases remain in the repository's `docs/` directory. Copy them
to `/emoji/` on the microSD card. Map tiles live at `/tiles/{z}/{x}/{y}.png`.
See [emoji assets](../tools/README-emoji.md) and [map assets](../tools/README-maps.md).

Build output, serial logs, local editor configuration, `tmp/`, and `sdcard/`
working data are ignored. Keep these local rather than adding them to Git.
Do not remove `partitions.bin`, the partition CSV files, release emoji atlases,
fonts, or vendored libraries as if they were disposable build output.

## Feature Limits and Security

- The dashboard is HTTP-only on port 80, without an authentication layer. Use
  it only on a trusted LAN; do not expose it to the Internet.
- The current P4 OTA and Telegram APIs are no-ops. UI controls do not mean
  those services are implemented.
- Translation uses an unofficial endpoint. HTTPS certificate verification is
  disabled; manual requests can use plaintext HTTP. Do not treat it as private.
- Remembered repeater passwords are local plaintext storage, not a secure vault.
  See [password behavior](repeater_passwords.md).

## Before Publishing

1. Run the checks and build the affected environments.
2. Review `git status --short` and `git diff --stat` from the repository root.
3. Review new source/docs/tests as well as modified files; new required headers
   must not be omitted from the commit.
4. Exclude real credentials, identities, private keys, location data, device logs,
   build folders and temporary analysis files. Do not use `git add -f` to bypass
   their ignore rules.
5. Review the staged diff before committing. Keep a hardware-test note in the
   release description, including anything not tested.

See also [power saving](crowpanel_power_saving.md), [battery behavior](crowpanel_battery.md),
[neighbour maps](neighbour_map.md), and [upstream integration](crowpanel_upstream.md).
