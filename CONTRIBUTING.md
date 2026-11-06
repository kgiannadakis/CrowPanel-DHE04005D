# Contributing

Thanks for helping improve the CrowPanel DHE04005D dual-boot firmware.

## Before opening a pull request

1. Create a branch from `main`.
2. Keep changes focused on one feature or fix.
3. Build each affected PlatformIO environment.
4. Test hardware-facing changes on a CrowPanel DHE04005D when possible.
5. Do not commit `.pio` output, logs, coredumps, credentials, API tokens, or local configuration.
6. Update `README.md` or `CHANGELOG.md` when behavior or setup changes.

## Project areas

- `selector/` contains the dual-boot selector.
- `meshcore/` contains the customized MeshCore firmware.
- `meshtastic/` contains the customized Meshtastic firmware.
- `flash_all.py` flashes the complete dual-boot image set.

For MeshCore ownership boundaries, locking rules, feature limitations and the
pre-publish checklist, see [the CrowPanel development guide](meshcore/docs/crowpanel.md).
Run `python tools/check_repository.py` from the repository root before building
the active `dhe04005d-5inch-P4-meshcore` environment. This checks installation
tooling, publishing hygiene, MeshCore source/policy assertions and portable
Meshtastic policy tests without
accessing a device. CI runs the same command. See the
[publishing checklist](docs/publishing.md) for release preparation.

When reporting a bug, include the affected firmware, build environment, hardware revision, radio module/frequency, serial output, and reproducible steps. Remove mesh identities, location data, Wi-Fi credentials, and tokens from logs before sharing them.

Preserve the license of the component you modify. The repository includes
GPL-3.0 firmware, MIT-licensed MeshCore code and separately licensed dependencies;
do not remove their attribution or license notices.
