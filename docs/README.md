# Documentation

## Using The Device

- [Installation and firmware updates](installation.md)
- [Screenshot gallery](screenshots.md)
- [MeshCore board and LoRa wiring](../meshcore/variants/crowpanel_dhe04005d/README.md)
- [Meshtastic features and build](../meshtastic/README.md) and
  [P4 board details](../meshtastic/variants/esp32p4/crowpanel_dhe04005d/README.md)
- [Emoji assets](../meshcore/tools/README-emoji.md) and [offline map tiles](../meshcore/tools/README-maps.md)
- [Battery readings](../meshcore/docs/crowpanel_battery.md)
- [Repeater passwords](../meshcore/docs/repeater_passwords.md)
- [Neighbour maps](../meshcore/docs/neighbour_map.md)

## Development And Diagnostics

- [Repository contribution guide](../CONTRIBUTING.md)
- [MeshCore architecture, ownership and locking](../meshcore/docs/crowpanel.md)
- [MeshCore test coverage](../meshcore/test/README.md)
- [Meshtastic host tests](../meshtastic/test/host/README.md)
- [Meshtastic development and hardware-test safety](../meshtastic/DEVELOPMENT.md)
- [Persistent crash reports](../meshcore/docs/crash_history.md)
- [Receive diagnostics](../meshcore/docs/rx_diagnostics.md)
- [Radio servicing](../meshcore/docs/crowpanel_rx_health.md)
- [Power-saving configuration and investigation history](../meshcore/docs/crowpanel_power_saving.md)
- [MeshCore upstream integration](../meshcore/docs/crowpanel_upstream.md)
- [Publishing checklist](publishing.md)

Optional [VS Code task examples](../tools/vscode-tasks.example.json) assume the
repository root is open and `python`/`platformio` are on PATH. Local editor and
serial-port settings are intentionally not distributed.

## Third-Party Code

The three firmware projects are bundled source trees, not interchangeable
libraries. Keep their build configuration and license notices together.
Alternate board examples and vendor files are retained for reference and
dependency compatibility; they are not all compiled by the CrowPanel environments.

| Component | License or notice |
| --- | --- |
| Repository | [GPL-3.0](../LICENSE) |
| MeshCore | [MIT](../meshcore/license.txt) |
| Meshtastic | [GPL-3.0](../meshtastic/LICENSE) |
| MeshCore LVGL copy | [MIT](../meshcore/lib/lvgl/LICENCE.txt) |
| Selector LVGL copy | [MIT](../selector/lib/lvgl/LICENCE.txt) |

Other dependencies retain the notices in their own directories or downloaded
packages. Map data is credited to OpenStreetMap contributors under the
[ODbL](https://www.openstreetmap.org/copyright); emoji generation uses
[Noto Emoji](https://github.com/googlefonts/noto-emoji).

The two release atlases in this directory are intentional distribution assets,
not disposable build output. Generated tile sets, device logs, full-flash/NVS
backups and temporary analysis belong outside the published repository.
