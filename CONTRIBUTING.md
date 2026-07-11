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

When reporting a bug, include the affected firmware, build environment, hardware revision, radio module/frequency, serial output, and reproducible steps. Remove mesh identities, location data, Wi-Fi credentials, and tokens from logs before sharing them.

By contributing, you agree that your contribution is licensed under this repository's GPL-3.0 license.
