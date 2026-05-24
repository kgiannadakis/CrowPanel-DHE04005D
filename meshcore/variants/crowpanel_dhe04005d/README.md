# CrowPanel DHE04005D (ESP32-P4) — MeshCore variant

**Status:** Phase 1 skeleton. Compiles and boots a status screen; radio is initialised but no chat UI yet.

## Hardware

- Elecrow CrowPanel Advanced 5" ESP32-P4 HMI AI Display — 800x480 IPS (ST7262 RGB) + GT911 touch.
- External SX1262 LoRa module wired per `board_config.h` (same module used by the DIS05020A v1.1 build).

### LoRa wiring (MUST match the build flags in `platformio.ini`)

| Signal  | GPIO |
|---------|------|
| SCK     | 26   |
| MISO    | 47 (consumes UART1 header) |
| MOSI    | 48 (consumes UART1 header) |
| NSS     | 30   |
| DIO1    | 31   |
| BUSY    | 29   |
| RESET   | 32   |
| DIO2    | NC (internal RF switch on the SX1262 module) |

## Toolchain

ESP32-P4 is **not** supported by stock `platform-espressif32 @ 6.x`. This variant uses the pioarduino fork:

```
platform = https://github.com/pioarduino/platform-espressif32/releases/download/54.03.20/platform-espressif32.zip
```

If PlatformIO can't find the `esp32-p4-evboard` board definition, bump the pioarduino release to the newest tag — boards move around between releases.

## What's in the Phase 1 build

- `variants/crowpanel_dhe04005d/` — board class, target (SX1262 RadioLib wrapper), shared `board_config.h`.
- `examples/crowpanel_p4_lvgl_chat/` — minimal `main.cpp` that brings up LVGL + GT911 via Elecrow's `ESP32_Display_Panel` BSP, then calls `radio_init()` and shows a status screen.
- `lib/ESP32_Display_Panel`, `lib/ESP32_IO_Expander`, `lib/esp-lib-utils` — copied verbatim from Elecrow's Lesson09 reference.

## What's intentionally NOT in Phase 1

- Chat UI / repeater UI / settings UI (to be ported from `examples/crowpanel_lvgl_chat` in Phase 2).
- WiFi dashboard, OTA, Telegram bridge, NTP — the P4's WiFi runs on a preprogrammed C6 coprocessor over ESP-Hosted, which is a separate integration effort.

## Build

```
pio run -e crowpanel_p4_lvgl_chat
pio run -e crowpanel_p4_lvgl_chat -t upload
pio device monitor -e crowpanel_p4_lvgl_chat
```

On success the screen shows board info + `Radio OK  869.618 MHz  ...` and the serial monitor prints the same.
