# CrowPanel Screen Power Saving

## Current Build: October 17 Promotion

The user-tested power-saving configuration is now the normal and only environment:
`dhe04005d-5inch-P4-meshcore`. The temporary test environment has been removed.
All application changes were shared already; this promotion transfers its exact
build flags and pinned AsyncTCP/ESPAsyncWebServer commits without upgrading them.

- CPU light sleep: 80 ms polling, or 250 ms with verified touch IRQ wake.
- Display-task backlight ownership with the minimum-brightness startup pulse.
- Awake-idle CPU scaling enabled; dynamic LCD timing and panel teardown disabled.
- Continuous boosted LoRa RX is unchanged. Hosted Wi-Fi still blocks CPU sleep
  when it has been used since boot; the change does not turn Wi-Fi off automatically.
- Debug information, quiet logging and exception decoding are retained.

November 3 stability update: scanout now uses direct PSRAM framebuffer DMA,
without the former 30-row bounce buffers. The prebuilt SDK's IRAM-safe RGB
bounce ISR could copy cached PSRAM during flash writes with caches disabled.
Timing, retained-panel sleep/wake, backlight handling and boosted LoRa RX are
unchanged. See [crash history](crash_history.md#rgb-bounce-isr-and-flash-writes-november-3).
Hardware testing must check for scanout artifacts and wake regressions; the
historical bounce-buffer and low-memory fallback notes below are not current.

Use `platformio run -e dhe04005d-5inch-P4-meshcore` to build, or the VS Code task
`MeshCore: Upload (COM3)` to upload. Debug/IntelliSense metadata is regenerated for
the normal environment. Selecting the normal build no longer selects the old
backlight-only configuration.

## Historical Investigation

Everything below records earlier tests and rollbacks. References to the retired
test environment and the former backlight-only default are historical, not
current build instructions.

Current status (October 16, after backlight fix): the user confirmed touch and
message wake at minimum brightness with the startup pulse. At their request,
the lowpower-test environment re-enables retained RGB pause and CPU sleep,
verified touch GPIO wake (250 ms timer, 80 ms polling fallback), and awake-idle
CPU scaling (180 MHz on the 360 MHz P4, 10 ms LVGL tick, up to 20 ms service).
RGB timing stays unchanged while awake; dark refresh/invalidation is paused.
The default environment remains backlight-only, with the same startup fix.
Wi-Fi must stay off from boot for CPU sleep and idle scaling; no automatic
Wi-Fi shutdown is introduced. LoRa boosted continuous RX is unchanged.
No power number or hardware wake success is claimed for this new combination.
Earlier investigation and rollback notes below are historical.

The later backlight-only trace also failed physical wake with continuous scan
and an acknowledged 5% PWM write. A diagnostic startup sequence now primes
PWM at 100% for 30 ms before restoring a nonzero sub-100% level, only after
off/unknown state (including boot). Normal brightness changes do not pulse;
saved brightness is unchanged. This tests a low-duty startup hypothesis and
may briefly flash the backlight. Hardware success is not yet established.

## Separate Low-Power Test: Display-Task Backlight

The stable backlight-only build now wakes on touch and new messages on the
user's hardware, at about 0.69-0.72 W with the screen off. Leave it as the
default profile. The new opt-in environment
`dhe04005d-5inch-P4-meshcore-lowpower-test` uses a separate build directory.
No change is made to `default_envs` or the stable profile's backlight behavior.
PlatformIO cleared the original cached build outputs on the configuration
change, so the stable profile was rebuilt too. Save both images and matching
ELFs under `.pio/diagnostics/power-tests-2026-10-15/`, outside the build cache,
with distinct `stable-backlight-only-...` and `lowpower-display-owner-...`
directories. The rebuilt stable image is not a byte-for-byte copy of the
previously flashed image; the default power behavior remains the same.
The test environment pins AsyncTCP/web-server commits to those in the working
build so creating the environment does not introduce a networking update.

This experiment enables retained-panel CPU light sleep with an 80 ms timer.
The October 16 log confirms a failed physical wake even though CPU sleep was
blocked by Wi-Fi, RGB resumed at 42 FPS, and the PWM restore returned success.
This is not evidence of a CPU-sleep crash or a missing PWM request. Do not
pause RGB at all when hosted Wi-Fi has made CPU sleep unavailable, or when a
panel/worker/safety gate prevents it. Keep normal scan, tick and touch service
for that whole dark session and restore brightness without a panel restart.
The log should say `RGB kept running: CPU sleep unavailable`; a Wi-Fi-on dark
report should show RGB running at about 42 FPS and tick 2 ms. Lower off-state
power is not expected in this fallback. The physical failure after a real
Wi-Fi-off CPU sleep still requires hardware testing; it is not claimed fixed.
Keep touch GPIO sleep wake, idle CPU scaling, dynamic LCD clock changes and
panel teardown disabled. LoRa boosted continuous RX and radio GPIO wake stay
unchanged. Wi-Fi must remain off since boot for CPU sleep to be eligible.

After initial setup, only the display task writes PWM in this test build:

- Sleep/wake and brightness callbacks publish the latest requested level.
  Concurrent requests during a write are picked up on the next service.
- Before RGB pause, write PWM zero and require I2C success. Do not pause RGB
  or enter CPU sleep with a failed/unknown backlight-off result.
- After RGB resume, keep the completed-redraw, two-VSYNC and 120 ms gate.
  Then restore the requested brightness directly from the display task.
  The main-loop wake ramp is disabled only for this test profile.
- Coalesce unchanged levels. Retry failed writes after one second; a newer
  target bypasses the previous target's retry delay. A failed write invalidates
  the cached physical state. Do not finish wake or allow sleep on a stale result.
- Log off/on write results and the first error in a failure episode, not every
  polling interval or every successful slider update. Battery I2C polling
  remains outside the LVGL mutex; the single PWM writer runs under it.

Build and monitor this environment, keeping the ELF matched to the flashed image:

```powershell
platformio run -e dhe04005d-5inch-P4-meshcore-lowpower-test
platformio device monitor -e dhe04005d-5inch-P4-meshcore-lowpower-test
```

The startup log must include `low-power TEST: 80 ms CPU sleep; display-task
backlight; no main-loop wake ramp`. Before sleep, expect `display backlight:
duty=0 write=ok`; after a valid wake frame, expect `display backlight: duty=5
write=ok` at minimum brightness (or the selected value). With Wi-Fi off, check
`CPU light sleep active: timer=80 ms`, and a paused RGB dark report. Test touch,
incoming messages with notifications enabled, short/long sleeps, minimum/mid
brightness and 30 wake cycles in each orientation. Then verify Wi-Fi, battery
readings and brightness changes after wake. Compare average USB-input watts
under the same conditions. No power saving or reliable wake is claimed until
these hardware tests pass. Recovery can disable RGB sleep for that boot and
raise off-state consumption; retain any recovery or backlight failure line.

To roll back, select and flash the original `dhe04005d-5inch-P4-meshcore`
environment/image. Do not erase NVS or change brightness/battery preferences.

## Current Default: Reliability Rollback And Debug Build

The October 15 trace shows successful touch wake, full-speed CPU, a completed
flush and resumed RGB at about 42 FPS, yet the physical display remains dark.
There is no exception/backtrace in that trace. Earlier wake recovery did not
resolve the physical failure. Its exact hardware cause remains unconfirmed.

Restore backlight-only sleep as the default: CPU light sleep, touch GPIO sleep
wake, idle CPU frequency scaling, dynamic display clocks and RGB teardown are
all disabled. RGB continues at the normal 18 MHz clock, CPU stays at its boot
frequency and LVGL keeps its 2 ms tick and normal servicing while dark. No
LCD/DMA pause/restart or sleep-related GPIO reconfiguration occurs by default.
Restore saved brightness directly on a touch/notification wake; do not wait for
the main-loop brightness ramp or a fresh-frame gate. Failed PWM writes retain
the retry path. Battery smoothing, all-tab indicators, brightness persistence,
LoRa boosted RX and notification preferences are unchanged.

The boot profile must say `backlight-only (RGB continuously running)` with
CPU light sleep, idle CPU scaling and touch GPIO sleep all `0`. A wake prints
one `backlight wake: duty=... write=ok` line. Screen-off RGB must remain running
at approximately 42 FPS, CPU `360/360 MHz`, tick `2 ms`, with no `CPU light
sleep active` line. Screen-off watts will rise relative to CPU/display sleep;
reliable wake takes priority. Test repeated touch and incoming-message wakes
with notifications enabled, at minimum and mid brightness in both orientations.

The existing default PlatformIO environment now uses `build_type = debug`
with `-Og -g3 -ggdb3`, following the
[PlatformIO build configuration guide](https://docs.platformio.org/en/latest/projectconf/build_configurations.html).
Use the matching freshly built ELF for decoding. No generic RadioLib/core
debug stream is enabled. This produces application debug symbols, not a
recompiled debug version of the prebuilt ESP-IDF libraries. Hardware tests
are still required after flashing.

Sections below document superseded opt-in experiments, not the stable defaults.

## Dark-Screen Wake Recovery

The reported failure affected touch and incoming-message wake, but no failing
serial trace was available. The code had unbounded waits for a quiet paused
scan and a successful first redraw; a failed PWM write could also finish the
brightness ramp without lighting the panel. Harden these paths without
claiming which one occurred on the device:

- Retry a missing first redraw every 250 ms. Preserve the successful-frame,
  two-VSYNC and 120 ms backlight gates; do not blindly light a stopped panel.
- Bound the pre-resume scan-settle wait at 1.5 seconds. Cancel a stale pause
  without truncating a running scan. Reinitialize a stopped scan on its IRQ
  owner core as before, retaining all buffers and normal 18 MHz timing.
- If wake is still pending after 1.5 seconds, request the driver's VSYNC
  restart when frames are moving. With no VSYNC progress, reinitialize on the
  owner core instead: a VSYNC-queued restart cannot execute without VSYNC.
  Keep the backlight off until the recovered redraw and scan pass the gates.
- A recovery disables further RGB pause/CPU light sleep for that boot and
  emits one diagnostic per failed wake, not one per polling interval. This
  safety fallback can increase screen-off watts until reboot. Healthy sleep
  cycles keep the established low-power path unchanged.
- Keep a verified touch GPIO wake even if its short interrupt pulse or touch
  coordinates are gone by the next poll. Swallow the wake touch normally;
  radio-only wake never counts as touch or overrides notification preferences.
- Notify LVGL immediately when software requests screen wake. Block the idle
  timeout until the backlight gate is ready, and reject a negative elapsed
  time from a concurrent newer touch/notification timestamp.
- Advance/finish the backlight ramp only after a successful STC8 PWM write;
  retry a failed write after one second instead of leaving the PWM at zero.

Policy checks cover retry boundaries, rollover, concurrent timestamps, short
touch GPIO pulses and unchanged frame/VSYNC gates. Hardware validation still
requires repeated short/long sleeps and wake by touch and incoming messages
(notifications enabled), with Wi-Fi both off and on, in both orientations.
Check minimum and mid brightness, confirm normal input/radio handling, and
check for tearing. If recovery activates, retain the single `[power] wake
recovery` or `scan pause did not settle` line with the sleep/wake reports.

## Current Default: CPU Sleep With Retained Screen Buffers

Hardware measurements reported 0.75 W with backlight-only sleep versus 0.45 W
with the earlier combined CPU/display sleep mode, but that earlier mode caused
wake deformation. The retained-buffer mode subsequently measured 0.47-0.55 W,
but occasional wake distortion remained. Frame-boundary pause and serialized
restart subsequently fixed waking on the user's hardware. Keep that confirmed
display sequence, CPU sleep mode, normal clocks and retained buffers unchanged.
The reduced idle wake/tick frequency subsequently measured 0.41-0.53 W on the
user's USB-input meter. These are whole-board readings, not isolated CPU power.
Compare averages under the same conditions rather than the lowest sample.

The latest experiment adds GT911 GPIO wake and permits 250 ms CPU idle sleep
intervals only after observing the configured INT level during a genuine touch
and its opposite idle level. Its whole-board wattage has not yet been measured.
No display clock, panel pause/restart, memory-domain or RX gain change is made.

## Battery-Only Cold Boot

Corrected the STC8 charge-state mapping against
[Elecrow's 5-inch Lesson09 driver](https://github.com/Elecrow-RD/-CrowPanel-Advanced-5inch-ESP32-P4-HMI-AI-Display-800x480-IPS-Touch-Screen/blob/master/example/V1.0/Arduino_Code_5inch/Lesson09-LVGL_Lighting_Control/bsp_stc8h1kxx.h):
idle=0, charging=1, full=2, not charging=3, error=4. The previous 0-2 gate
discarded battery-only state 3, leaving every header indicator hidden. Plausible
voltage now drives the estimate independently of charger state; only states
1/2 count as charging. The existing wake/load filtering remains unchanged.

All headers show the battery outline and `--%` until a valid voltage arrives,
not a fabricated 0% or a blank space. Read once before the first startup frame;
retry every second until a reading is accepted, then return to five-second polls.
I2C remains outside the LVGL mutex. Cold-boot on battery without USB, check all
tabs, then test USB charging/unplugging and repeated display sleep/wake. Hardware
verification is still required after flashing.

## Persistent Brightness

Brightness is stored in NVS `ui/brightness` when the slider is released, not
on every drag update. Releasing it at an already saved level does not rewrite
flash. The saved 5-100% PWM level is loaded before the first boot backlight
command, and the Settings slider is restored to a matching position. Screen
sleep's zero PWM is never saved; waking retains the selected level and existing
ramp. Missing/invalid stored values retain the previous 100% default. Check
minimum, middle and maximum settings across power-off/restart and sleep/wake.

## Screen-On Idle CPU Experiment

The user confirmed the `awake idle` serial line, but screen-on USB input still
measured about 0.8 W at low brightness. CPU scaling activates; this measurement
does not establish a meaningful whole-board saving. LCD timings remain unchanged.

With Wi-Fi off since boot, a stationary screen can now run at 180 MHz instead
of 360 MHz (200 instead of 400 MHz on production P4). Only CPLL /2 is used;
other boot clock configurations are left unchanged. The installed
[ESP-IDF clock implementation](https://github.com/espressif/esp-idf/blob/v5.5.2/components/esp_hw_support/port/esp32p4/rtc_clk.c)
keeps memory/system/APB clocks unchanged for these divider pairs. Runtime
checks also require APB frequency to remain unchanged, otherwise the
optimization is disabled for that boot and the full CPU clock is requested.

Idle needs five seconds without touch or a screen transition, two seconds
without rendering, no pending screen wake, no animations, and idle audio.
Pending invalidation restores full speed before servicing LVGL; touch and
flush callbacks also restore it before event dispatch or framebuffer transfer.
Network transport use blocks this conservative mode for the rest of that boot,
including translation/OTA. Audio activity keeps full speed at the next UI service.
Active maps, animations and keyboard cursor redraws may also keep full speed.

LVGL tick callbacks change from 2 to 10 ms while idle; UI service waits are
bounded at 20 ms instead of 10 ms, respecting earlier timer deadlines. Elapsed
time remains monotonic. There is no screen-on light sleep, change to brightness,
LCD pixel clock/scan timings, RX gain, or the existing dark-screen sleep sequence.
Full CPU speed is restored before every display sleep/wake transition.

After flashing, disable Wi-Fi and reboot, then leave a stationary screen on
for at least eight seconds. Check the one-shot `awake idle: CPU=180 MHz;
tick=10 ms; LCD timing unchanged` serial line. Compare average watts with the
same screen, brightness and radio conditions. Test typing, text selection,
map panning, incoming messages/audio, Wi-Fi/web/translation and 30 sleep/wake
cycles in both orientations. Build/policy checks cannot establish real power
savings or rule out hardware scan artifacts.

Rollback only this screen-on experiment with `-D CROWPANEL_AWAKE_IDLE_POWER=0`;
the established screen-off light sleep and display restart remain unchanged.
The historical dynamic screen-clock mode disables this experiment automatically.

## Additional Screen-Off Experiment: Touch GPIO Wake

- Read `Module_Switch1` at `0x804D` twice at touch initialization to obtain the
  existing interrupt polarity; do not rewrite the touch configuration. The
  mapping follows the [Goodix programming guide](https://www.lcd-module.de/fileadmin/eng/pdf/zubehoer/GT911_Programming_Guide_Rev.10.pdf).
- Enable longer CPU sleeps only after normal touch reads have observed a real
  press with the matching active INT level, and the opposite level while idle.
  An unverified/failed touch read retains the original 80 ms timer fallback.
  Repeated active INT with an empty touch buffer revokes verification to avoid
  a stuck-line wake loop. GPIO setup failure disables this optimization for
  the current boot without disabling the established LoRa sleep path.
- Arm touch GPIO42 as a second GPIO light-sleep wake source using the verified
  level. Keep LoRa DIO1 wake, continuous boosted RX and all existing sleep gates.
  An already-active touch is serviced immediately instead of entering sleep.
- Timer-only idle sleep can now last 250 ms, about four periodic idle wakes per
  second instead of 12.5, before sleep entry/exit/application overhead. Incoming
  LoRa and touch GPIOs can end a sleep early. The timer remains a backup, not a
  promise that touch GPIO wake works on every panel. GT911 repeats its interrupt
  while its coordinate buffer remains unread; a pulse ending before the host
  resumes does not clear that buffer.
- Keep LVGL awake ticks at 2 ms and dark ticks/polling at 80 ms. During explicit
  CPU sleep, ticks catch up by elapsed time and the LVGL service is notified on
  return. When CPU sleep is blocked (Wi-Fi, audio, pending packets, recovery),
  the established 80 ms touch polling remains responsive; there is no always-on
  touch CPU ISR and no new periodic debug spam.
- The flash/PSRAM/LCD engine registers and peripheral power domains stay as in
  the confirmed display fix. This changes idle CPU activity, not the radio or
  touch controller's power mode.

After flashing, disable Wi-Fi and reboot, tap/use the UI once, and let the
screen turn off. Check for `timer=250 ms` and `touch=GPIO wake` in the one-shot
CPU sleep line. `timer=80 ms` with `touch=polling fallback` means the validation
has not qualified this hardware or a fallback was triggered. Measure average
USB-input watts under the same conditions as the previous 0.41-0.53 W readings.
Test brief taps, held touches, incoming messages/ACKs with notifications on/off,
audio, and at least 30 screen wake cycles in both orientations. Hardware tests
are still required; no extra wattage reduction or artifact-free wake is claimed.

Rollback just this experiment with `-D CROWPANEL_TOUCH_IRQ_SLEEP=0`; the previous
80 ms CPU sleep and working panel lifecycle remain. The extended interval can
be overridden with `CROWPANEL_TOUCH_IRQ_SLEEP_MS` in the bounded 80-500 ms range;
the default is 250 ms. The separate polling-only `CROWPANEL_CPU_SLEEP_MS` keeps
its existing 20-80 ms range.

## Unchanged Display And Radio Sequence

- Normal scanout timing remains 18 MHz. CPU frequency remains at its boot
  setting (360 MHz on this ES P4) between light-sleep intervals.
- While dark, disable auto-next-frame using the HAL and latch that update.
  Let hardware finish the entire frame; the VSYNC callback confirms that the
  engine stopped. Never clear LCD_START partway through a frame. This preserves
  the driver's alternating bounce-buffer phase across sleep.
  Keep the panel handle, framebuffers, bounce buffers, DMA descriptors and all
  UI state allocated at the same addresses. No driver-private layout is used.
- CPU sleep requires LCD_START to be clear and at least 60 ms with no new
  VSYNC interrupts. Unsafe/unconfirmed PPA transactions block scan pause and
  CPU sleep. Audio and radio work retain the existing mutex/IRQ safeguards.
- A small blocked worker on the core which allocated the RGB/LCD DMA interrupts
  applies the pause request and wake restart. A short local critical section
  prevents those ISRs racing the driver's GDMA reset and bounce-buffer prefill.
  The worker never takes LVGL's mutex and uses its own 4 KB stack, not the SDK's
  small IPC stack. It performs no allocations during sleep/wake transitions.
- Wake waits for the confirmed, quiet pause and calls public
  `esp_lcd_panel_init()` on the same panel. The checked IDF
  5.5 driver restarts GDMA/LCD from the beginning and refills its bounce buffers;
  it does not allocate new framebuffers. Its normal VSYNC resynchronization
  remains enabled. A compile-time guard requires reviewing this lifecycle
  before moving to a different IDF major/minor version.
- Rapid wake still waits for the final frame rather than resetting active DMA.
  If pause remains unconfirmed after 250 ms, cancel it on the interrupt core,
  restore scanning and disable further CPU sleep for that boot with one warning.
  Worker allocation failure also falls back without pausing scanout.
- Resume LVGL invalidation, request a full redraw, and keep the backlight off
  until a successful rendered frame, two later VSYNCs and 120 ms of settling.
- LVGL keeps its normal 2 ms tick while awake and uses an 80 ms periodic tick
  while dark. Its task is serviced every 80 ms while dark and explicitly
  notified after each CPU sleep interval. Each dark service also advances the
  tick using monotonic elapsed time, so a radio wake need not wait for the next
  tick. Timer/task tick writers are serialized to prevent duplicate/backwards
  increments. No replay of missed 2 ms callbacks is needed.
- The polling fallback CPU wake interval remains 80 ms instead of 20 ms: about 12.5
  rather than 50 idle intervals per second before entry/exit/application work.
  LoRa DIO1 still wakes immediately. Without qualified GPIO wake, touch is polled and may wait up
  to one idle interval plus normal service/redraw latency; its GPIO interrupt
  polarity is not assumed. All held radio packets, including delayed inbound
  work, block CPU sleep until processing finishes.
- Keep the 5-100% brightness range, gradual wake ramp, swallowed first touch
  and SX1262 continuous boosted-gain RX. CPU wake does not necessarily wake
  the screen when message notifications are disabled.

The LCD scan must pause during CPU sleep; keeping it actively scanning is not
safe. This approach retains its timing configuration and memory instead of
changing pixel clocks or destroying it. The implementation relies on the sole
P4 RGB engine and the inspected [IDF 5.5.2 RGB driver](https://github.com/espressif/esp-idf/blob/v5.5.2/components/esp_lcd/rgb/esp_lcd_panel_rgb.c),
matching the installed SDK version. The retained engine/channel registers and
flash/PSRAM supply domains are not powered off by the CPU sleep path.

Disable Wi-Fi and reboot before testing; do not scan/connect Wi-Fi afterward.
The hosted SDIO transport still blocks CPU sleep once used during that boot.
Serial should show `CPU light sleep active` followed by a one-shot dark report
with `RGB=paused request=18000000 Hz`, zero FPS and CPU `360/360 MHz` (frequency
between sleeps) and `tick=active/80 ms`. The sleep-active line should say
`timer=250 ms` with verified touch GPIO wake, or `timer=80 ms` in polling fallback.
Wake should report `RGB=running` near 42 FPS, `tick=active/2 ms` and a CPU sleep
summary. No per-interval logs are emitted.

Measure screen-off watts under the same conditions, test incoming messages,
ACKs and audio with notifications on/off, and perform 30 touch-wake cycles in
portrait and landscape using chats, keyboard and map. Confirm no deformation
and no loss of contacts/messages. Recheck Wi-Fi/web/translation after wake.
No specific wattage or artifact-free wake is guaranteed without these tests.
The radio, LCD electronics and board supplies stay powered. This optimization
reduces time spent entering/leaving sleep and doing idle CPU work, not those
loads, and does not change the sleep power-domain configuration.

Rollback to continuous-scan/backlight-only sleep with
`-D CROWPANEL_CPU_LIGHT_SLEEP=0`. Dynamic clock changes remain disabled with
`CROWPANEL_DYNAMIC_SCREEN_POWER=0`; panel deletion remains disabled with
`CROWPANEL_SUSPEND_RGB_WHEN_DARK=0`.
For a cadence-only A/B comparison, use `-D CROWPANEL_CPU_SLEEP_MS=20` (accepted
range 20-80 ms). This retains the fixed display restart and dark tick coalescing.

## Historical Experiments (Not Active By Default)

The following notes describe previous teardown/clock-change implementations,
not the current retained-buffer build.

Brightness now spans 5-100% STC8 backlight duty. Zero duty is reserved for
screen-off. This historical implementation did not persist brightness.

After the screen timeout, the dedicated LVGL task applies these changes under
its mutex:

- Disable display invalidation and pause the refresh timer. LVGL invalidation
  otherwise resumes a paused refresh timer automatically.
- Service touch and application timers every 40 ms instead of at most 10 ms.
- Change the LVGL tick timer from 2 ms to 20 ms, using elapsed monotonic time
  so discovery, animation and other deadlines do not drift.
- Reduce the CPU from 360 to 180 MHz on the ES P4 (400 to 200 MHz on the
  production P4). This CPLL divider change preserves MEM, SYS and APB clocks.
  Other initial CPU configurations are left unchanged.
- Stop RGB scanning using the public `esp_lcd_panel_del()` lifecycle API.
  This releases the LCD peripheral, DMA channel, interrupts, bounce buffers
  and RGB scan framebuffers. LVGL's separate draw buffers and widget state
  remain intact. This does not disconnect power from the physical LCD.

Wake restores CPU speed and recreates the RGB panel with the original timings
and 18 MHz pixel clock. All scan framebuffers and their scan-offset borders
are cleared before starting DMA; portrait framebuffer selection resets to
index zero. Invalidation resumes and a full redraw is requested. The backlight
ramp waits for a successful rendered frame, two subsequent VSYNC events and
at least 120 ms of settling. Brightness-slider changes respect this gate.
The first touch is still swallowed. RGB recreation adds some wake latency;
its duration must be measured on hardware.

On allocation failure, creation tries smaller 10-row bounce buffers instead
of the normal 30 rows. If setup still fails, the backlight remains off and
recreation retries at most once per second, reporting the first failure rather
than printing on each retry. No firmware reset is requested. Smaller bounce
buffers can be more susceptible to starvation during heavy redraws.

For an A/B comparison or rollback, add
`-D CROWPANEL_SUSPEND_RGB_WHEN_DARK=0` to this environment's build flags. This
retains the old 18-to-9 MHz clock reduction without teardown. If suspension
fails or the LVGL draw buffers alias scan framebuffers, the same slower-scan
fallback is used rather than freeing buffers owned by LVGL.
After a PPA timeout or queue failure, scan framebuffers are retained for the
rest of that boot and slower scanning is used. This avoids freeing a buffer
that an unconfirmed/late PPA DMA transaction could still target.

Main-loop contact-list rebuilds and visual status updates are coalesced while
dark. Incoming message queues, persistence, acknowledgements, radio polling,
discovery timers and Wi-Fi services remain active. LoRa RX boosted gain stays
enabled. There is no deep sleep, receiver duty cycling or Wi-Fi shutdown.

## CPU light sleep with continuous LoRa RX

Uses the public APIs documented in the
[ESP-IDF P4 sleep guide](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32p4/api-reference/system/sleep_modes.html).

The main loop now requests explicit ESP-IDF light sleep in intervals of at
most 80 ms while dark. SX1262 stays in continuous receive, with boosted gain
unchanged. Its latched DIO1 signal can wake the CPU earlier; the timer fallback
services touch, application deadlines and persistence. Wi-Fi must remain off
since boot, including no Wi-Fi scans. Once the hosted SDIO transport has been
used, sleep is blocked for that boot even after disconnecting: disconnect is
not a transport shutdown, and the existing stop/restart path is unreliable.
Disable Wi-Fi and reboot before measuring this mode.

Sleep also requires confirmed stopped RGB scanning, no unsafe PPA transaction,
no held radio packets, and the radio's exact RX state without a pending IRQ.
The main loop takes the existing LVGL and mesh mutexes, then an audio-idle
mutex. No audio DMA or application I2C operation can be active in this window.
SPI/peripheral and flash/PSRAM supply domains are retained; no device state or
radio FIFO is cleared by the sleep path.

GPIO wake setup changes the GPIO interrupt type. The existing radio CPU ISR
is temporarily masked before level wake is configured, then restored to its
normal rising-edge mode immediately afterward. An RX DIO asserted during
that masked window is latched into RadioLib's existing receive flag before
the next mesh loop. No replacement ISR is installed. A rejected sleep request
is normal if a packet arrives during entry; other failures disable CPU sleep
for the remainder of that boot. The previous dark-screen mode remains available.

Manual sleep does not assume FreeRTOS delay ticks catch up. On return, the UI
task is explicitly notified, while LVGL's elapsed-time tick and MeshCore's
millisecond deadlines use the monotonic ESP timer. Touch remains polled; the
GT911 interrupt polarity is not changed. The 20 ms sleep cap is not a guarantee
of end-to-end wake latency, which also includes scheduling and RGB recreation.

Serial prints one `CPU light sleep active` line after the first successful
interval, a summary on screen wake (`intervals`, total API elapsed milliseconds,
GPIO radio wakes and rejected entries), or one blocked/error message. Elapsed
time includes entry/exit overhead, not just clock-gated time. There is no log
per interval. The existing CPU MHz report describes the frequency when awake
between sleeps, not uninterrupted CPU operation while dark.

For rollback/A-B measurements, add `-D CROWPANEL_CPU_LIGHT_SLEEP=0` to the
environment build flags. CPU sleep is also blocked if RGB suspension is disabled.
After flashing, confirm the active message, compare dark-screen USB watts with
Wi-Fi off since boot, send multiple incoming messages with notifications both
off and on, verify stored history/ACKs/discovery, test notification audio and
30 touch-wake cycles in both orientations. Then enable Wi-Fi, verify the
blocked message and normal web/translation behavior. Reception and power
savings require hardware verification; this is not deep sleep.

## Verification

Build the firmware and compile the constexpr policy checks:

```powershell
platformio run -e dhe04005d-5inch-P4-meshcore
riscv32-esp-elf-g++ -std=c++17 -Wall -Wextra -Werror -fsyntax-only test/test_screen_power_policy/test_screen_power_policy.cpp
```

Hardware checks still required after flashing:

1. Measure USB input watts at minimum brightness and after screen timeout,
   with no battery charging and the same Wi-Fi/radio conditions as before.
2. Sweep brightness across its range; check readability and flicker at 5%.
3. Repeat sleep/touch-wake at minimum and maximum brightness in both screen
   orientations. Check that wake touches never activate underlying controls,
   that the keyboard/map/chat state survives, and that no tearing, black
   frames or wrapped borders appear after wake. Repeat at least 30 cycles.
4. Receive messages with notifications both enabled and disabled while dark.
   Verify notification wake, stored history and unread counts.
5. Start discovery just before screen timeout; verify the scan completes and
   results remain available on wake. Confirm direct-message acknowledgements
   and incoming radio messages while `RGB=stopped`.
6. Check map, keyboard, web dashboard and translation after repeated wake.
   Clock/timer failures emit a single error per failed transition.

Two seconds after boot and each sleep/wake transition, one `[power]` serial
line reports actual/requested CPU MHz, requested RGB pixel clock, measured
VSYNC frames per second, refresh-timer state, flush calls during the measurement
window, whether the LVGL tick timer is active, and free/largest internal and
PSRAM heap blocks in bytes. There is no periodic output.
While dark, expect CPU `180/180 MHz` on the ES P4, `RGB=stopped request=0 Hz`,
`redraw=paused`, zero flushes, and `scan=0.0 fps`. After wake, expect
`RGB=running` and approximately 42 fps. With the rollback flag, expect about
21.5 fps while dark instead. Repeated wake tests should show stable free and
largest internal/PSRAM blocks; reallocation behavior has not been hardware
validated yet.

USB measurements under unchanged conditions showed about 1.2 to 0.8 W at
minimum brightness, but 0.72-0.78 W while dark versus about 0.75 W before.
That confirms lower minimum-screen-on consumption, not a measurable reduction
in screen-off consumption with the previous slower-scan implementation.
Wi-Fi-disabled measurements remained 0.72-0.75 W. Full scan suspension was
confirmed at zero dark-screen FPS and measured at 0.69-0.72 W. CPU light sleep
has not yet been measured on the board. Check the transition reports before attributing the
remaining load to a specific component. The LCD, touch
controller, C6, SX1262 and onboard regulators still have idle consumption.

## October 16: hardware wake failure, experimental profile withdrawn

The Wi-Fi-off trace confirms CPU sleep and radio wake, followed by restored
VSYNC (~42 fps) and an acknowledged PWM write, but the user reports a physically
dark screen. These software counters cannot validate panel illumination.
The lowpower-test environment now selects the same backlight-only policy as
the stable environment (CPU sleep and display-task PWM disabled). Its name is
retained for existing VS Code selections, not as a claim of lower consumption.
This rollback sacrifices the 0.4-0.5 W sleep target to restore the previously
working lifecycle. The retained-panel hardware fault remains unresolved.
After uploading, use the restart method that restores the display before
testing touch/message wake. Upload success alone does not prove a cold reset.
