# Meshtastic Development

This directory contains the Meshtastic firmware and its CrowPanel customization.
See the repository [contribution guide](../CONTRIBUTING.md),
[CrowPanel board guide](variants/esp32p4/crowpanel_dhe04005d/README.md), and
[host test guide](test/host/README.md).

## Build And Test

From this directory:

```sh
pio run -e dhe04005d-5inch-P4-meshtastic
```

The optional [hardware test server](mcp-server/README.md) provides device discovery,
build/flash wrappers, exclusive serial sessions, and controlled device administration.
Use its own documentation and fixtures when running hardware tests.

## Safety And Ownership

- Never erase, reset, flash, reboot, shut down or reconfigure a connected device
  without the operator's explicit approval.
- Preserve mesh identity and private keys. A full factory reset regenerates keys
  and invalidates existing peer relationships; a configuration reset is distinct.
- Sequence serial operations as open, inspect/change, close. Do not open the same
  port concurrently from the monitor and a test tool.
- Hardware-test fixtures snapshot and restore `userPrefs.jsonc`. Do not edit that
  session-state file from inside a test.
- Keep protocol, board, UI and storage changes in their owning modules. Use the
  surrounding C++17 style and preserve component licenses.
- Use the existing `Throttle` helpers for elapsed-time checks; avoid rollover-unsafe
  comparisons against `last + interval`.
- Keep comments short and explain non-obvious constraints, not routine assignments.
- Format only affected source with the project's tooling; avoid unrelated vendor churn.
- Report uncertain reset causes as unknown. A last-operation breadcrumb is not a backtrace.

## Verification

Build affected environments and run relevant host tests before publishing. Document
any hardware tests not performed. Keep a matching ELF for crash decoding and redact
credentials, private keys and personal location data from diagnostic reports.
