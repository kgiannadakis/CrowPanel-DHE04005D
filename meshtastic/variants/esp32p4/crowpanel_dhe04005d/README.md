# CrowPanel DHE04005D — Meshtastic (ESP32-P4) variant

**Status:** scope (A) — full mcui UI + radio + WiFi working on hardware (2026-04-26). SX1262 LoRa TX/RX confirmed against a second node, public LongFast and PKI direct messages both working. GT911 touch, LVGL 9 status bar / chat / nodes / settings tabs, tap-to-wake backlight, settings save persistence — all verified. WiFi via ESP-Hosted SDIO link to the on-board C6 connects, scans, joins, and obtains an IP; NTP sync over UDP works. DS3231 RTC restores wall-clock at boot. Auto-TZ (HTTPClient → worldtimeapi.org) is disabled on this build — see "WiFi via ESP-Hosted" section below for why.

This variant ports the lessons from the sibling `meshcore/variants/crowpanel_dhe04005d/` build into Meshtastic firmware. The display does **not** go through `meshtastic-device-ui` (which is built on LovyanGFX, no P4 RGB driver). Instead we drop in a P4-native `McDisplayP4.cpp` that uses `esp_lcd_panel_rgb` directly, and let the rest of mcui (status bar, tab bar, chat view, nodes, maps, settings, keyboard) layer LVGL 9 widgets on top.

Build env: `dhe04005d-5inch-P4-meshtastic`.

---

## What works

| Subsystem | How |
|---|---|
| **SX1262 LoRa** | `CrowPanelP4Hal` — IDF-native `spi_master` on `SPI3_HOST` with DMA disabled. Subclasses `LockingArduinoHal` so the existing `RadioInterface` plumbing accepts it as a drop-in. Pins set in `platformio.ini` build_flags. |
| **WiFi** | `WiFi.setPins()` for the ESP-Hosted SDIO link to the on-board ESP32-C6 (pins from `board_config.h`). Patched into `src/mesh/wifi/WiFiAPClient.cpp::initWifi()`. |
| **Display** | 800×480 RGB16 via `esp_lcd_panel_rgb`. **Landscape and portrait** both work. LVGL 9 partial render at the *trimmed* size: 792×479 in landscape, 479×792 in portrait. We render at the smaller size to absorb the P4 RGB peripheral's fixed -8 col / -1 row scan-start offset (verified empirically in pioarduino IDF 5.5 — not fixable via timing/cfg knobs). The 800×480 framebuffer stays fully allocated and zeroed; the 8-col / 1-row trimmed margin is wrapped out of view by the hardware offset. Without this you'd see a smear at the screen edges. |
| **Touch** | GT911 over the shared IDF I2C bus (SCL=46, SDA=45) on `I2C_NUM_0`. STC8 coprocessor handles GT911 reset. |
| **Backlight + tap-to-wake** | STC8 coprocessor PWM. `crowpanel_backlight.cpp` runs an idle-timer task on core 1 that dims via STC8 after `screen_on_secs`; our `touch_read_cb` calls `backlight_notify_activity()` and consumes the wake-tap. |
| **mcui (full Meshtastic UI)** | `src/graphics/mcui/` — chat, nodes, maps, settings tabs + on-screen keyboard, all LVGL 9 widgets. `McDisplayP4.cpp` (in this variant dir) replaces the LovyanGFX-based `McDisplay.cpp` via `build_src_filter`. UI task pinned to core 1 (avoids cross-core PSRAM heap race). Orientation switch via Settings → Display → "Switch to portrait/landscape mode" reboots and applies on next boot. |
| **Portrait via PPA SRM** | The RGB peripheral can't rotate its scan, so portrait uses the P4's PPA (Pixel Processing Accelerator) Scaling/Rotation/Mirroring engine. `display_init()` registers a PPA SRM client when `mcui::landscape_active()` returns false; `flush_cb` then calls `ppa_do_scale_rotate_mirror()` per dirty rect with `rotation_angle = PPA_SRM_ROTATION_ANGLE_90` (CCW) into the single 800×480 panel framebuffer. Touch coords use the inverse mapping. Runtime cost: one blocking PPA call per LVGL flush — invisible at typical UI frame rates. Code in `CrowPanelP4Display.cpp::flush_cb` and `touch_read_cb`. |
| **Settings persistence** | `Save & reboot` from the onboarding modal writes `/prefs/config.proto` to LittleFS and survives reboot. Took some heap-allocator surgery to get there — see "P4 PSRAM heap workaround" below. |
| **DS3231 RTC** | I2C 0x68 (TCXO-stabilised, battery-backed). Driver in `ds3231.{h,cpp}`. At boot `mcclock_init()` reads the chip and feeds the time into Meshtastic's `perhapsSetRTC(RTCQualityDevice, …)`. NTP-via-UDP overwrites it with `RTCQualityNTP` once WiFi associates and `mcclock_save()` writes the new epoch back. Survives full power-down. |
| **Timezone (algorithmic, auto-DST)** | Settings → Display → Timezone dropdown. ~43 entries covering UTC-12 through UTC+14 incl. half- and quarter-hour zones. Saved as a uint8 *index* in NVS (key `mcuiTzIdx`, namespace `meshtastic`) — not as a POSIX string in `config.device.tzdef`, because the pioarduino IDF newlib silently drops M-rules from POSIX TZ strings (verified: `EET-2EEST,M3.5.0/3,M10.5.0/4` parses as just `EET-2`). mcui owns the TZ logic instead: a static `(label, std_offset_s, dst_rule)` table, hand-rolled `last_sunday(year, month)` math for EU/US/AU DST, then a fixed-offset POSIX string (`<+02>-2`) pushed to libc each minute via McAuxThread so the seasonal flip happens automatically. Code in `src/graphics/mcui/screens/McSettings.cpp` (`TZ_LIST[]`, `tz_is_dst_active`, `tz_apply_to_libc`, `mcui_tz_apply_at_boot`). Pattern lifted from the meshcore reference firmware on the same hardware. |
| **Manual position entry** | No on-board GPS module, and the phone-app TCP API is intentionally disabled on this build (it would crash, see WiFi section). Instead, mcui Settings → **Manual Position** has tappable Latitude / Longitude / Altitude rows + Save & Clear buttons. The user copies coordinates from any GPS app (Google Maps long-press → drop pin → tap to copy lat,lon) and types them in. Save validates the values, calls `nodeDB->setLocalPosition()` with `location_source = LOC_MANUAL`, sets `config.position.fixed_position = true`, persists to disk, and triggers `positionModule->sendOurPosition()` so neighbours get the new fix immediately rather than waiting for `position.broadcast_secs`. The position survives reboots (LittleFS) and continues re-broadcasting on the configured interval. Implementation: `src/graphics/mcui/screens/McSettings.cpp` (`queue_position_apply` / aux thread block / Manual Position card around line 2300). |

