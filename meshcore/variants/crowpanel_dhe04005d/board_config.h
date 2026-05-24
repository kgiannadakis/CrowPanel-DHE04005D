#pragma once
// ============================================================
// board_config.h — CrowPanel DHE04005D (ESP32-P4 + 5" 800x480 RGB)
// ------------------------------------------------------------
// Lifted from Elecrow's reference Lesson09 example.
// Kept in the variant folder so both the MeshCore target code
// and the LVGL port can share a single source of pin truth.
// ============================================================

// ---------- GT911 capacitive touch ----------
#define Touch_GPIO_RST      (36)
#define Touch_GPIO_INT      (42)

// ---------- I2C (touch + PH2.0-4P expansion header) ----------
#define I2C_GPIO_SCL        (46)
#define I2C_GPIO_SDA        (45)

// ---------- Display panel geometry ----------
#define H_size              (800)
#define V_size              (480)

// ---------- RGB panel timings ----------
// Byte-for-byte copy of Elecrow's Lesson09 bsp_illuminate.h. Same macros
// render cleanly in their idf-code tree; on our pioarduino build (IDF
// 5.5.4) the scan engine has a fixed -8 column / -1 row origin offset
// versus the FB. Tested: HBP/VBP tuning has no effect on the offset
// (the DMA-start timing tracks back-porch, so the visible image and
// the FB-read pointer shift together). We work around it at the LVGL
// layer by rendering at 792×479 and letting the overflow columns/row
// stay black — see kLandWidth/kLandHeight in display.cpp.
// Tested lowering to 12 MHz as a PSRAM-bandwidth A/B — panel dropped
// into a "white/red/blue/green" colour-bar pattern, meaning the RGB
// peripheral's bounce-buffer DMA underruns at that rate with our
// current bounce_buffer_size_px = H_size * 20 setting. 18 MHz is the
// lower edge that still feeds reliably. To actually test the BW
// hypothesis we'd need to simultaneously reduce PCLK AND bump the
// bounce buffer so the FIFO stays primed — otherwise we break scan
// before we can measure. Leaving at 18 until someone has time for that.
#define LCD_CLK_MHZ         (16)
#define LCD_HPW             ( 4)
#define LCD_HBP             ( 8)
#define LCD_HFP             ( 8)
#define LCD_VPW             ( 4)
#define LCD_VBP             (16)
#define LCD_VFP             (16)

// ---------- RGB panel control pins ----------
#define LCD_GPIO_RST        (-1)
#define RGB_PIN_NUM_DISP_EN (-1)
#define RGB_PIN_NUM_HSYNC   (40)
#define RGB_PIN_NUM_VSYNC   (41)
#define RGB_PIN_NUM_DE      ( 2)
#define RGB_PIN_NUM_PCLK    ( 3)

// ---------- RGB panel 16-bit data bus ----------
#define RGB_PIN_NUM_DATA0   ( 8)
#define RGB_PIN_NUM_DATA1   ( 7)
#define RGB_PIN_NUM_DATA2   ( 6)
#define RGB_PIN_NUM_DATA3   ( 5)
#define RGB_PIN_NUM_DATA4   ( 4)
#define RGB_PIN_NUM_DATA5   (14)
#define RGB_PIN_NUM_DATA6   (13)
#define RGB_PIN_NUM_DATA7   (12)
#define RGB_PIN_NUM_DATA8   (11)
#define RGB_PIN_NUM_DATA9   (10)
#define RGB_PIN_NUM_DATA10  ( 9)
#define RGB_PIN_NUM_DATA11  (19)
#define RGB_PIN_NUM_DATA12  (18)
#define RGB_PIN_NUM_DATA13  (17)
#define RGB_PIN_NUM_DATA14  (16)
#define RGB_PIN_NUM_DATA15  (15)

// ---------- ESP-Hosted SDIO → on-board ESP32-C6 (WiFi / BT) ----------
// CrowPanel routes the P4-to-C6 ESP-Hosted link on these pins; arduino-esp32's
// defaults don't match this board, so we set them explicitly via WiFi.setPins().
#define WIFI_HOSTED_SDIO_PIN_CMD    (54)
#define WIFI_HOSTED_SDIO_PIN_CLK    (53)
#define WIFI_HOSTED_SDIO_PIN_D0     (52)
#define WIFI_HOSTED_SDIO_PIN_D1     (51)
#define WIFI_HOSTED_SDIO_PIN_D2     (50)
#define WIFI_HOSTED_SDIO_PIN_D3     (49)
#define WIFI_HOSTED_SDIO_PIN_RESET  (20)

// ---------- microSD (SDMMC slot 0, 1-bit) ----------
// Pins lifted from Elecrow's Lesson08 SD-card example for this board.
// This is a separate SDMMC slot from the ESP-Hosted SDIO link to the
// C6 (which lives on pins 49–54 above), so map tiles / emoji PNGs /
// music / whatever can be read from the card without contending with
// the Wi-Fi/BT peripheral. 1-bit mode, 10 MHz max — matches Elecrow.
#define SD_GPIO_MMC_CLK     (43)
#define SD_GPIO_MMC_CMD     (44)
#define SD_GPIO_MMC_D0      (39)

// ---------- I2S speaker / DAC ----------
// Wired to the PH2.0-3P audio header. Standard (Philips) I2S mode:
// LRCK/WS selects left/right channel, BCLK clocks each bit, SDOUT is
// the serial audio data line. Driven via ESP-IDF's i2s_std driver —
// see beep_*() in utils.cpp.
#define SPK_I2S_LRCK_GPIO   (21)
#define SPK_I2S_BCLK_GPIO   (22)
#define SPK_I2S_SDOUT_GPIO  (23)

// ---------- SX1262 LoRa (external module wired to pinheaders) ----------
// Note: MISO/MOSI consume the UART1 PH2.0-4P header (IO47/IO48).
#define PIN_SPI_SCK         (26)
#define PIN_SPI_MOSI        (48)
#define PIN_SPI_MISO        (47)
#define LORA_RESET          (32)
#define LORA_DIO1           (31)
#define LORA_BUSY           (29)
#define LORA_CS             (30)
// DIO2 is NC on the header; the SX1262 module uses DIO2 internally as
// RF switch (same module as the DIS05020A v1.1 build).
