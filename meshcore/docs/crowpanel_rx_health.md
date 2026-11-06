# CrowPanel receive servicing

## Quiet RX snapshot

`http://DEVICE_IP/api/rx` returns counters since reboot: raw radio packets,
read failures and the last RadioLib read error, TX count, RX restart failures,
pending interrupt and maximum main-task radio-service gap. Per-channel counts
are incremented after channel resolution and application deduplication, before
chat persistence/UI work. The last receive time is the CrowPanel RTC epoch,
not the sender's timestamp; no message bodies or keys are included.

Compare snapshots before and after a known Heltec reception. Counters cannot
identify a packet that the radio never decoded, or retrospectively explain
missing messages before this build. A maximum gap alone does not prove loss;
screen-off timer sleeps intentionally add service gaps. Busy snapshots ask for
a retry. The endpoint takes a short mesh-locked snapshot then formats outside
the lock, sends no RF commands, and adds no periodic serial or flash writes.
Alternatively send `rx` plus Enter/newline over serial. Prefer this for Wi-Fi-off
sleep comparisons: enabling hosted Wi-Fi blocks CPU light sleep for that boot
and would change the conditions of the test. Counters reset on reboot.

## SPI transaction concurrency fix (October 21, 2026)

A device log captured `spi_device_transmit` asserting
`ret_trans == trans_desc`, with SPI calls present on both CPU stacks. The
CrowPanel HAL's transaction begin/end hooks were empty, allowing tasks to
interleave commands and consume another caller's completion on the same SPI
device. This is a software synchronization defect, not evidence of poor RF
signal, neighbour-map corruption, or a gain-setting problem. The exact caller
chain cannot be reliably decoded without the ELF matching the logged firmware.

The HAL now initializes a static FreeRTOS mutex before radio startup. RadioLib
takes it before lowering chip-select and releases it after raising chip-select,
so every command and all its 64-byte FIFO chunks stay together. The lock does
not disable interrupts or the scheduler and is not allocated in the RX path.
No RF settings, receive gain, GPIO assignments, or sleep timings are changed.

UI floor-noise samples also take the mesh mutex without waiting, skipping busy
ticks. The job counts 20 successful reads and deletes itself and its timer
exactly once. The TX-power settings callback takes the mesh lock around the
radio/pref update and releases it before NVS or UI work.

Verification: source-invariant checks and a normal debug firmware build; on
hardware exercise noise-floor measurement, repeater commands, RX traffic,
screen wake, and TX-power changes concurrently. A compile/source check does
not establish that the intermittent crash has been eliminated on the device.

## October 17, 2026

This change improves how promptly software handles radio packets. It does not
increase the SX1262's physical sensitivity or change frequency, bandwidth,
spreading factor, coding rate, transmit power, or the power-saving profile.

The Arduino task now services MeshCore before UI work, after the deferred UI
drain, after translation, and before CPU light sleep. The mesh mutex is acquired
without waiting. UI callbacks from received packets remain deferred.

Translation previously performed synchronous HTTP/TLS on that same task. A
single lazy HTTP-only helper now performs the request while the Arduino task
waits in 5 ms intervals and services the radio. No second translation starts
until that helper finishes. Other main-loop network consumers do not advance
during the wait. Queue handling, widget updates and persistence stay on the
Arduino task; the helper holds no LVGL pointers and never accesses chat files
or the radio. Its 12 KB internal stack is included in the existing heap guard.
The helper blocks on a semaphore when idle and is never allocated if translation
is unused. Its stack remains allocated once it has been created.

Translation waits for pending message bubbles to be drained before selecting a
request. New requests queued by radio callbacks during HTTP wait for the next
main-loop iteration, preserving live translation ordering.

RX gain is read back at startup, after screen-off/wake transitions, and silently
every 30 seconds. Checks defer during TX, a pending RX interrupt, or an active
preamble/header. Only a recognized normal/boosted value can trigger restoration
of the configured setting, including the retention setting. SPI errors or
unknown register bytes produce a warning instead of an assumed successful fix.
The check does not put the radio in standby or reset the receiver. Continuous
LoRa RX and the existing DIO wake/latch recovery are unchanged.

## Serial Output

`[rx-health] RX gain boosted; screen=on; verified` confirms register readback,
not just a successful write. One confirmation is printed per screen transition;
successful periodic checks are silent. A restored setting is explicitly logged.

`[rx-health] service gap=...; pending RX=...; received=...; read errors=...`
warns about service gaps over 500 ms awake or 1000 ms dark, or changed read-error
counts, including failed attempts to restart RX and the last RadioLib error
code. Warnings are limited to one per 10 seconds. `received` counts successful
raw radio reads, not unique chat messages. Read errors include CRC/read failures;
they do not count packets never detected or overwritten before software read
them. A long gap indicates a risk, not proof of packet loss.

UI history work, translation file rewriting, and other synchronous network/file
operations can still delay servicing. This is not a hard real-time guarantee;
the warnings help identify remaining delays. There are no automatic discovery
transmissions, periodic scan results, or noise-floor dumps.

## Device Verification

1. With the usual radio preset, check the gain confirmation at boot, then after
   touch wake and message wake. Repeat several sleep/wake cycles with Wi-Fi off.
2. Send numbered messages from another device while scrolling a large channel
   and opening chats. Watch for missing numbers and `[rx-health]` gap warnings.
3. Enable Wi-Fi and repeat while manually/automatically translating. Confirm
   incoming messages still arrive during HTTP waits, translations attach to the
   correct bubble after navigation, and no low-memory/TLS regressions occur.
4. Compare power consumption with the same brightness, antenna and Wi-Fi state.
   The idle helper should not prevent the existing screen-off CPU sleep.

Source regression checks and a firmware build cannot establish RF improvement
or verify sleep/wake on hardware. Do not interpret their success as a measured
sensitivity improvement.
