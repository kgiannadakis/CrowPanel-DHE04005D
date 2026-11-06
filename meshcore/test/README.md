# MeshCore Test Coverage

## Source And Policy Checks

From `meshcore/`:

```sh
python tools/check_crowpanel.py
```

This discovers `test_*/test_*.py` configuration/source guards and compiles the
listed C++17 policy assertion files without executing a firmware image. It uses
a host `g++`/`clang++`, `--compiler PATH`, or the installed ESP32 RISC-V compiler.
`--python-only` is an explicit partial check, not full verification.

From the repository root, `python tools/check_repository.py` additionally tests
the installation helper and repository hygiene. CI runs that command without
contacting a device. The checked partition tables and release assets must remain
available in a fresh clone, not only in a developer's `.pio` directory.

## Production-Method Emulation

Install `unicorn` and `pyelftools` in the Python environment running these tools,
and install the PlatformIO ESP32 RISC-V compiler by building the normal firmware.
Local Python packages under `.pio/rx-test-runtime/` are also supported.

```sh
python tools/test_chat_persistence.py
python tools/test_factory_boot_guard.py
python tools/test_rx_path_parser.py
```

These compile extracted production methods and execute RV32 code against mocks.
They cover chat batching/retry/lock order, factory-image bounds/selection, and
packet path-width parsing. They do not simulate actual multithread scheduling,
flash/cache behavior, RF reception, power loss or LCD scanout.

## Upstream Suites

Other `test_*` directories contain imported MeshCore tests. They need the
upstream native/GoogleTest toolchain and are not executed by the CrowPanel
source/policy runner. This checkout does not configure the upstream `native`
PlatformIO environment. Do not report those suites as passed after only running
the source guards.

## Hardware Tests

Build success is separate from device verification. Test cold boot, large chats,
simultaneous RX/UI work, contact/history persistence, input/clipboard, translation,
maps and repeated touch/message wake. Preserve the matching ELF and sanitized
reboot report for a watchdog investigation. Never erase flash as a test shortcut.
