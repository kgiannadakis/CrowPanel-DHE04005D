# Installation And Updates

## Requirements

- Elecrow CrowPanel Advanced 5-inch DHE04005D with the matching SX1262 module.
- Python 3.11, Git and PlatformIO, or the PlatformIO VS Code extension.
- A reliable USB supply and data cable.
- A backup of important device data before any firmware installation.

Clone this repository, open the relevant firmware directory as a PlatformIO
project, and retain its pinned SDK/library revisions. Close the serial monitor
before uploading. Replace `YOUR_PORT` below with the actual device port.

## Routine Firmware Updates

For a device already using this repository's current partition layout:

```sh
platformio run -d meshcore -e dhe04005d-5inch-P4-meshcore
platformio run -d meshcore -e dhe04005d-5inch-P4-meshcore -t upload --upload-port YOUR_PORT
```

Use the equivalent Meshtastic project/environment when updating Meshtastic.
Do not use a merged/factory image, erase flash, upload a filesystem image or run
the full-install helper for an ordinary app update. Verify the installed layout
first; an old layout needs a separate migration plan. Keep the matching
`firmware.elf` locally when investigating a crash.

## Full Dual-Boot Installation

`flash_all.py` is a full installation tool, not a data-migration or backup tool.
It writes the bootloader, partition table, selector and both application slots.
It does not erase the entire chip or intentionally write the data partitions,
but a wrong or changed layout can still make existing data inaccessible.
Back up first and do not assume this operation preserves an unknown layout.

For the normal complete installation, clone the repository and use the single
build-and-flash command:

```sh
git clone https://github.com/kgiannadakis/CrowPanel-DHE04005D.git
cd CrowPanel-DHE04005D
python flash_all.py YOUR_PORT
```

Replace `YOUR_PORT` with the device port, such as `COM20` on Windows,
`/dev/ttyUSB0` on Linux, or `/dev/cu.usbserial` on macOS. The command builds all
three projects, validates their images and partition layout, and flashes the
complete set.

The following commands are optional advanced alternatives for building or
checking the images separately:

```sh
platformio run -d selector -e dhe04005d-5inch-P4-selector
platformio run -d meshcore -e dhe04005d-5inch-P4-meshcore
platformio run -d meshtastic -e dhe04005d-5inch-P4-meshtastic

# Inspect already-built inputs; no build, serial connection or file writes.
python flash_all.py --dry-run

# Full installation, after reviewing the plan and backing up the device.
python flash_all.py YOUR_PORT --skip-build
```

Without `--skip-build`, the helper builds the projects first. It locates the
PlatformIO CLI in the usual user installation or on PATH. Run it from a Python
environment containing `esptool` when actually flashing; the PlatformIO Python
environment normally provides it.

The plan checks that all three partition CSVs, the distributed table and the
selector's generated table agree. It rejects missing, oversized, wrong-magic or
ambiguous application files. These checks are not a complete firmware signature
verification, device backup or on-device compatibility test. The normal esptool
RAM-stub path is retained; do not substitute `--no-stub` after a communication
failure. Stop and investigate instead.

### Flash Layout

The source of truth is [selector/partitions_dualboot.csv](../selector/partitions_dualboot.csv).
MeshCore and Meshtastic carry matching CSVs; [partitions.bin](../partitions.bin)
is distributed for installation. Do not edit or regenerate one table independently.

| Address | Input | Role |
| --- | --- | --- |
| `0x2000` | Meshtastic build's `bootloader.bin` | Bootloader |
| `0x8000` | Selector build's `partitions.bin` | Partition table |
| `0x10000` | `selector.bin` | Factory selector |
| `0x210000` | `meshcore.bin` | MeshCore application |
| `0x690000` | `meshtastic.bin` | Meshtastic application |

The latter names describe release assets; the build helper uses each project's
actual application filename. Data partitions `mcdata` and `mtdata` are not
installation inputs. Restore a selector separately only with a verified image
that fits its factory slot. See the [boot-recovery notes](../meshcore/docs/crash_history.md#factory-slot-boot-recovery).

## SD Assets

Create `/emoji/` on the microSD card and copy
[emoji_atlas_20.bin](emoji_atlas_20.bin) and
[emoji_atlas_32.bin](emoji_atlas_32.bin) into it. Both firmwares use the same
offline map tile layout, `/tiles/{z}/{x}/{y}.png`, on the card.

Both map views support zoom levels Z7-Z18. Detailed Z18 imagery requires tiles
under `/tiles/18/{x}/{y}.png`; increasing the zoom limit does not generate them.

See the [emoji guide](../meshcore/tools/README-emoji.md) and
[map guide](../meshcore/tools/README-maps.md). Country-scale downloads need an
appropriate tile provider; do not bulk-download the public OpenStreetMap server.

## Optional RTC

The firmware supports an external RTC on the shared I2C bus at address `0x68`.
The board's expansion header uses GPIO45 for SDA and GPIO46 for SCL. These pins
are not spare LoRa RXEN/TXEN pins. Use a 3.3 V-compatible module and verify its
board-specific wiring and backup-battery requirements before connecting it.
The driver is under `meshcore/variants/crowpanel_dhe04005d/ds1307.*`.

## First Boot And Verification

Select the desired firmware, configure the radio/channel settings, and enable
Wi-Fi only when needed. Open the MeshCore dashboard at `http://DEVICE_IP` after
enabling it; HTTPS is not supported and the service has no authentication.

Verify message transmission/reception, contacts, settings persistence, touch,
and repeated touch/message wake. Hardware tests and known limitations belong
in release notes rather than being inferred from a successful upload.
