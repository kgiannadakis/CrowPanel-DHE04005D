# CrowPanel DHE04005D MeshCore Variant

This is the active board layer for the ESP32-P4 touchscreen chat firmware.
See the [development guide](../../docs/crowpanel.md) for module ownership,
checks, supported features and release preparation.

## Hardware

- Elecrow CrowPanel Advanced 5-inch DHE04005D, 800x480 RGB IPS display.
- ESP32-P4 with 16 MB flash and 32 MB PSRAM.
- GT911 touch and STC8 backlight/battery control on the shared I2C bus.
- ESP32-C6 Wi-Fi coprocessor using ESP-Hosted.
- SX1262 LoRa module, wired according to `board_config.h`.
- Optional external RTC at I2C address `0x68`.

## LoRa Wiring

Keep the pin definitions aligned with `board_config.h` and `platformio.ini`.

| Signal | GPIO |
| --- | --- |
| SCK | 26 |
| MISO | 47 |
| MOSI | 48 |
| NSS | 30 |
| DIO1 | 31 |
| BUSY | 29 |
| RESET | 32 |
| DIO2 | Module RF-switch control, not an ESP32 GPIO |

The radio uses the ESP-IDF SPI HAL in `EspHal.h`, not the Arduino SPI path.
See `target.cpp` for the board-specific reason. Boosted receive gain and its
retention setting are configured there and checked by the main loop.

## Toolchain and Build

The active environment pins pioarduino `55.03.36-1` / Arduino-ESP32 `3.3.6`.
Do not substitute the stock ESP32 platform or upgrade dependencies during
unrelated cleanup.

From `meshcore/`:

```sh
platformio run -e dhe04005d-5inch-P4-meshcore
platformio device monitor -e dhe04005d-5inch-P4-meshcore --port YOUR_PORT
python tools/check_crowpanel.py
```

For complete-device flashing, follow the [repository installation guide](../../../README.md).
The active partition table is `meshcore/partitions_p4.csv`; it shares its layout
with the selector and Meshtastic firmware. Do not change offsets independently.

## Driver Ownership

- `target.cpp`, `target.h`, `CrowPanelP4Board.h`: MeshCore board/radio glue.
- `board_config.h`: display, touch, radio and shared-bus pin definitions.
- `i2c_bus.*`: shared I2C bus access.
- `gt911.*`: touch controller and wake handling.
- `stc8.*`: backlight PWM, battery sampling and controller state.
- `ds1307.*`: compatible external RTC driver.

Display/LVGL lifecycle is implemented in `examples/crowpanel_p4_lvgl_chat/display.cpp`.
The board drivers must not manipulate LVGL widgets directly.
