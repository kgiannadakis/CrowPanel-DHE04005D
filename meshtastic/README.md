# Meshtastic For CrowPanel DHE04005D

Custom Meshtastic firmware for the Elecrow CrowPanel Advanced 5-inch ESP32-P4.
This is a hardware-specific fork, not an official upstream firmware release.

[Repository overview](../README.md) | [Installation](../docs/installation.md) |
[Board guide](variants/esp32p4/crowpanel_dhe04005d/README.md) |
[Development](DEVELOPMENT.md) | [Host tests](test/host/README.md)

## Features

- Touchscreen chats, channels, searchable nodes and node actions.
- SX1262 LoRa with continuous receive, boosted gain and idle-time RX checks.
- MQTT uplink/downlink with text-focused filtering and optional contact discovery.
- Persistent contacts, chat history, identity and configuration.
- Offline maps using the shared SD tile layout, `/tiles/{z}/{x}/{y}.png`.
- English, Greek and German input, color emoji, and text selection/copy/paste.
- Optional DS3231 clock and ESP-Hosted WiFi through the onboard ESP32-C6.

MQTT delivery depends on the broker's filters, channel keys and remote-node
settings. Local traceroute support cannot make a public broker forward a blocked
request or make a remote node reply. Hardware behavior must be verified on the
target board; passing host tests is not a stability guarantee.

## Build

From the repository root:

```sh
platformio run -d meshtastic -e dhe04005d-5inch-P4-meshtastic
```

The CrowPanel environment uses the SDK and libraries selected in
[the variant configuration](variants/esp32p4/crowpanel_dhe04005d/platformio.ini).
Do not substitute the standard upstream ESP32 environment for this P4 build.

For an existing installation with the matching partition layout, update only the
application image. Do not erase flash or upload LittleFS during a normal update.
See the [installation guide](../docs/installation.md) before using any flashing
command or changing partitions.

## Source Layout

| Directory | Contents |
| --- | --- |
| `src/graphics/mcui/` | Touch UI, keyboards, chat views and UI data |
| `src/mesh/` | Mesh protocol, radio interfaces and persistent node database |
| `src/mqtt/` | Broker transport and downlink admission policy |
| `variants/esp32p4/crowpanel_dhe04005d/` | Board pins, RGB/touch drivers and storage |
| `test/host/` | Portable policies, codec tests and headless UI tests |
| `mcp-server/` | Optional hardware test tooling |
| `protobufs/`, `lib/`, other variants | Upstream protocol sources and dependencies |

The previous backlight-only timeout is retained. Experimental CPU sleep,
dynamic clock scaling and retained RGB pause/resume have been reverted.
LittleFS initializes automatically only when the entire `mtdata` partition is
verified erased; nonblank mount failures never trigger automatic formatting.

## Verification

```sh
python test/host/check_policies.py
```

Run the [complete host-test commands](test/host/README.md) for changes involving
MQTT codecs, radio registers, presets, persistence or text editing. They require
the relevant PlatformIO dependencies. Serial logs, build output, local compiler
installations and device backups stay outside the published source.

## Upstream And License

Based on [Meshtastic firmware](https://github.com/meshtastic/firmware).
Upstream documentation: [meshtastic.org](https://meshtastic.org/docs/).
GPL-3.0 applies to this firmware; see [LICENSE](LICENSE). Bundled dependencies
retain their own license and attribution notices.
