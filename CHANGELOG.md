# Changelog

All notable changes to the CrowPanel DHE04005D dual-boot LoRa mesh firmware are
documented in this file. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project
follows simple `MAJOR.MINOR` versioning that matches the GitHub releases.

## [Unreleased]

### Changed

- Added a CrowPanel-specific Meshtastic entry guide and portable policy-test
  runner, and included Meshtastic documentation and policy coverage in the root
  publishing checks. Firmware runtime behavior is unchanged by this preparation.
- Removed stale machine-specific build notes and moved development guidance into
  the component's developer guide.
- Reorganized user/developer documentation, installation instructions and the
  screenshot gallery, with one publishing checklist and component license notes.
- Made the full-install helper importable and portable, added a read-only
  `--dry-run`, and rejected mismatched partition tables and ambiguous image inputs.
- Added repository/tooling tests and read-only GitHub CI checks with pinned actions.
- Removed unreferenced loose boot/partition binaries, a duplicate component
  manifest backup and an upstream-only CNAME.

- Neighbour map requests no longer import or prioritise repeaters in the saved
  list. Untouched generated placeholders are cleaned up on boot; map links are
  solid black and originate at the logged-in repeater.
- Documented the active CrowPanel MeshCore build, source ownership, feature
  limitations, and pre-publish verification workflow.
- Added a single command for Python source checks and C++ policy assertions.
- Removed retired HTTPS/certificate artifacts, prototype backups, disabled
  duplicate implementations, and unused monochrome font copies from the P4 app.
- Kept local device data and temporary analysis output out of Git.
- Consolidated identical emoji atlases under `docs/`, removing duplicate
  generated copies from the tools directory.
- Corrected the README's MeshCore flash offset to match the partition tables.

### Fixed

- Removed RGB bounce refills that could access cached PSRAM from an IRAM-safe
  interrupt during flash writes. LCD timing and radio settings are unchanged;
  direct-DMA visual/wake behavior still needs hardware stress verification.
- Standardized chat-flush render/queue lock ordering and serialized contact
  replacement with UI history work, with additional retained crash breadcrumbs.

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
