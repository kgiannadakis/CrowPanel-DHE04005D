# Route-width reception diagnostics

`rx` plus Enter on serial, or GET `/api/rx`, reports group-text counts by
1/2/3-byte path hash width. These are route identifiers, not message sizes or
text-header lengths. The outgoing path-width preference does not filter RX.

The `[rx-path]` serial summary is automatic, at most once per minute and only
after more group-text radio traffic. No per-packet logging or RF changes are
introduced. Counts reset on reboot:

- raw: radio delivered bytes identifying group text and the indicated route width;
  classification precedes packet validation, so corrupt metadata can be counted.
- parsed: dispatcher accepted a group-text packet, before duplicate/decryption checks.
- decrypted: channel MAC/decryption succeeded, before validating the text header.
- accepted: application resolved the channel and passed its duplicate filter,
  before queuing storage/UI work. This does not establish durable file persistence.

Repeated copies increase raw/parsed counts but normally not decrypted/accepted.
The difference between parsed and decrypted is not a decryption-failure count:
duplicates, unknown channel keys and direct packets still addressed to other hops
also stop before the callback. Analyzer reception is at its observers, not proof
that the same bytes reached this device.

The isolated production parser can be executed under RV32 emulation:

```powershell
python -m pip install --target .pio/rx-test-runtime unicorn pyelftools
python tools/test_rx_path_parser.py
```

The harness covers every legal hop count for 1/2/3-byte paths and all four route
types, truncated paths, reserved/oversize paths, and Public packet fixtures from
2026-10-31 (DX jungle, Domibe, RIZZI). It runs the parser method extracted unchanged
from `src/Dispatcher.cpp`, using the production Packet header and constructor.
It is not an end-to-end radio, cryptography or firmware test. Fixtures came from
the public read API of analyzer.on8ar.eu for the 13:20-13:30 UTC interval.
