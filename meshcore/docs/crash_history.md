# Persistent reboot history

## Factory-slot boot recovery

`src/crowpanel_dualboot.cpp` must validate the entire factory image with its
actual partition size before clearing OTA selection records. A magic-byte-only
check is insufficient. A bounded header/segment-length preflight must run before
the framework image verifier: malformed lengths can otherwise make that verifier
hash out-of-partition data long enough to trigger the early-boot task watchdog.
On 2026-10-31 the device's factory slot contained an old
image whose segment extended beyond the 2 MB factory partition into `ota_0`.
The complete current MeshCore image at `0x210000` verified against the build,
but blank OTA records sent boot to the invalid factory image and triggered a
bootloader `verify_load_addresses` assertion before application startup.

The guard preserves OTA selection if the factory image is missing, oversized,
or fails image verification. A valid factory selector retains normal dual-boot
behavior. This prevents the boot trap; it does not diagnose the original reset.
The intended scoped recovery updates `otadata` (`0xe000`, 8 KB), after backing
it up, then installs the guarded MeshCore app in `ota_0`. Neither filesystem
needs erasing. The invalid factory selector needs separate restoration before
selector-based switching can be used again.

### Recovery Incident On 2026-10-31

ROM-only (`--no-stub`) flash-write attempts during recovery unexpectedly cleared
regions outside the requested writes, including bootloader/partition-table/NVS
and both filesystem partitions. The exact offending command/mechanism was not
isolated. The original 64 KB boot/NVS backup was restored using the standard RAM
flasher stub, with OTA selection pointing to MeshCore; both restored metadata
and the guarded app passed device digest verification. MeshCore reached UI-ready
and retained OTA selection across the following reset. On that subsequent boot,
LittleFS mounted its newly initialized filesystem.

The filesystem backup was taken too late: both original data partitions already
contained only 0xFF bytes. It is not a usable data backup. The first recovered
boot formatted the erased MeshCore filesystem. No earlier backup was available;
contacts, chat history, saved channels and the filesystem-backed MeshCore identity
cannot be restored from the captured files. NVS was restored from its original
pre-write backup. This was a recovery failure, not evidence that user data had
already been erased by the reported crash.

Do not use the ROM-only write path again on this board. Resolve RAM-stub
connectivity first, capture and validate a complete flash backup before any
recovery write, and verify preserved regions as well as the requested target.
Local captures are under ignored `logs/boot-recovery-*`; NVS captures contain
private settings and must not be published. No further device writes were made
after the loss was confirmed and the user reported having no earlier backup.

The original runtime crash and missed-message issue remain unresolved by this
boot guard. Other firmware slots need separate verification/restoration before
claiming dual-boot recovery is complete.

## Runtime Evidence

### RGB Bounce ISR And Flash Writes (November 3)

The October 31 17:27:32 build (`8835ce54...`) recorded watchdogs at chat
context 500 and contact context 203 while LVGL was handling timers or pruning
a history file. Storage context 4863 in that report is the chat file size passed
to the pruning breadcrumb, not an error code. These reports are newer than the
initial queue-mutex fix; no panic backtrace was captured.