## What is intentionally disabled

- **Bluetooth** (`MESHTASTIC_EXCLUDE_BLUETOOTH=1`) — P4 has no BT radio. The C6 has one but `NimBLE-Arduino` cannot drive a hosted controller. **The mobile/desktop Meshtastic apps will not be able to pair with this board over BLE.**
- **libpax** (`MESHTASTIC_EXCLUDE_PAXCOUNTER=1`) — assumes native WiFi/BLE radios.
- **MQTT, web admin, paxcounter, input broker, canned messages, generic I2C scan** — not all of these matter on P4, but they each save build size and dodge specific compile/runtime issues. See `MESHTASTIC_EXCLUDE_*` flags in `platformio.ini`.
- **`meshtastic-device-ui` library** — replaced by mcui. LovyanGFX has no P4 RGB driver upstream.

## File layout

```
boards/esp32-p4-crowpanel-dhe04005d.json     ← board JSON (ESP32-P4, 16MB) — unused, see iteration #1
variants/esp32p4/crowpanel_dhe04005d/
├── platformio.ini             ← env [dhe04005d-5inch-P4-meshtastic], pioarduino @55.03.36-1, LVGL 9
├── partitions.csv             ← 16MB single-app + ~10MB LittleFS (label "mtdata")
├── pins_arduino.h             ← Arduino-esp32 P4 pin overrides (SCK/MOSI/MISO/SS only)
├── variant.h                  ← Meshtastic-side pin macros (LoRa, I2C, touch)
├── lv_conf.h                  ← LVGL 9.2.2 config (CLIB allocator, FreeRTOS mutex)
├── board_config.h             ← *verbatim from meshcore* — pin source-of-truth
├── EspHal.h                   ← *verbatim from meshcore* — IDF-native RadioLib HAL
├── i2c_bus.{h,cpp}            ← *verbatim from meshcore* (I2C_NUM_0, shared bus)
├── gt911.{h,cpp}              ← *verbatim from meshcore*
├── stc8.{h,cpp}               ← *verbatim from meshcore*
├── CrowPanelP4Hal.h           ← LockingArduinoHal subclass that delegates to EspHal
├── CrowPanelP4Display.{h,cpp} ← LVGL 9 init on esp_lcd_panel_rgb + GT911 indev + tap-to-wake
├── McDisplayP4.cpp            ← drop-in replacement for src/graphics/mcui/McDisplay.cpp
├── ds3231.{h,cpp}             ← DS3231SN battery-backed RTC at I2C 0x68
├── PsramAllocGuard.cpp        ← linker --wrap shim for app-side heap_caps_* allocs
├── EspHostedCliNoop.cpp       ← linker --wrap stub for esp_console_cmd_register
└── README.md                  ← this file
```

## Patched core files

All edits in `src/` are gated by `defined(ARCH_ESP32P4) && defined(CROWPANEL_DHE04005D)` (or `CONFIG_IDF_TARGET_ESP32P4` for things that affect any P4 build) so they only affect this env:

