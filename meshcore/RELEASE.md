# Releasing The CrowPanel Firmware

Follow the repository [publishing checklist](../docs/publishing.md). This fork
does not configure the upstream tag-triggered multi-board release pipeline.
The repository workflow runs checks only; it does not flash hardware or publish
a firmware release.

Build the `dhe04005d-5inch-P4-meshcore` environment, preserve its matching ELF for
crash decoding, and record the tested hardware, radio, SDK and firmware versions.
Include unresolved issues and untested behavior in release notes. A successful
compile is not a hardware-stability qualification.

Publish only the intended firmware and documentation assets. Never attach a
full-flash/NVS capture, device identity, saved contacts, Wi-Fi credentials or
unredacted crash logs. Normal firmware updates must not require formatting the
user's filesystem. A partition-layout migration needs separate, explicit
installation instructions and a data-loss warning.
