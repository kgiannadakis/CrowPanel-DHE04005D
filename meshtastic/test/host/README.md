# Meshtastic Host Tests

## Portable Policy Checks

From the repository root:

```sh
python meshtastic/test/host/check_policies.py
```

This runs five SDK-independent tests for storage initialization, map tile layout,
contact retention, node search and coordinates. It accepts `--compiler PATH`
(g++, clang++ or Zig). A detected embedded cross-compiler performs syntax checks
only and reports that tests were not executed. Repository CI uses native g++ and
executes the tests. It does not download packages or connect to a device.

The remaining tests below require the matching PlatformIO dependencies. They
provide additional coverage and are not replaced by the portable policy checks.

## MQTT Transport

Run from the Meshtastic directory after resolving the P4 PlatformIO dependencies:

```sh
c++ -std=c++17 -Wall -Wextra -Werror -Isrc \
  -I.pio/libdeps/dhe04005d-5inch-P4-meshtastic/Nanopb \
  test/host/mqtt_transport_test.cpp -o mqtt_transport_test
./mqtt_transport_test
```

Tests exercise the actual fixed-frame buffer and chat/contact policy, including
fragmentation, QoS1, malformed headers, size limits, reconnect resets, randomized
frames, arbitrary input, contact discovery on/off, and repeated text acceptance
without a rate quota.

With MQTT contact discovery enabled, position packets are admitted as silent map
metadata. Weather/device telemetry and map reports remain filtered; position
updates do not become chat messages or trigger RF replies/phone queue copies.

```sh
c++ -std=c++17 -Wall -Wextra -Werror -Isrc \
  test/host/position_coordinates_test.cpp -o position_coordinates_test
./position_coordinates_test
```

Covers coordinate ranges, missing fields, zero placeholders, and valid positions
on the equator or prime meridian. The Node Database Codec test also verifies
coordinates survive save/update/backup reload. Actual marker rendering and MQTT
position delivery require board testing.

The parser integration test compiles the project's actual TBPubSubClient against
a framed fake Client (the small Arduino headers are host-only stubs):

```sh
c++ -std=c++17 -Wall -Wextra -Werror -Wno-sign-compare -Isrc -Itest/host/arduino \
  -I.pio/libdeps/dhe04005d-5inch-P4-meshtastic/TBPubSubClient/src \
  test/host/mqtt_pubsub_test.cpp \
  .pio/libdeps/dhe04005d-5inch-P4-meshtastic/TBPubSubClient/src/PubSubClient.cpp \
  -o mqtt_pubsub_test
./mqtt_pubsub_test
```

It covers CONNECT/CONNACK, subscriptions, fragmented QoS0/1 delivery, PUBACK,
bursts, keepalive, and reconnect. Incomplete frames must not block in readByte.
The sign-comparison exemption is for the unmodified dependency's timeout check.

These are host tests, not an ESP-Hosted, WiFi, or LoRa hardware stress test.

## Map Tile Layout

```sh
c++ -std=c++17 -Wall -Wextra -Werror -Isrc \
  test/host/map_tile_layout_test.cpp -o map_tile_layout_test
./map_tile_layout_test
```

Covers tile-slot uniqueness, overlapping tile reuse, zoom changes, missing and
out-of-world tile coordinates, capacity bounds, and 10,000 random pan steps.
These test the layout used by the firmware, not LVGL rendering or SD latency.

## Node Database Codec

```sh
c++ -x c++ -std=c++17 -include vector -Isrc -Isrc/mesh/generated \
  -I.pio/libdeps/dhe04005d-5inch-P4-meshtastic/Nanopb \
  test/host/node_database_codec_test.cpp src/mesh/generated/meshtastic/*.pb.cpp \
  .pio/libdeps/dhe04005d-5inch-P4-meshtastic/Nanopb/pb_common.c \
  .pio/libdeps/dhe04005d-5inch-P4-meshtastic/Nanopb/pb_encode.c \
  .pio/libdeps/dhe04005d-5inch-P4-meshtastic/Nanopb/pb_decode.c \
  -o node_database_codec_test
./node_database_codec_test
```

Uses the firmware's actual protobuf descriptors and vector callback. Covers
round-trip identities/names/keys, unnamed contacts, omission of unused capacity,
every short-output boundary, malformed nested messages, legacy empty slots,
bounded decoding, and clearing failed decode results before backup reload.
Filesystem atomic rename and real power-loss recovery require board testing.

## Node Search

```sh
c++ -std=c++17 -Wall -Wextra -Werror -Isrc \
  test/host/node_search_test.cpp -o node_search_test
./node_search_test
```

Covers case-insensitive Latin, German and Greek name/ID matching, emoji boundaries,
bounded unterminated names, malformed UTF-8, and 20,000 randomized inputs.
Keyboard touch targets and filtered-list navigation require testing on the panel.

## Node Retention

```sh
c++ -std=c++17 -Wall -Wextra -Werror -Isrc \
  test/host/node_retention_test.cpp -o node_retention_test
./node_retention_test
```

