# CrowPanel MeshCore Base

This port integrates the official `companion-v1.17.1` release:

- Repository: https://github.com/meshcore-dev/MeshCore
- Commit: `d92964352441e53b93e8667b802e04f6e072b39e`
- CrowPanel firmware identifier: `1.17.1-p4-rc1`
- RadioLib revision: `6d8934836678d8894e3d556550475b37dce3e2b6`

The upstream core, standard firmware examples, protocol documentation, and
tests were integrated using a three-way source merge from the previous 1.16.0
base. CrowPanel's extended text-header support remains in BaseChatMesh.
Upstream CI and unrelated board definitions were not imported; this project's
PlatformIO configuration remains specific to the ESP32-P4 CrowPanel.

The CrowPanel contact consumers use `startContactsIterator()` to skip the
eight anonymous-request slots reserved by 1.17.1. This includes contact
persistence, conversation lists, repeater lists, and the map. The existing
contact/channel files and flash partition layout are unchanged.

The local `Packet` extension retains received RSSI alongside SNR while packets
wait in the inbound queue. These measurements are not serialized or transmitted.
CrowPanel's manual zero-hop repeater discovery uses them for reception results;
the response payload's SNR is displayed separately as the repeater's reception
of the discovery request. Scan results are RAM-only and cleared for each scan.
Manual scans queue the original flood advert and zero-hop discovery request,
arming the reply tag under the mesh mutex. The original 15-second scan window
is restored, without the experimental TX-callback scan controller. An empty scan does not prove
that no repeater is nearby: upstream repeaters limit discovery to four requests
per two-minute window, shared by manual scans and channel receipt probes.

## Verification

Build with:

```powershell
platformio run -e dhe04005d-5inch-P4-meshcore
```

After an application-only flash, verify contacts, repeaters, channels, and
identity survive reboot; send and receive public/private channel messages and
direct messages; check delivery acknowledgements and map pins. Retest chat
entry with a large history and confirm crash diagnostics remain available.
Run Repeater Discovery and check local RX RSSI/SNR and remote request SNR;
repeat the scan after moving the antenna and confirm stale results disappear.
Verify replies appear again with a known nearby repeater; an empty scan reports
only that no direct discovery replies were received, not a guessed cause.
Do not erase flash or upload a new filesystem image as part of this update.

The imported upstream native tests require a host C++ compiler and Google's
test framework; the embedded cross-compiler alone cannot run them.
