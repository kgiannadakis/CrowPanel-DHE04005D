# Changelog

All notable changes to the CrowPanel DHE04005D dual-boot LoRa mesh firmware are
documented in this file. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project
follows simple `MAJOR.MINOR` versioning that matches the GitHub releases.

## [1.1] - 2026-06-21

### Added
- **Greek and German language support** across both UIs — full Greek font set
  and German Latin LVGL fonts (10–22 px), with on-screen keyboard layouts for
  the new characters.
- **Emoji rendering** via a dedicated emoji atlas/font pipeline
  (`McEmojiAtlas`, `McEmojiFont`, `McFonts`) shared by the Meshtastic UI.
- **Translate-on-receive** for incoming messages and richer map markers —
  distinct marker shapes/labels with repeater markers now drawn at every zoom
  level (z7–z12).
- Chat history now loads up to ~7 days of messages when a conversation is
  opened.
- Hosted-SDIO driver (`CrowpanelHostedSdio`) for the ESP32-P4 variant.

### Changed
- Vendored the `ESP32_Display_Panel`, `ESP32_IO_Expander` and `esp-lib-utils`
  libraries into the tree for reproducible firmware builds.
- Numerous mcui screen, clock, sender and node-action refinements.

### Fixed
- **MQTT stability** on the 5" RGB-panel board — addressed PSRAM/internal-RAM
  starvation that could crash the device under bidirectional MQTT load, and
  hardened the MQTT/ServiceEnvelope paths.

## [1.0] - Initial release

- First public dual-boot release: choose **MeshCore** or the custom
  **Meshtastic** UI at boot, no reflashing required.
- Combined flash image: `bootloader.bin`, `partitions.bin`, `selector.bin`,
  `meshcore.bin`, `meshtastic.bin`.