- `src/mesh/RadioInterface.cpp` — uses `CrowPanelP4Hal` instead of `LockingArduinoHal(SPI, ...)`.
- `src/mesh/wifi/WiFiAPClient.cpp` — calls `WiFi.setPins(...)` for the ESP-Hosted SDIO link before `WiFi.mode(WIFI_STA)`.
- `src/main.cpp` — overrides `heap_caps_malloc_extmem_enable(2048)` so default malloc < 2 KB stays internal (see "P4 PSRAM heap workaround").
- `src/platform/esp32/main-esp32.cpp` — adds P4 to the IDF 5 watchdog API branch and the `esp_task_wdt_reconfigure` fallback; bumps `APP_WATCHDOG_SECS` to 300 on this build (mcui sharing core 1 with loopTask occasionally stalls the WDT feed during long LVGL frames).
- `src/sleep.cpp` — gates the IDF-4-only `RTCWDT_BROWN_OUT_RESET` / `TG0WDT_SYS_RESET` / `TG1WDT_SYS_RESET` and `CONFIG_ESP32_DEFAULT_CPU_FREQ_MHZ` references off P4.
- `src/mesh/RadioLibInterface.cpp/.h` — extends the missed-IRQ poller (`pollMissedIrqs`) to also catch missed `TX_DONE` when `sendingPacket` is set. Without this, completed transmissions on the SX1262 don't get noticed, every subsequent send is rejected as `busyTx`, and the firmware reboots after 60 s with critical error 8 (#34).
- `src/mesh/SX126xInterface.cpp` — replaces RadioLib's unbounded `scanChannel()` CAD wait with a 200 ms IRQ-flag poll for this build. Without this, `RadioIf` deadlocks after a phantom preamble: the chip never raises CAD-done DIO1 edge, the first send goes out fine, the second never can (#33).
- `src/graphics/mcui/McUI.cpp` — `prefs_load_once()` forces `s_landscape = true` on this variant; UI task pinned to core 1 to avoid cross-core PSRAM heap races.
- `src/graphics/mcui/screens/McSettings.cpp` — `modal_card_height()` reserves keyboard space; onboarding card is scrollable; Save/Later buttons positioned at absolute Y so they're inside scrollable content (not viewport-bottom which would overlap form fields in landscape).
- `src/graphics/mcui/data/McMessages.cpp` + `McMessages.h` — message store routes through `MALLOC_CAP_INTERNAL` (with no-`8BIT` fallback); `MC_MAX_CONVERSATIONS` cut from 24 to 8 to fit the 8-bit-internal pool.
- `src/graphics/mcui/data/McSender.cpp` — wakes the Arduino loop after queueing a UI-originated send (otherwise the queue would idle when mcui shares core 1 with loopTask, #31). Also: refuses to send a legacy DM to a peer whose public key is unknown; instead broadcasts NodeInfo with `wantReplies=true` and marks the local message as failed (#35).
- `src/graphics/mcui/data/McObserver.cpp` — when public text arrives from a node whose pubkey we don't have, broadcast NodeInfo with `wantReplies=true` to solicit theirs (#35 complement).
- `src/crowpanel_backlight.cpp` — `_bl_cmd()` gated to drive STC8 PWM on this board instead of the S3 board's I2C backlight controller.
- `variants/esp32p4/crowpanel_dhe04005d/EspHal.h` — stateful `attachInterrupt`/`detachInterrupt` so the GPIO ISR service is installed exactly once and per-pin handlers re-arm cleanly across RadioLib's setup-time `detach → attach` sequence (#32).

## Building

```sh
pio run -e dhe04005d-5inch-P4-meshtastic
pio run -e dhe04005d-5inch-P4-meshtastic -t upload
pio device monitor -e dhe04005d-5inch-P4-meshtastic
```

The `pioarduino @55.03.36-1` platform is downloaded by PlatformIO on first build (~hundreds of MB; matches meshcore).

## Iterations applied to reach a clean build (notes for the next porter)

These were the compile/link/runtime issues we hit and the gates we added. Listed in the order they surfaced, since each one only becomes visible after the previous is fixed:

1. **`sdkconfig.h: No such file or directory`** in newlib's `reent.h`. Fix: use pioarduino's bundled `board = esp32-p4-evboard` instead of a custom board JSON — pioarduino's build script only injects the sdkconfig include path for board names it knows.
2. **`-include mbedtls/error.h` breaks** Wire/SPI compilation (transitively pulls sdkconfig.h on a path that doesn't have it). Drop the force-include.
3. **`lv_conf.h` not found** in lvgl. Copy [lv_conf.h](lv_conf.h) from meshcore into the variant dir (the existing `-Ivariants/...` flag makes it discoverable).
4. **`pins_arduino.h` redefining `NUM_DIGITAL_PINS` / `EXTERNAL_NUM_INTERRUPTS` / `NUM_ANALOG_INPUTS`** — Arduino-esp32 P4 core defines them. Don't.
5. **`SCK`/`MISO`/`MOSI`/`SS` undeclared** in arduino-esp32's `SPI.cpp`. Provide the four C++ symbols in `pins_arduino.h` (sourced from `board_config.h`'s pin macros).
6. **`AES.h` not found** in `src/mesh/CryptoEngine.h`. Add `rweather/Crypto@0.4.0` to `lib_deps` (we extend `arduino_base` not `esp32_common`, so we don't get it for free).
7. **`ARCH_ESP32` redefined warning** — `src/platform/esp32/architecture.h` already defines it. Don't re-define in build_flags.
8. **`Power.cpp` legacy ADC API** (`adc1_get_raw`, `esp_adc_cal_*`) doesn't exist on P4. Don't define `BATTERY_PIN` at all (the gate is `#if defined(BATTERY_PIN)`, and `-1` still counts as "defined").
9. **`SERIAL_BUFFER_SIZE` undeclared** in `gps/GPS.cpp`. Add to build_flags (along with a few other things `esp32_common` provides that `arduino_base` doesn't).
10. **`USERPREFS_TZ_STRING` / `USERPREFS_RINGTONE_RTTTL` undeclared** — `platformio-custom.py` is supposed to inject these via `projenv.Append` but it doesn't reach our env. Provide explicit fallbacks in build_flags.
11. **`HTTPBodyParser.hpp` not found** — esp32_https_server isn't in our lib_deps. Add `MESHTASTIC_EXCLUDE_WEBSERVER=1`.
12. **`mqtt` / `MQTT::*` link errors** — we excluded `src/mqtt/` via `build_src_filter`, but core code still references the symbols. Add `MESHTASTIC_EXCLUDE_MQTT=1`.
13. **`esp_task_wdt_init(int, bool)` doesn't exist on IDF 5.x** — takes a config struct. The C6 branch in `main-esp32.cpp` already had the new API; just add `|| defined(CONFIG_IDF_TARGET_ESP32P4)` to that branch.
14. **`RTCWDT_BROWN_OUT_RESET` / `TG0WDT_SYS_RESET` / `TG1WDT_SYS_RESET` and `CONFIG_ESP32_DEFAULT_CPU_FREQ_MHZ`** removed/renamed in IDF 5.5. Gate around the offending lines in `src/sleep.cpp` with `#if !defined(CONFIG_IDF_TARGET_ESP32P4)`.
15. **`extern "C"` inside a function body** — invalid C++. The `crowpanel_p4_boot()` / `crowpanel_p4_loop()` declarations in `src/main.cpp` must be at file scope.
16. **TWDT already initialised** at runtime — Arduino-esp32 app_main now pre-inits TWDT on IDF 5.x. Catch `ESP_ERR_INVALID_STATE` from `esp_task_wdt_init` and call `esp_task_wdt_reconfigure` instead.
17. **LittleFS partition label** — Meshtastic mounts `mtdata`. Our partition name was `spiffs`. Rename to `mtdata`.
18. **I2C bus port collision** — the first Meshtastic port reused an S3-era `Wire.begin(15,16)` backlight path, which claimed `I2C_NUM_0` before the DHE04005D STC8/GT911 bus could initialise. Fix: keep the shared bus on MeshCore's `I2C_NUM_0`, disable generic Meshtastic I2C probing, and route the backlight helper through the DHE04005D IDF I2C/STC8 driver.
19. **Phantom "M5 cardKB" detection** at 0x5f on the misconfigured Wire bus, polled forever. Add `MESHTASTIC_EXCLUDE_I2C=1` + `MESHTASTIC_EXCLUDE_INPUTBROKER=1` + `MESHTASTIC_EXCLUDE_CANNEDMESSAGES=1`.
20. **mcui needs LVGL 9, our scope-B build was on LVGL 8.** Bumped `lib_deps` to `lvgl/lvgl@^9.2.2`, replaced `lv_conf.h` with v9.2.2 from device-ui, rewrote `CrowPanelP4Display.cpp` for the v9 API (`lv_display_create`, `lv_display_set_buffers`, `lv_indev_create`, new flush_cb signature). After clean build, LVGL re-compiles with the new config (PIO doesn't always notice `lv_conf.h` changes — `--target clean` or delete `.pio/build/.../lib*/lvgl` to be safe).
21. **`ui_font_montserrat_14` undefined at link time** — device-ui's `lv_conf.h` defaults `LV_FONT_DEFAULT` to a custom font asset that we don't ship. Switch to `&lv_font_montserrat_14` (built-in) and enable `LV_FONT_MONTSERRAT_14 1`.
22. **mcui's `McDisplay.cpp` uses LovyanGFX** — exclude it via `build_src_filter -<graphics/mcui/McDisplay.cpp>` and provide our own `McDisplayP4.cpp` that re-defines `mcui::display_init()` on top of `crowpanel_p4::display_init()`. The rest of mcui (status bar, screens, keyboard, data layer) is pure LVGL and compiles unchanged.
23. **`pcf8563.h` not found** — mcui's `McClock.cpp` probes a PCF8563 RTC. Add `lewisxhe/PCF8563_Library@1.0.1` to `lib_deps`. Probe gracefully fails on this board (no on-board PCF8563).
24. **Default LVGL TLSF heap of 96 KB chokes mcui** — set `RAM_SIZE=6144` in build_flags so `lv_conf.h` computes `LV_MEM_SIZE = 6 MB`. Pool is allocated in PSRAM via `LV_MEM_POOL_ALLOC = heap_caps_aligned_alloc(MALLOC_CAP_SPIRAM)` (lv_conf.h auto-detects `BOARD_HAS_PSRAM`).
25. **mcui hits IDF heap assert immediately on PSRAM allocs after FBs land.** See "P4 PSRAM heap workaround" below for the long story; short answer is `heap_caps_malloc_extmem_enable(2048)` early in `setup()`.
26. **`extern "C"` inside a function body** in `src/main.cpp` — invalid C++. The `crowpanel_p4_*` declarations must be at file scope. (Hooks have since been removed in favour of the mcui path.)
27. **Onboarding card overflows landscape (480 px tall) screen** — extends to 470 px of content, plus a 180 px keyboard. Make the card scrollable (`lv_obj_set_scroll_dir(card, LV_DIR_VER)`) and switch Save/Later from `LV_ALIGN_BOTTOM_*` to absolute Y at `onboarding_btn_y = 380` so they sit at content-bottom (reachable by scrolling), not viewport-bottom (where they'd overlap form fields).
28. **Backlight dim → screen never wakes** — Meshtastic's `screen_on_secs` (default 60s) calls `_bl_cmd(BL_OFF)` via the backlight task on core 1. The S3 mcui's `touch_cb` calls `backlight_notify_activity()` and consumes the wake-tap; mirror that in our `touch_read_cb`.
29. **mcui channel-encrypt fails: `esp-aes: Failed to allocate memory for the array of DMA descriptors`** — the IDF AES hardware accelerator's GDMA descriptors need DMA-capable internal RAM. Our LVGL partial-render buffers (2 × 64 KB) had been claiming most of that pool. Move the LVGL buffers to PSRAM, allocated **before** `esp_lcd_new_rgb_panel` so we beat the post-FB heap corruption. (`CrowPanelP4Display.cpp::display_init` does the pre-allocation; the buffers are then handed to `lv_display_set_buffers`.)
30. **mcui message store failed alloc**: 187 KB `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT` request fails despite `largest_free_block` reporting more — some "internal" RAM is 32-bit-only IRAM that 8-bit-byte requests can't use. Reduce `MC_MAX_CONVERSATIONS` to 8 on this build (`#ifdef CROWPANEL_DHE04005D` in `McMessages.h`) so the store fits as ~63 KB. Channels 0..7 are covered; direct conversations recycle the oldest slot via `find_or_create_locked`.
30b. **WDT reboot at uptime ~150s under mcui+loopTask sharing core 1** — bump `APP_WATCHDOG_SECS` to 300 on this variant. Long LVGL frames during chat/screen rebuilds can stall loopTask's WDT feed under round-robin scheduling.
31. **UI send queues a packet, then RadioIf doesn't pick it up** — `messages_init`/`mcui::sendText` were enqueueing the radio packet but never poking the Arduino loop. On core 1 with mcui sharing the CPU, the loop wouldn't run promptly enough to drain the queue. Patch `McSender.cpp` to wake the main loop immediately after queueing (Meshtastic's notify mechanism).
32. **`gpio_isr_handler_remove(572): GPIO isr service is not installed` / `gpio_install_isr_service(530): already installed` noise + DIO1 IRQ delivery flaky** — RadioLib calls `detachInterrupt` first on init (before our `EspHal` has called `gpio_install_isr_service`), then `attachInterrupt`. Make `EspHal::attachInterrupt`/`detachInterrupt` stateful — one-shot ISR-service install, track per-pin handler so re-arm is clean. After this DIO1 RX/TX_DONE IRQs deliver reliably most of the time.
33. **`RadioIf` deadlocks on phantom-preamble CAD wait — first send succeeds, second send times out** — RadioLib's `scanChannel()` (used for Listen-Before-Talk) uses an unbounded wait for the SX1262 CAD-done DIO1 edge. On P4 with our HAL, after a "false preamble" reset the chip sometimes never raises that edge, freezing `RadioIf`. Replace the `scanChannel()` call (in `SX126xInterface.cpp`, gated on `CROWPANEL_DHE04005D`) with a 200 ms bounded poll of the SX1262 IRQ flag register; on timeout, log `SX126X CAD timed out, treating channel as free` and proceed with TX. After this, the radio will TX again after a phantom preamble instead of wedging.
34. **`Hardware Failure! busyTx for more than 60s` reboot after the first send** — TX completes on the air, but the `TX_DONE` IRQ doesn't always fire on this HAL, so `sendingPacket` stays non-null and every subsequent send is rejected with `busyTx`. After 60 s of stuck-TX state, Meshtastic records critical error 8 and reboots. Fix: extend the existing `pollMissedIrqs` safety net in `RadioLibInterface.cpp` to also poll the SX1262 `TX_DONE` flag when `sendingPacket` is set, and route a missed completion through the normal `ISR_TX` path. After this, log shows `caught missed TX_DONE` then `Completed sending` and the next send proceeds normally.
35. **Direct messages refused: `Unknown public key for destination node ... refusing to send legacy DM`** — Meshtastic v2.7 requires PKI for DMs. mcui was treating the immediate `error=39` ("no pubkey") as if the message had been sent. Fix in `McSender.cpp`: when sending to an unknown-pubkey peer, mark the local message as failed and broadcast a NodeInfo with `wantReplies=true` to solicit the peer's pubkey. Complement in `McObserver.cpp`: when public text arrives from a node whose pubkey we don't have on file, also broadcast NodeInfo with `wantReplies=true`. After a single round-trip of NodeInfo exchange the pubkey arrives and PMs work.
36. **DS3231 RTC at I2C 0x68 not wired** — Meshtastic's stock `src/gps/RTC.cpp` only probes RV3028/PCF8563/PCF85063, none of which this board has. mcui's `McClock.cpp` already does an out-of-band PCF8563 probe; extend it to also try DS3231 first via our `ds3231.{h,cpp}` driver (which lives on our private IDF I2C bus, separate from Arduino `Wire`). `mcclock_save` similarly routes to `ds3231_write_unix` under the gate. Net: wall-clock time restores across reboots; on first boot of a fresh chip the OSF flag is set and we wait for a peer-packet timestamp / NTP to seed the chip.

## P4 PSRAM heap workaround

The single most painful symptom of porting Meshtastic to ESP32-P4 is this: **once `esp_lcd_new_rgb_panel()` allocates the framebuffers in PSRAM, the IDF heap reports `Largest Free Block: 24 B` despite ~31 MB free** — and any subsequent allocation that visits the PSRAM free-list metadata trips the assert

```
assert failed: block_locate_free tlsf_control_functions.h:618 (block_size(block) >= *size)
```

The corruption is in the heap *structure* (TLSF bin/free-list bookkeeping is inconsistent), not in any specific allocation. We didn't track down the underlying P4 framework / IDF interaction bug — it would need JTAG to inspect properly. Instead we route around it.

**Things that didn't work:**

- `heap_caps_malloc_extmem_enable(SIZE_MAX)` — sets only a *preference*; when internal RAM gets tight the IDF allocator still falls back to PSRAM and dies.
- Pre-claiming all but ~2 MB of PSRAM in one big allocation at boot — leaves PSRAM "full" so default malloc can't go there, but the heap walker still visits the (corrupted) free-list bins during fallback consideration and asserts.
- Switching LVGL from BUILTIN TLSF to CLIB allocator — mcui wasn't the one corrupting things; the corruption was already there from the FB allocs.
- Pinning the mcui task to the same core as the Arduino loop (core 1) — helps with the *initial* widget allocation crash (probably a SMP race), but doesn't fix subsequent saves.

**What works (`src/main.cpp:394`):**

```c
#if defined(ARCH_ESP32P4) && defined(CROWPANEL_DHE04005D)
    heap_caps_malloc_extmem_enable(2048);   // < 2 KB → internal only
#endif
```

This sets the IDF heap's allocation-preference threshold to 2 KB. Allocations smaller than 2 KB (the 754-byte nanopb buffers, FILE struct buffers, LVGL widget allocations, almost everything that crashed) go straight to internal RAM and never visit the corrupted PSRAM heap. Allocations ≥ 2 KB still go to PSRAM (and they don't trip the assert because they hit different code paths inside multi_heap_malloc).

Three complementary measures keep mcui's footprint inside internal RAM:

- `src/graphics/mcui/data/McMessages.cpp` allocates the message store explicitly with `MALLOC_CAP_INTERNAL` on this board — never touches the PSRAM heap. Falls back to plain `MALLOC_CAP_INTERNAL` (no `MALLOC_CAP_8BIT`) if the 8-bit-internal pool is too small.
- `MC_MAX_CONVERSATIONS` is reduced from 24 to 8 (`#ifdef CROWPANEL_DHE04005D` in `McMessages.h`) — full 24 slots × ~7.8 KB = ~187 KB which won't fit a single contiguous internal block once LVGL widgets are allocated. 8 slots × ~7.8 KB ≈ 63 KB fits comfortably.
- LVGL partial draw buffers (`CrowPanelP4Display.cpp`) are allocated from PSRAM **before** `esp_lcd_new_rgb_panel` runs (the post-FB heap corruption only kicks in after the panel claims its FBs). This frees ~128 KB of DMA-capable internal RAM that the IDF AES hardware accelerator's GDMA descriptors need — without this, channel encryption fails with `esp-aes: Failed to allocate memory for the array of DMA descriptors` and packets ship with corrupted ciphertext.
- The mcui UI task is pinned to core 1 (same as the Arduino loop) in `src/graphics/mcui/McUI.cpp::setup` so widget allocations don't race with main-thread mallocs across cores.

## Cosmetic issues that remain (non-blocking)

- Boot-time I2C error spam on SDA=15/SCL=16 should be gone: the DHE04005D backlight helper no longer initialises Arduino `Wire` and now claims the real STC8/GT911 bus with the IDF driver first.
- `LittleFS partition "spiffs" could not be found` line at boot — comes from a *different* mount attempt; the actual `mtdata` mount succeeds (proven by Meshtastic loading `/prefs/nodes.proto` etc.).
- `CPU clock could not be set to 80 MHz. Supported frequencies: 360 MHz` — Meshtastic's idle-power code tries to slow down; P4 only runs at 360 MHz. Cosmetic.
- LVGL: `glyph dsc. not found for U+2014` (em-dash) — montserrat font is missing the em-dash glyph; replaced with `-` if it bothers you.

## Known compile-time risks (notes preserved from initial port — most no longer apply)

This variant pulls in pieces of the Meshtastic core that were never written for ESP32-P4. Expect some of the following and gate them out as you go:

1. **Sleep code** (`src/sleep.cpp`) — uses `esp_sleep_enable_ext0_wakeup()` which doesn't exist on P4. The meshcore variant uses `ext1` instead. Add `#if !defined(ARCH_ESP32P4)` around any `ext0` references.
2. **Audio / Codec2** — `ESP32_Codec2` lib_dep on the S3 path. We don't pull `esp32s3.ini` here so it shouldn't appear, but verify nothing reaches for `MESHTASTIC_EXCLUDE_AUDIO`.
3. **`XPowersLib`** — referenced by ESP32 base. Doesn't initialize on P4 because there's no PMIC. Should be benign (`pmu_found` stays false) but watch for hard asserts.
4. **`esp32_https_server`** — IDF version assumptions; may not build cleanly against IDF 5.5. If linker fails here, drop it from `lib_deps` (we don't need HTTPS for the status screen).
5. **`SPI` global** — arduino-esp32 P4 core does declare it, but binding it to nothing is wasteful. Harmless if unused, which it is once `CrowPanelP4Hal` takes over.
6. **NodeDB API drift** — `CrowPanelP4Main.cpp` reads `getMeshNode()` / `user.id` / `user.short_name`. If those names have changed, the file won't compile; fix the field accessors there.

## Hardware verification — verified status

- [x] **Boot reaches `setup()` end** — Meshtastic banner appears, NodeDB loads.
- [x] **Backlight on** — STC8 PWM ramps at boot.
- [x] **mcui UI renders** — status bar, tab bar, chat / nodes / maps / settings tabs, on-screen keyboard. Em-dash glyph (U+2014) is missing from `lv_font_montserrat_14`; cosmetic.
- [x] **Touch responds** — `[gt911] GT911 found at 0x5D` in log; tap navigates UI.
- [x] **Tap-to-wake** — backlight dims at `screen_on_secs` and any tap wakes it.
- [x] **Onboarding modal scrolls** — long name / short name / region / Save & reboot all reachable in landscape.
- [x] **Save & reboot persists** — region + names + module config survive reboot. (Verified after the heap-allocator workaround.)
- [x] **SX1262 init** — `SX126x init result 0`, `SX1262 init success, TCXO, Vref 1.800000V` in log. Module **does** have a TCXO; `SX126X_DIO3_TCXO_VOLTAGE=1.8 + TCXO_OPTIONAL` is the right config (probe 1.8V, fall back to XTAL on failure).
- [x] **LoRa packet TX** — first send goes on-air after the bounded-CAD patch (#33); second-and-onward sends recover via the missed-TX_DONE poll (#34). Verified RX confirmed on remote node.
- [x] **LoRa packet RX** — `Lora RX (...) Received text msg from=...` in log; messages render in the Chats tab.
- [x] **Public LongFast chat works both ways** — TX from CrowPanel reaches remote node, TX from remote node renders in CrowPanel chat.
- [x] **Direct messages work both ways** — after the PKI / NodeInfo handshake patches (#35).
- [x] **DS3231 RTC at 0x68 wired** — `mcclock_init` probes our `ds3231_*` driver on the IDF I2C bus first, falls back to the legacy PCF8563 path. Wall-clock time persists across reboots; `mcclock_save` writes back to DS3231 when Meshtastic syncs from a peer packet timestamp or NTP.
- [ ] ~~**WiFi associates**~~ — **incompatible with this build, see "WiFi via ESP-Hosted is blocked" below.** SDIO link to the C6 *does* come up correctly with the right pin map (`clk=53, cmd=54, d0=52..d3=49, rst=20`), but ESP-Hosted's first internal `heap_caps_malloc(MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM)` then asserts on the corrupted PSRAM heap. Same root cause as the FB-induced PSRAM bug, but unfixable from outside the IDF.
- [ ] ~~**TCP API reachable**~~ — blocked by WiFi being unavailable. Use the python CLI over USB instead: `meshtastic --port COM<n> --info`.

## Cosmetic issues that remain (non-blocking)

- Boot-time I2C error spam on SDA=15/SCL=16 from arduino-esp32 auto-init Wire on stale defaults — harmless, no fix without forking the framework.
- `LittleFS partition "spiffs" could not be found` line at boot — comes from a *different* mount attempt; the actual `mtdata` mount succeeds (Meshtastic loads `/prefs/nodes.proto` etc.).
- `CPU clock could not be set to 80 MHz. Supported frequencies: 360 MHz` — Meshtastic's idle-power code tries to slow down; P4 only runs at 360 MHz.
- LVGL: `glyph dsc. not found for U+2014` (em-dash) at boot — montserrat 14 doesn't include the em-dash glyph.
- ~~Right-edge / bottom-row "smear" of LVGL pixels~~ — fixed by trimming the LVGL render size 8 px wide / 1 px tall (see "Display" row in *What works*).

## WiFi via ESP-Hosted (works with one caveat)

Same root cause as the P4 PSRAM heap workaround: once `esp_lcd_new_rgb_panel()` allocates its framebuffers in PSRAM, the IDF heap is left in a corrupted state where any walk of its free-list metadata trips a TLSF assert (`block_locate_free` in `tlsf_control_functions.h:618`). The IDF `esp_hosted` component allocates its packet ring buffers with **explicit `heap_caps_malloc(MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM)` calls** that bypass the `extmem_enable` allocation preference entirely. If that allocation runs *after* the framebuffers land, it crashes on the corrupted heap.

**The fix:** pre-initialise both ESP-Hosted **and** the WiFi state machine in `initVariant()`, *before* `esp_lcd_new_rgb_panel()` runs. See `src/crowpanel_backlight.cpp::initVariant()`:

```cpp
WiFi.setPins(CLK, CMD, D0..D3, RST);   // CrowPanel routes the SDIO link to
                                       // pins 53/54/52/51/50/49/20 — not the
                                       // arduino-esp32 default 18/19/14/...
hostedInitWiFi();                      // SDIO transport up to the C6 — large
                                       // ring-buffer allocs land in pristine PSRAM
WiFi.mode(WIFI_STA);                   // esp_wifi_init/start — second wave of big
                                       // PSRAM allocs (state-machine buffers)
WiFi.disconnect();                     // radio idle, but buffers stay claimed
```

This adds ~3 seconds to boot time (SDIO probe + version exchange with the C6) but lets WiFi coexist with the RGB panel. Subsequent `WiFi.begin()` calls from the UI reuse the already-allocated buffers — no fresh PSRAM allocs, no crash.

After this fix, the After-Setup heap dump shows ~1.7 MB of PSRAM allocated (the WiFi state-machine ring buffers), and `WiFi.scanNetworks()` / `WiFi.begin()` from the Settings tab work correctly. NTP-via-UDP syncs the wall clock and saves it to the DS3231.

### What's still gated off: HTTPClient (Auto-TZ) and the phone-app TCP API

**Auto-TZ:** Meshtastic's "Auto-TZ" feature does an HTTP GET against `worldtimeapi.org`, and `HTTPClient` allocates its own buffers from PSRAM at request time — those allocations *do* hit the corrupted heap and crash. We disable Auto-TZ on this build with `-DMESHTASTIC_EXCLUDE_TZ=1`. That same flag also gates out the stock `setenv("TZ", config.device.tzdef, 1) + tzset()` block in `main.cpp`, so we replaced the whole TZ pipeline with the algorithmic engine in `McSettings.cpp` — see the "Timezone (algorithmic, auto-DST)" row in the *What works* table at the top of this file. The DS3231 preserves wall-clock UTC across reboots so accurate localtime is just a one-time dropdown pick.

**Phone-app TCP API (port 4403):** intentionally **disabled** on this build. Stock Meshtastic already gates `initApiServer()` on `displaymode != COLOR` in `WiFiAPClient.cpp` — we keep that gate. We tried opening the API server back up to get phone-pushed GPS positions through, but `WiFiClient::write()` from `StreamAPI::emitTxBuffer()` triggers the IDF lwip stack to allocate a 64 KB TCP send buffer (`CONFIG_LWIP_TCP_SND_BUF_DEFAULT=65534`) — that allocation goes through `heap_caps_malloc(MALLOC_CAP_SPIRAM, …)` *inside* the IDF's pre-built lwip blob and crashes on the corrupted PSRAM heap (`block_locate_free tlsf_control_functions.h:618`).

Mitigation attempts that did not work:

1. **Linker `--wrap=heap_caps_malloc`** (`PsramAllocGuard.cpp`) — only catches references resolved at our link step. Pre-built IDF libraries had their internal calls resolved at IDF build time and bypass the wrap entirely. Verified via `nm` on `firmware.elf`: `__wrap_heap_caps_malloc` is at `0x40058a7e` (app code) but IDF's lwip calls `heap_caps_malloc` directly at `0x4ff01636` (IDF flash). The wrap *does* still help for app-side allocs and we keep it as a safety net.

2. **Pre-consume all unused PSRAM at boot** so subsequent IDF allocs return NULL and fall back gracefully — destabilised `lv_init()` itself, presumably by perturbing the heap state in a way that broke a different early allocation path.

A real fix would require **rebuilding the IDF** with `CONFIG_LWIP_TCP_SND_BUF_DEFAULT` reduced to <4 KB (so it lands in internal RAM via `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096`) or with `CONFIG_LWIP_TCP_USE_PSRAM=n`. That's outside what's tractable from a PlatformIO project consuming pioarduino's pre-built blob.

**Replacement:** Manual Position entry from mcui Settings → Manual Position. The user types lat / lon / alt by hand (copying from any GPS app), Save validates and broadcasts on LoRa via `positionModule->sendOurPosition()`. No phone connection needed; works offline once coordinates are entered. Survives reboots. See the "Manual position entry" row in the *What works* table at the top of this file.

The mesh-side LoRa traffic is fully functional; the phone-app inability to connect is purely a configuration-/observation-side limitation. The Python CLI over USB (`pip install meshtastic && meshtastic --port COM<n> --set ...`) covers anything mcui doesn't expose.

## Out of scope for this build (follow-ups)

- **BLE phone-app pairing.** Requires routing NimBLE through the C6's hosted BT controller (no Arduino-level support exists yet).
- **Portrait orientation** with PPA SRM rotation in the LVGL flush (meshcore has it; omitted here, mcui forced landscape).
- **Deep sleep + wakeup.** P4 sleep API is different; current code paths assume S3.
- **microSD support.** Pins defined in `board_config.h`; no Meshtastic SD path wired.
- **Root-cause the IDF/P4 PSRAM heap corruption** so we can move the message store and other large allocs back to PSRAM. Needs JTAG access. Workaround documented above is stable.