Covers a 300-slot last-heard MQTT cache, self/favourite/ignored/verified/RF
protection, refreshed contacts, no eligible replacement, 10,000 replacements,
and sticky RF provenance. Old saved contacts whose origin is ambiguous remain
protected. Database codec tests cover the 300-entry decode bound, importing a
1000-entry save after downgrade, and persisted MQTT origin flags. No slot
replacement reallocates the vector.

## Text Selection And Clipboard

On Windows with Zig and the PlatformIO LVGL dependency:

```powershell
./test/host/run_text_edit_test.ps1
```

Install Zig on PATH, set the `ZIG` environment variable, or pass
`-Zig /path/to/zig.exe`. The script also accepts an existing local compiler under
`.pio/host-tools/ziglang/`; that directory is not part of the repository.

Runs the actual LVGL library, McKeyboard and McTextEdit in a headless display.
Covers word/all selection, touch-handle dragging, copying message labels,
Copy/Cut/Paste, selected-text replacement by typing/backspace, Greek/German/emoji
boundaries, clipboard limits, password copy protection, field-length limits,
deleted/hidden fields and 1000 menu open/close cycles. Writes portrait/landscape
PNG renders to `.pio/text-selection-*.png` and checks canvas pixels and toolbar
bounds. The host fonts do not include the device's emoji atlas. Hardware touch
accuracy and performance with active MQTT still require testing on the device.

## CrowPanel Storage Initialization

```powershell
./.pio/host-tools/ziglang/zig.exe c++ -std=c++17 -Wall -Wextra -Werror `
  -Ivariants/esp32p4/crowpanel_dhe04005d `
  test/host/crowpanel_storage_test.cpp -o .pio/crowpanel_storage_test.exe
./.pio/crowpanel_storage_test.exe
```

Covers full-size blank-partition verification, nonblank bytes at the beginning,
middle and end (including an erased header with data elsewhere), read failures,
zero length and a final partial block. No device is formatted by these tests.
Actual mounting and persistence across reboot still require board testing.

## SX126x RX Health

```powershell
./.pio/host-tools/ziglang/zig.exe c++ -std=c++17 -Wall -Wextra -Werror -Isrc `
  -I.pio/libdeps/dhe04005d-5inch-P4-meshtastic/RadioLib/src `
  test/host/sx126x_rx_health_test.cpp -o .pio/sx126x_rx_health_test.exe
./.pio/sx126x_rx_health_test.exe
```

Tests the firmware's idle-only gain/sensitivity repair, retained gain, readback,
preservation of unrelated register bits, boost/patch disabled, invalid SPI bytes,
read/write failures, a packet starting between checks, and stale activity flags.
Completed RX, CRC, TX and timeout flags must not be discarded by activity cleanup.
These tests use fake register I/O; range and IRQ timing require board testing.

## Modem Presets

```sh
c++ -x c++ -std=c++17 -Wall -Wextra -Werror -Isrc -Isrc/mesh/generated \
  -I.pio/libdeps/dhe04005d-5inch-P4-meshtastic/Nanopb \
  test/host/modem_presets_test.cpp src/DisplayFormatters.cpp \
  src/mesh/generated/meshtastic/config.pb.cpp \
  src/mesh/generated/meshtastic/device_ui.pb.cpp \
  .pio/libdeps/dhe04005d-5inch-P4-meshtastic/Nanopb/pb_common.c \
  .pio/libdeps/dhe04005d-5inch-P4-meshtastic/Nanopb/pb_encode.c \
  .pio/libdeps/dhe04005d-5inch-P4-meshtastic/Nanopb/pb_decode.c \
  -o modem_presets_test
./modem_presets_test
```

Tests the actual radio mapper and display/channel naming code: Narrow Slow uses
62.5 kHz, SF8, CR4/6 and wire enum 13. Checks every previously implemented preset
with both bandwidth modes, bandwidth encoding, custom/fallback names, and a
Narrow Slow protobuf save/load round trip with no coding-rate override.
On-device preset selection, MQTT topics and RF interoperability require board testing.

## MQTT Traceroute

```sh
c++ -std=c++17 -Wall -Wextra -Werror -Isrc \
  -I.pio/libdeps/dhe04005d-5inch-P4-meshtastic/Nanopb \
  test/host/mqtt_traceroute_test.cpp -o mqtt_traceroute_test
./mqtt_traceroute_test
```

Covers directed trace requests, empty request payloads, request ID/target/destination
correlation, stale and unrelated replies, and keeping telemetry ports filtered.
Also checks channel-based MQTT/RF deadlines and distinguishes broker echoes from
target acknowledgements, target rejections, and local send errors. An ACK is not
a RouteDiscovery result.
Broker round trips and RF/MQTT route rendering require remote-node hardware testing.

The public broker's documented default-key port filter does not list traceroute:
https://meshtastic.org/docs/software/integrations/mqtt/#optimized-traffic-filtering
Local acceptance cannot override broker filtering or enable a remote node's downlink.
