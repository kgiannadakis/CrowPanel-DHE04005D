#define I2C_SDA 15
#define I2C_SCL 16

#if CROW_SELECT == 1
#define WAKE_ON_TOUCH
#define SCREEN_TOUCH_INT 47
#define USE_POWERSAVE
#define SLEEP_TIME 180
#endif

#if CROW_SELECT == 1
// dac / amp
// #define HAS_I2S // didn't get I2S sound working
#define PIN_BUZZER -1 // using pwm buzzer instead (nobody will notice, lol)
#define DAC_I2S_BCK 13
#define DAC_I2S_WS 11
#define DAC_I2S_DOUT 12
#define DAC_I2S_MCLK 8 // don't use GPIO0 because it's assigned to LoRa or button
#else
#define PIN_BUZZER -1
#endif

// GPS via UART1 connector
#define GPS_DEFAULT_NOT_PRESENT 1
#if CROW_SELECT == 1
#define HAS_GPS 1
#define GPS_RX_PIN 18
#define GPS_TX_PIN 17
#else
// Large panels route GPIO19/GPIO20 to SX1262 reset/DIO1. Do not expose GPS
// here; a stale config that enables Serial1 on these pins breaks LoRa RX/TX.
#define HAS_GPS 0
#endif

// Extension Slot Layout, viewed from above (2.4-3.5)
// DIO1/IO1 o   o IO2/NRESET
// SCK/IO10 o   o IO16/NC
// MISO/IO9 o   o IO15/NC
// MOSI/IO3 o   o NC/DIO2
//      3V3 o   o IO46/BUSY
//      GND o   o IO0/NSS
//    5V/NC o   o NC/DIO3
//         J9   J8

// Extension Slot Layout, viewed from above (4.3-7.0)
// !! DIO1/IO20 o   o IO19/NRESET !!
// !!   SCK/IO5 o   o IO16/NC
// !!  MISO/IO4 o   o IO15/NC
// !!  MOSI/IO6 o   o NC/DIO2
//          3V3 o   o IO2/BUSY !!
//          GND o   o IO0/NSS
//        5V/NC o   o NC/DIO3
//             J9   J8

// LoRa
#define USE_SX1262

#if CROW_SELECT == 1
// 2.4", 2.8, 3.5"""
#define HW_SPI1_DEVICE
#define LORA_CS 0
#define LORA_SCK 10
#define LORA_MISO 9
#define LORA_MOSI 3

#define LORA_RESET 2
#define LORA_DIO1 1  // SX1262 IRQ
#define LORA_DIO2 46 // SX1262 BUSY

// need to pull IO45 low to enable LORA and disable Microphone on 24 28 35
#define SENSOR_POWER_CTRL_PIN 45
#define SENSOR_POWER_ON LOW
#else
// 4.3", 5.0", 7.0"
#define LORA_CS 8
#define LORA_SCK 5
#define LORA_MISO 4
#define LORA_MOSI 6

#define LORA_RESET 19
#define LORA_DIO1 20 // SX1262 IRQ
#define LORA_DIO2 2  // SX1262 BUSY
#endif

#define SX126X_CS LORA_CS
#define SX126X_DIO1 LORA_DIO1
#define SX126X_BUSY LORA_DIO2
#define SX126X_RESET LORA_RESET
#define SX126X_DIO2_AS_RF_SWITCH
// TCXO reference voltage delivered on SX1262 DIO3 to power the external
// TCXO part sitting next to the radio. Set to 1.8 V to match the Semtech
// SX1262 reference design default, which is what virtually every other
// Heltec / Ebyte / T-Beam / RAK variant in this tree uses. This file
// previously specified 3.3 V which is the topmost bin of the SX1262's
// discrete DIO3 voltage selector (1.6, 1.7, 1.8, 2.2, 2.4, 2.7, 3.0,
// 3.3 V) — empirically swapping 3.3 -> 1.8 had no measurable effect on
// the reported noise floor on this specific board, so treat the 1.8 V
// choice as "match the reference design" rather than as a proven
// sensitivity fix.
#define SX126X_DIO3_TCXO_VOLTAGE 1.8

// CrowPanel PA path supports the high-power SX1262 setting.
#define SX126X_MAX_POWER 27
#define DEFAULT_LORA_TX_POWER 20

#if CROW_SELECT != 1
// CrowPanel RF experiment switches. Override these from PlatformIO build_flags
// for A/B firmware without hand-editing the radio driver.
#ifndef SX126X_REGISTER_PATCH_0X8B5
#define SX126X_REGISTER_PATCH_0X8B5 1
#endif
#ifndef SX126X_FORCE_CONTINUOUS_RX
#define SX126X_FORCE_CONTINUOUS_RX 0
#endif
#ifndef SX126X_AGC_RESET_INTERVAL_MS
#define SX126X_AGC_RESET_INTERVAL_MS (5 * 60 * 1000)
#endif
#endif

// Build-time pin conflict guards. These are intentionally blunt: on the large
// RGB panels the LoRa IRQ/reset pins are easy to steal with stale GPS-style
// assumptions, and that produces "deaf radio" symptoms instead of obvious
// compile errors.
#if HAS_GPS && defined(GPS_RX_PIN) && \
    ((GPS_RX_PIN == LORA_CS) || (GPS_RX_PIN == LORA_SCK) || (GPS_RX_PIN == LORA_MISO) || \
     (GPS_RX_PIN == LORA_MOSI) || (GPS_RX_PIN == LORA_RESET) || (GPS_RX_PIN == LORA_DIO1) || \
     (GPS_RX_PIN == LORA_DIO2))
#error "CrowPanel GPS_RX_PIN conflicts with LoRa pins"
#endif

#if HAS_GPS && defined(GPS_TX_PIN) && \
    ((GPS_TX_PIN == LORA_CS) || (GPS_TX_PIN == LORA_SCK) || (GPS_TX_PIN == LORA_MISO) || \
     (GPS_TX_PIN == LORA_MOSI) || (GPS_TX_PIN == LORA_RESET) || (GPS_TX_PIN == LORA_DIO1) || \
     (GPS_TX_PIN == LORA_DIO2))
#error "CrowPanel GPS_TX_PIN conflicts with LoRa pins"
#endif

#if defined(SDCARD_CS) && ((SDCARD_CS == LORA_CS) || (SDCARD_CS == LORA_RESET) || \
                          (SDCARD_CS == LORA_DIO1) || (SDCARD_CS == LORA_DIO2))
#error "CrowPanel SDCARD_CS conflicts with LoRa control pins"
#endif

#if ((I2C_SDA == LORA_RESET) || (I2C_SDA == LORA_DIO1) || (I2C_SDA == LORA_DIO2) || \
     (I2C_SCL == LORA_RESET) || (I2C_SCL == LORA_DIO1) || (I2C_SCL == LORA_DIO2))
#error "CrowPanel I2C pins conflict with LoRa IRQ/reset/busy pins"
#endif