The installed SDK is IDF `f56bea3d1f`, with `CONFIG_LCD_RGB_ISR_IRAM_SAFE=y`,
flash auto-suspend disabled and no PSRAM XIP. The application enabled 30-row
PSRAM-backed RGB bounce refills. The matching
[driver's refill function](https://github.com/espressif/esp-idf/blob/f56bea3d1f/components/esp_lcd/rgb/esp_lcd_panel_rgb.c#L823-L838)
copies framebuffer bytes from PSRAM in the DMA ISR and explicitly warns about
cache-disabled crashes. The IRAM-safe ISR can run during those flash operations.
This is a concrete unsafe configuration consistent with the reports, not a
captured proof of which instruction caused each watchdog reset.

RGB now uses direct framebuffer DMA (`bounce_buffer_size_px=0`) instead of an
ISR that copies cached PSRAM. Pixel clock, porch/sync timing, two framebuffers,
CPU sleep and radio settings remain unchanged. No SDK/private flash hooks,
watchdog disabling, formatting or partition changes are introduced. Direct DMA
removes the former 96 KB internal bounce allocation. Verify redraws, map/keyboard
activity, incoming messages, Wi-Fi/translation, and repeated sleep/wake in both
orientations on hardware; different scanout behavior can expose visual artifacts.
The boot marker is `display_init: RGB direct DMA (no PSRAM bounce ISR)`.

Every chat flush now takes the LVGL lock before the pending-queue mutex, not
only the timed/notification call sites. Full-queue enqueue releases queue
ownership before attempting a flush, and the deadline check takes no queue lock.
A busy render lock returns after at most 5 ms and leaves queued records intact.
Overflow remains bounded: a new line cannot be retained if the queue is full
and its forced flush cannot run. Contact saves release mesh ownership before
acquiring the same LVGL lock; acquisition failure schedules the existing retry.
This serializes contact replacement against UI history pruning and display
transitions, but does not claim to serialize every NVS or filesystem caller.

Additional breadcrumbs: 506 waiting for render gate, 507 queue acquired;
contact replacement 210 verify temp, 211 remove old backup, 212 check original,
213 rename original to backup, 214 install temp, 215 rollback, 216 remove temp,
217 installed. All are retained-RAM markers without continuous serial output.

### Chat-flush Concurrency

A later watchdog report (`0e9af340...`, uptime 3431833 ms) stopped at main
storage context 5 and LVGL flush context 0. Those are last-observed operations,
not a captured fault location. The main/UI tasks shared pending chat Strings
without a queue mutex, and the timed main-loop flush did not hold the render
lock. This is a concrete race risk, not proof of the watchdog's cause.

The queue now has a statically allocated recursive mutex, initialized before
radio callbacks/UI service. Its deadline is atomic. The timed flush also uses
the existing LVGL lock, matching notification-triggered flushes. This does not
serialize every filesystem/NVS operation with all rendering or RGB scanout.

Chat writes stage at most 128 bytes through internal task-stack RAM. Failed
opens keep their pending records (no fallback to truncating FILE_WRITE).
Short writes retain a byte offset, back off for one second, and compact pending
slots to preserve per-chat FIFO ordering on retry. Unread metadata stays dirty
until the pending text flush succeeds. RAM queue overflow is still bounded;
when flushing cannot free space, the new line is rejected with a monitor warning.
This is not an atomic power-loss transaction or an error-checked close operation.

Chat storage subphases: 500 waiting for queue mutex, 501 append-open,
502 staged write, 503 close, 504 drained, 505 retry pending, 506 render gate,
507 queue acquired. Display flush
subphases: 1 landscape draw, 2 portrait framebuffer submission, 3 flush-ready;
1000 + source-row records CPU-rotation progress every 64 rows. Markers only
update retained RAM; they introduce no continuous serial traffic.

`python tools/test_chat_persistence.py` executes the production queue methods
under RV32 emulation with filesystem/String/RTOS mocks. It covers batching,
overflow flushing, render-before-queue lock ordering, busy-render deferral,
deadlines, open failures, partial writes and retry ordering. It does not simulate concurrent task scheduling, hardware caches,
actual LittleFS durability or a watchdog reset. Device stress testing is needed.

The stability test now uses CPU portrait rotation by default, bypassing PPA
registration/submission/waits. Early-boot watchdog reports stopped at ppa-wait;
this A/B change isolates that path without claiming it proves the root cause.
Portrait redraw performance/power can differ; LCD timing and RF settings are
unchanged. CROWPANEL_PPA_ROTATION=1 explicitly restores accelerator testing.
Check portrait orientation, complete redraw, touch and sleep/wake on hardware.
Reset reason 9 is separately reported as BROWNOUT; do not disable the voltage
detector. Zero early-boot heap/stack fields can mean sampling has not run.

Radio contexts: 0 inside MeshCore loop, 1 loop returned, 2 RX-gain verification,
3 verification returned. Render contexts: 0 entry/sample, 1 backlight, 2 screen
power transition, 3 wake recovery, 4 idle CPU/tick scaling, 5 LVGL timer handler,
6 post-handler reporting/backlight. Flush/PPA markers still supersede Render
inside those callbacks.

Per-core last-SPI phases: 0 not sampled, 1 waiting for transaction mutex,
2 acquired, 3 in transmit, 4 transmit returned, 5 released. Phase 3/4 includes
the command opcode and current FIFO chunk size. This is last-observed activity
per core, not a backtrace or proof that an operation was stuck. These updates
only write retained RAM: no logging, allocation or extra flash writes.
The retained record version changes with the new layout; old-format RAM is
ignored on first upgrade, while previously saved NVS reports are preserved.

Storage contexts: 1 notifications, 2 contacts, 3 UI preferences, 4 map viewport,
and contact-save subphases 200 open, 201 batched write, 202 close, 203 verify/replace;
5 chat append flush. Storage is now marked only around an actual save; older
builds left this marker active even without pending writes. Sleep marks the
CPU light-sleep polling function (which can also return without sleeping).
These markers identify the last activity, not necessarily the root cause.

Notification-save subphases: 101 NVS open, 102 contact unread counts,
103 channel unread counts, 104 chat read positions, 105 NVS close.
The notification snapshot now uses internal heap memory rather than about
4 KB of main-task stack, with a private Preferences handle. Snapshot copying
is protected by the LVGL lock; NVS operations run after releasing that lock.
Failed allocation/open/write keeps notification metadata pending for retry.

Pending chat text is flushed before saving unread metadata. Previously a
reboot between saving a badge and the deferred text flush could restore
"1 new" against the last old message while losing the buffered new message.
This changes ordering, not transaction atomicity: file-write failures and
power loss can still interrupt persistence. Already lost text cannot be
reconstructed from an unread count. Test reception and reboot persistence
on hardware; the breadcrumbs alone do not prove the watchdog cause.

Every MeshCore startup saves a bounded report to a four-slot NVS ring in the
`crashlog` namespace. Nothing is written to flash from the panic hook or main
loop. Normal reboots are recorded too, and must not be interpreted as crashes.
Boot-looping devices can eventually overwrite earlier evidence in this ring.

After the next unexpected reboot, retrieve `http://DEVICE_IP/api/crash`, or
connect serial at 115200 and send `crash` followed by Enter (newline enabled).
The HTTP endpoint is read-only and belongs on a trusted LAN like the dashboard.
It reads a boot-time RAM snapshot, not NVS from the web task. Older reports can
survive a later full power cycle, provided their NVS writes completed.

Reports contain current build and ELF SHA256, app reset reason, and previous
task stages, uptime, stack watermarks, heap and panic registers when retained.
The selector may mask the original reset reason or overwrite retained RAM;
hard power loss can erase it. Missing registers do not establish a brownout.
No flash core-dump partition is added and this is not a complete backtrace.

Preserve the firmware.elf matching a report's build for address decoding.
Build identity on each entry identifies the reporting app; previous build
identifies the retained activity. Reports may include both normal restarts and
exceptions. Test serial retrieval with Wi-Fi disabled as well as HTTP after
enabling Wi-Fi, and test power-cycle persistence without purging/erasing NVS.
