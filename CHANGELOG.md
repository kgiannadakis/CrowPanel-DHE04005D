# Changelog

All notable changes to the CrowPanel DHE04005D dual-boot LoRa mesh firmware are
documented in this file. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project
follows simple `MAJOR.MINOR` versioning that matches the GitHub releases.

## [3.0] - 2026-11-06

Major firmware and stability revision for the CrowPanel DHE04005D dual-boot
platform.

### Added

#### MeshCore

- Updated the bundled MeshCore firmware to version 1.17.1.
- Added battery level and charging-state indicators across the main interface.
- Added screen power-saving, touch-wake recovery, and awake-idle CPU scaling.
- Added repeater-neighbour visualization on the map, including the logged-in
  repeater's location and links to located neighbours.
- Added secure per-repeater password storage with Remember Password and Show
  Password controls.
- Added persistent crash diagnostics and reboot history.
- Added receive-health and route-width diagnostics through serial and the web API.
- Added additional radio, display, Ethernet, vibration, rotary-input, routing,
  configuration, and UTF-8 support inherited from the updated MeshCore codebase.
- Added extensive policy and regression tests for power management, touch wake,
  backlight control, battery handling, neighbour maps, repeater credentials,
  routing, reception, persistence, and display recovery.

#### Meshtastic

- Added node search by long name, short name, or node ID.
- Added improved offline map rendering with positioned-node markers, clustering,
  optional node names, and saved map settings.
- Added safer reusable text-editing and keyboard handling.
- Added additional CrowPanel storage handling and storage-policy validation.
- Added host-side policy tests for maps, nodes, radio reception, MQTT, storage,
  positions, traceroute, and database retention.

#### Repository and tooling

- Added GitHub validation workflows and repository publishing checks.
- Added consolidated installation, screenshot, development, and publishing
  documentation.
- Added a portable, importable full-device installation helper with a read-only
  `--dry-run` mode.
- Added validation for partition-table agreement, firmware-image size, image
  type, overlap, and ambiguous build outputs.

### Changed

- Updated the CrowPanel MeshCore base from 1.16 to the official 1.17.1 release.
- Retained the CrowPanel Meshtastic base at 2.7.26.54 with extensive
  CrowPanel-specific improvements.
- Reworked MeshCore display and power handling while retaining the established
  LCD timing and radio configuration.
- Improved MeshCore maps, repeater management, chat persistence, translations,
  battery display, settings, and web diagnostics.
- Improved Meshtastic maps, nodes, keyboard behavior, MQTT handling, storage,
  radio diagnostics, traceroute, and position validation.
- Updated the selector screenshot and added the printable CrowPanel on/off-switch
  model.
- Reorganized project documentation and removed obsolete generated files,
  backups, duplicate assets, and unused development artifacts.
- Corrected and validated firmware offsets against the active partition tables.
- Consolidated duplicate emoji atlases under `docs/`.

### Fixed

- Fixed an ESP32-P4 display path that could access cached PSRAM from an
  interrupt-safe context during flash writes.
- Fixed display wake and backlight sequencing, including stalled-wake recovery.
- Fixed chat-render and persistence lock ordering that could contribute to
  crashes under concurrent UI activity.
- Fixed repeater-neighbour map placeholders, origin selection, and link rendering.
- Fixed battery-value filtering and propagation across lazily created screens.
- Improved MeshCore receive servicing and diagnostics during prolonged UI,
  storage, and networking activity.
- Improved Meshtastic node-list refresh behavior, map coordinate validation,
  traceroute state handling, MQTT buffer safety, and database retention.
- Improved protection against mismatched partition tables and incorrect firmware
  images during a full-device installation.

### Upgrade notes

- Back up important device data before upgrading.
- Download and flash all five release assets: `bootloader.bin`,
  `partitions.bin`, `selector.bin`, `meshcore.bin`, and `meshtastic.bin`.
- Version 3.0 should be installed as a complete firmware set. Do not flash only
  one application partition.
- The external DS3231 RTC remains optional but is recommended for retaining the
  time while the device is powered off.

## [2.0] - 2026-07-11

This is a major revision. **Erase the device completely before flashing Version 2.0.**

### Added

- Added display-memory allocation support for the ESP32-P4 RGB panel in Meshtastic.
- Added message character counting.
- Integrated additional Meshtastic hardware, sensor, display, input, radio, and
  protocol support from the updated upstream codebase.
- Integrated new MeshCore boards, display drivers, radio variants, and tests
  from the updated upstream codebase.

### Changed

- Updated the bundled MeshCore source tree to version 1.16.
- Updated the bundled Meshtastic firmware and protobuf sources to version 2.7.26.54.
- Revised the Meshtastic CrowPanel display implementation and PSRAM allocation
  behavior.
- Updated the dual-boot selector and partition layouts.
- Updated the complete-device flashing script.
- Regenerated the partition binary used by the combined installation.
- Expanded support for newer boards and platform configurations in both
  firmware trees.

### Fixed

- Improved ESP32-P4 display-memory management and allocation safety.
- Improved CrowPanel backlight and STC8 controller integration.
- Improved stability during UI, persistence, networking, and memory-intensive
  operations.

### Upgrade notes

- **Erase the device before installing Version 2.0.**
- Version 2.0 introduces a new dual-boot partition layout. Perform a
  complete-device flash with `flash_all.py`; do not update only one firmware
  partition.
- The external DS3231 RTC remains optional but is recommended for retaining the
  time while the device is powered off.

## [1.1] - 2026-06-21

### Added
- **Greek and German language support** across *both* UIs:
  - MeshCore chat: Greek font set (`lv_font_greek`) and German Latin LVGL fonts
    in 7 sizes (10, 12, 14, 16, 18, 20, 22 px).
  - Meshtastic UI: dedicated `McGreekFont` and `McGermanFont` glyph sets.
  - On-screen keyboard layouts extended to type the new Greek/German characters.
- **Emoji rendering** in both UIs via a dedicated atlas/font pipeline
  (`McEmojiAtlas`, `McEmojiFont`, `McFonts` on Meshtastic; `emoji_atlas` on
  MeshCore), with prebuilt atlases shipped under `docs/`.
- **Translate-on-receive** — incoming messages can be auto-translated as they
  arrive.
- **Improved map view** — distinct marker shapes and labels per node type, with
  repeater markers now drawn at every zoom level (z7–z12).
- **Longer chat history** — up to ~7 days of messages are loaded when a
  conversation is opened.
- Hosted-SDIO driver (`CrowpanelHostedSdio`) for the ESP32-P4 variant.

### Changed
- Vendored the `ESP32_Display_Panel`, `ESP32_IO_Expander` and `esp-lib-utils`
  libraries into the tree for reproducible firmware builds.
- Refreshed Meshtastic mcui throughout — theme, tab bar, keyboard, settings,
  node list/actions, clock, sender and chat view.
- Updated the printable enclosure models (`case_A`, `case_B` STL).

### Fixed
- **MQTT stability** on the 5" RGB-panel board — addressed PSRAM / internal-RAM
  starvation that could crash the device under bidirectional MQTT load, and
  hardened the MQTT / `ServiceEnvelope` paths (plus related NodeDB, Router and
  MeshService fixes).

## [1.0] - Initial release

- First public dual-boot release: choose **MeshCore** or the custom
  **Meshtastic** UI at boot, no reflashing required.
- Combined flash image: `bootloader.bin`, `partitions.bin`, `selector.bin`,
  `meshcore.bin`, `meshtastic.bin`.
