# CrowPanel DHE04005D — Meshtastic (ESP32-P4) variant

Meshtastic firmware for the **Elecrow CrowPanel Advanced 5-inch ESP32-P4 HMI**
(model **DHE04005D**): an 800×480 IPS RGB touch panel driven by an ESP32-P4,
with an on-board ESP32-C6 for Wi-Fi (over ESP-Hosted/SDIO) and an SX1262 LoRa
module.

Meshtastic's stock touch UI (`meshtastic-device-ui`) is built on LovyanGFX,
which has no ESP32-P4 RGB driver. This variant instead provides a P4-native
display backend (`CrowPanelP4Display.cpp` / `McDisplayP4.cpp`) built directly on
`esp_lcd_panel_rgb`, and lets the rest of the **mcui** layer (status bar, tabs,
chat, nodes, maps, settings, on-screen keyboard) render LVGL 9 widgets on top.

**Build env:** `dhe04005d-5inch-P4-meshtastic`
**App version tag:** `2.7.26-p4` (tracks upstream Meshtastic 2.7.26.54)

---

## What works

| Subsystem | Notes |
|---|---|
| **Display** | 800×480 RGB565 via `esp_lcd_panel_rgb`, LVGL 9.2.2. Double-framebuffer flip for tear-free output. **Both landscape and portrait** work. LVGL renders at a trimmed size (792×479 landscape, 479×792 portrait) to absorb the P4 RGB peripheral's fixed −8 col / −1 row scan-start offset. |
| **Orientation** | Selectable in Settings → Display; the change reboots and applies on next boot. Landscape uses a direct row blit; portrait uses the P4 **PPA** (Pixel Processing Accelerator) to rotate 90° per dirty rect into the panel framebuffer. |
| **Touch** | GT911 over an IDF I2C bus (`I2C_NUM_0`, SCL=46 / SDA=45). STC8 coprocessor handles GT911 reset. |
| **Backlight + tap-to-wake** | STC8 coprocessor PWM (`crowpanel_backlight.cpp`). An idle timer dims after `screen_on_secs`; `touch_read_cb` consumes the wake tap. |
| **Battery** | Read from the on-board STC8 MCU (I2C `0x2F`), which measures pack voltage and charge state. The status bar shows a battery icon + percentage; the percentage is derived from voltage via a Li-ion curve (`McStatusBar.cpp`, `stc8_read_battery`). |
| **SX1262 LoRa** | `CrowPanelP4Hal` — IDF-native `spi_master` on `SPI3_HOST`, subclassing `LockingArduinoHal` so `RadioInterface` accepts it as a drop-in. TCXO (DIO3, 1.8 V). TX/RX verified against a second node; public LongFast and PKI direct messages both work. |
| **Wi-Fi** | ESP-Hosted SDIO link to the on-board ESP32-C6. Brought up early (before the RGB panel init) so its DMA/PSRAM allocations land on a clean heap — see *PSRAM heap discipline* below. NTP-over-UDP syncs the clock. |
| **MQTT** | Connects to the public broker over Wi-Fi. Includes an "OK to MQTT (relay my packets)" toggle in Settings, low-latency downlink servicing, and a DMA-pressure guard that protects the SDIO link under load. |
| **Maps** | Offline raster tiles from the microSD card (`S:/tiles/{z}/{x}/{y}.png`), decoded with lodepng into a dedicated PSRAM arena. Pan/zoom, node markers with screen-space clustering (one dot + a count where nodes crowd), and node name tags at the closer zoom levels. |
| **Node DB** | Up to 300 nodes (`MAX_NUM_NODES=300`), persisted to LittleFS across reboots. |
| **Chat** | 8 conversations × 100 messages, stored on the PSRAM heap and persisted to LittleFS (`/prefs/mcui_msgs.bin`). Sender names resolve from the node DB, falling back to the `!hexid` node id until NodeInfo arrives. |
| **DS3231 RTC** | I2C `0x68`, TCXO-stabilised, battery-backed (`ds3231.{h,cpp}`). Restores wall-clock time at boot; NTP overwrites it once Wi-Fi is up. Survives full power-down. |
| **Timezone (auto-DST)** | Algorithmic TZ engine with a dropdown of ~43 zones; DST flips automatically. Saved as an index in NVS (the platform's newlib drops M-rules from POSIX TZ strings, so mcui owns the logic). Code in `McSettings.cpp`. |
| **Manual position entry** | Settings → Manual Position: type lat/lon/alt by hand (no on-board GPS). Save validates, sets a fixed position, persists, and broadcasts it on LoRa. |
| **Settings persistence** | Config, node DB, channels, chat history and UI prefs all persist to LittleFS. |

## Intentionally disabled

- **Bluetooth** (`MESHTASTIC_EXCLUDE_BLUETOOTH=1`) — the P4 has no BT radio, and `NimBLE-Arduino` can't drive the C6's hosted controller. The phone apps cannot pair over BLE.
- **libpax** (`MESHTASTIC_EXCLUDE_PAXCOUNTER=1`) — assumes native Wi-Fi/BLE radios.
- **Phone-app TCP API (port 4403)** — kept off (stock Meshtastic already gates it on `displaymode != COLOR`). Use the Python CLI over USB for anything mcui doesn't expose.
- **Web admin / HTTPS server** — pulls dependencies not built here.
- **`meshtastic-device-ui`** — replaced by the P4-native mcui backend.

## PSRAM heap discipline (the core of the port)

On this board, once `esp_lcd_new_rgb_panel()` allocates its framebuffers in
PSRAM, the IDF system PSRAM heap's TLSF free-list metadata is left inconsistent:
later allocations that walk it can assert (`block_locate_free`,
`tlsf_control_functions.h`). The port routes around this rather than patching the
pre-built IDF:

- **`heap_caps_malloc_extmem_enable(2048)`** early in `setup()` keeps allocations
  under 2 KB in internal RAM, away from the fragile PSRAM free-list.
- **Large buffers are reserved up-front, before the panel init**, while the PSRAM
  heap is still clean:
  - the **LVGL heap + image-decode arena** are one shared 16 MB PSRAM
    `multi_heap` (`crowpanel_lvgl_alloc.cpp` + `PngDecodeArena.cpp`). Sharing a
    single robust heap for LVGL widgets *and* decoded map tiles means a decoded
    buffer can never be freed into a different heap than it came from. LVGL uses
    `LV_STDLIB_CUSTOM` (routed onto this heap) and `LV_OS_NONE`.
  - the two panel framebuffers are allocated inside `esp_lcd_new_rgb_panel()`.
- **`PsramAllocGuard.cpp`** (`--wrap=heap_caps_*`) redirects post-init app-side
  SPIRAM allocations to internal RAM as a safety net.
- **Wi-Fi/ESP-Hosted is pre-initialised before the panel** so its DMA ring
  buffers land on the clean heap (`CrowpanelHostedSdio.cpp`).

The **DMA-capable internal pool** is the other scarce resource: ESP-Hosted's SDIO
RX buffers, the AES accelerator's descriptors, and the RGB bounce buffers all
draw from it. MQTT includes a guard that watches this pool and briefly drops the
broker connection if it collapses, plus a small pre-reserved "lifeboat" block.

## Display notes

- **RGB565 = 2 bytes/pixel.** All framebuffer byte math uses an explicit
  `kBytesPerPixel = 2` constant — *not* `sizeof(lv_color_t)`, which is a 3-byte
  RGB888 struct in LVGL 9 and would corrupt the landscape blit stride.
- Anti-tearing: LVGL composes into an off-screen framebuffer and flips at the
  frame boundary; dirty-rect history catches the newly-shown buffer up.
- Portrait rotation is a single blocking PPA call per flush — invisible at normal
  UI frame rates.

## File layout

```
variants/esp32p4/crowpanel_dhe04005d/
├── platformio.ini             ← env [dhe04005d-5inch-P4-meshtastic]
├── partitions.csv             ← 16 MB app + LittleFS data partitions
├── pins_arduino.h             ← Arduino-esp32 P4 pin overrides (SCK/MOSI/MISO/SS)
├── variant.h                  ← Meshtastic pin macros (LoRa, I2C, touch)
├── board_config.h             ← pin source-of-truth
├── lv_conf.h                  ← LVGL 9.2.2 config (CUSTOM allocator, OS_NONE)
├── EspHal.h                   ← IDF-native RadioLib HAL
├── CrowPanelP4Hal.h           ← LockingArduinoHal subclass over EspHal
├── CrowPanelP4Display.{h,cpp} ← RGB panel + LVGL init, flush/blit, touch, tap-to-wake
├── McDisplayP4.cpp            ← provides mcui::display_init() for this board
├── crowpanel_lvgl_alloc.cpp   ← LVGL custom allocator → shared PSRAM multi_heap
├── PngDecodeArena.cpp         ← PSRAM image-decode arena + lodepng allocators
├── PsramAllocGuard.cpp        ← --wrap shim for app-side heap_caps_* allocs
├── CrowpanelHostedSdio.cpp    ← pre-panel ESP-Hosted / Wi-Fi bring-up
├── EspHostedCliNoop.cpp       ← --wrap stub for esp_console_cmd_register
├── i2c_bus.{h,cpp}            ← shared IDF I2C bus (I2C_NUM_0)
├── gt911.{h,cpp}              ← GT911 touch controller
├── stc8.{h,cpp}              ← STC8 coprocessor (backlight PWM, GPIO, battery)
├── ds3231.{h,cpp}            ← DS3231 battery-backed RTC at I2C 0x68
└── README.md                  ← this file
```

Core Meshtastic files under `src/` are patched behind
`defined(ARCH_ESP32P4) && defined(CROWPANEL_DHE04005D)` (or
`CONFIG_IDF_TARGET_ESP32P4`) so the changes only affect this env — chiefly the
radio HAL wiring, the Wi-Fi pin map, the P4 heap/watchdog/sleep adaptations, and
the mcui data/observer layer.

## Building

```sh
pio run   -e dhe04005d-5inch-P4-meshtastic
pio run   -e dhe04005d-5inch-P4-meshtastic -t upload
pio device monitor -e dhe04005d-5inch-P4-meshtastic
```

`board = esp32-p4-evboard` in `platformio.ini` is the Espressif ESP32-P4
platform profile (the MCU target), not a different product board; the CrowPanel's
own pins/timings are set via `board_build.*` and build flags. The platform
toolchain is fetched by PlatformIO on the first build.

**Flashing note:** the app image (`ota_1`) can be flashed on its own to update
firmware; the LittleFS image holds `/prefs` (node DB, config, chat history) and
should only be re-flashed when you intend to wipe that data.

## Known cosmetic issues (non-blocking)

- `CPU clock could not be set to 80 MHz. Supported frequencies: 360 MHz` — the
  idle-power code tries to slow down; the P4 only runs at 360 MHz.
- A stray `LittleFS partition "spiffs" could not be found` line at boot comes from
  an unrelated mount probe; the real data partitions mount fine.

## Out of scope / follow-ups

- **BLE phone-app pairing** — needs NimBLE routed through the C6's hosted BT
  controller (no Arduino-level support yet).
- **Deep sleep + wakeup** — the P4 sleep API differs from the older ESP32 model
  some core code paths still assume.
- **Raster tiles must be pre-loaded onto the microSD** as `S:/tiles/{z}/{x}/{y}.png`;
  areas without tiles show placeholders.
