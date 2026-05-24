// I2C-RTC driver. Originally written for DS1307; the wired chip on this
// board is actually a DS3231SN (Maxim's TCXO-stabilised drop-in for
// DS1307 at the same I2C address 0x68). The two chips share the same
// register layout for 0x00..0x06 (BCD time + date), so the reads/writes
// here are identical for both. The DS3231-specific bits handled below:
//
//   - reg 0x0F bit 7 = OSF (Oscillator Stop Flag). Set whenever the
//     oscillator was stopped (first power-up, dead VBAT). Time in
//     0x00..0x06 is only valid when OSF=0. We check this on read and
//     clear it after a successful write so subsequent boots see a
//     valid time.
//   - reg 0x05 bit 7 = Century. Always 0 for the 2000..2099 window we
//     support, so we mask it off on read and write 0.
//
// File/function names kept as `ds1307_*` for minimal call-site churn —
// they're just labels.
#include "ds1307.h"
#include "i2c_bus.h"
#include <Arduino.h>                 // Serial
#include <driver/i2c_master.h>
#include <esp_log.h>
#include <string.h>
#include <time.h>

static const char* TAG = "rtc";

// I2C
static constexpr uint8_t  RTC_ADDR        = 0x68;
static constexpr uint32_t RTC_FREQ_HZ     = 100000;   // safe for both DS1307 + DS3231
static constexpr int      I2C_TIMEOUT_MS  = 100;

// Register map (subset we use)
static constexpr uint8_t REG_SECONDS = 0x00;   // bit 7 = CH on DS1307, reserved on DS3231
static constexpr uint8_t REG_STATUS  = 0x0F;   // DS3231 only: bit 7 = OSF

static i2c_master_dev_handle_t s_dev     = nullptr;
static bool                    s_present = false;

static inline uint8_t bcd_to_bin(uint8_t v) { return (v & 0x0F) + 10 * (v >> 4); }
static inline uint8_t bin_to_bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

static bool read_regs(uint8_t reg, uint8_t* buf, size_t len) {
  if (!s_dev) return false;
  return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, I2C_TIMEOUT_MS) == ESP_OK;
}

static bool write_regs(uint8_t reg, const uint8_t* buf, size_t len) {
  if (!s_dev || len > 7) return false;
  uint8_t packet[8];
  packet[0] = reg;
  memcpy(&packet[1], buf, len);
  return i2c_master_transmit(s_dev, packet, len + 1, I2C_TIMEOUT_MS) == ESP_OK;
}

// Convert UTC y/m/d/h/m/s to Unix epoch — TZ-independent unlike mktime().
// Valid for 1970..2099 (chip range).
static uint32_t utc_ymdhms_to_epoch(int y, int mo, int d, int h, int mi, int s) {
  static const int16_t cum[12] = {0,31,59,90,120,151,181,212,243,273,304,334};
  uint32_t days = 0;
  for (int yy = 1970; yy < y; yy++) {
    bool leap = (yy % 4 == 0 && yy % 100 != 0) || yy % 400 == 0;
    days += leap ? 366 : 365;
  }
  days += cum[mo - 1] + (d - 1);
  bool leap_y = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
  if (mo > 2 && leap_y) days++;
  return days * 86400UL + (uint32_t)h * 3600 + (uint32_t)mi * 60 + (uint32_t)s;
}

// Scan every 7-bit address on the shared I2C bus and log which ones
// ACK. Takes ~300 ms (128 probes × 2 ms each). Run once at boot for
// diagnostics so we can see exactly what's on the bus when debugging
// RTC/touch/accessory issues.
static void i2c_bus_scan(i2c_master_bus_handle_t bus) {
  Serial.println("[rtc] I2C bus scan 0x08..0x77:");
  int found = 0;
  for (uint8_t addr = 0x08; addr < 0x78; addr++) {
    if (i2c_master_probe(bus, addr, 20) == ESP_OK) {
      Serial.printf("[rtc]   0x%02X ACK\n", addr);
      found++;
    }
  }
  Serial.printf("[rtc] scan done - %d device(s) responded\n", found);
}

bool ds1307_begin(void) {
  if (s_dev) return s_present;

  Serial.println("[rtc] ds1307_begin() entering");

  i2c_master_bus_handle_t bus = i2c1_bus_handle();
  if (!bus) {
    Serial.println("[rtc] ERROR: I2C bus unavailable");
    return false;
  }

  i2c_bus_scan(bus);

  i2c_device_config_t cfg = {};
  cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  cfg.device_address  = RTC_ADDR;
  cfg.scl_speed_hz    = RTC_FREQ_HZ;
  if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) {
    Serial.println("[rtc] ERROR: i2c_master_bus_add_device failed");
    s_dev = nullptr;
    return false;
  }

  uint8_t sec = 0, status = 0;
  if (!read_regs(REG_SECONDS, &sec, 1)) {
    Serial.printf("[rtc] No RTC at 0x%02X — disabled\n", RTC_ADDR);
    i2c_master_bus_rm_device(s_dev);
    s_dev = nullptr;
    return false;
  }
  // OSF is meaningful only on DS3231; on DS1307 reg 0x0F is user SRAM
  // and reads back whatever was last written there (or 0xFF on a fresh
  // chip). Read it for diagnostic logging only.
  read_regs(REG_STATUS, &status, 1);

  s_present = true;
  Serial.printf("[rtc] RTC detected at 0x%02X (sec=0x%02X CH=%d status=0x%02X OSF=%d)\n",
                RTC_ADDR, sec, (sec & 0x80) ? 1 : 0, status, (status & 0x80) ? 1 : 0);
  return true;
}

bool ds1307_present(void) { return s_present; }

bool ds1307_read_unix(uint32_t* out_utc) {
  if (!s_present || !out_utc) return false;

  // DS3231: if OSF is set, the oscillator was stopped at some point —
  // time registers contain stale or undefined data. Reject.
  // DS1307: reg 0x0F is SRAM and unrelated; we keep it 0 by clearing
  // OSF after every successful write, so this check is benign there too.
  uint8_t status = 0;
  if (read_regs(REG_STATUS, &status, 1) && (status & 0x80)) {
    return false;
  }

  uint8_t r[7];
  if (!read_regs(REG_SECONDS, r, 7)) return false;
  if (r[0] & 0x80) return false;   // CH (DS1307) set → time invalid

  int s  = bcd_to_bin(r[0] & 0x7F);
  int mi = bcd_to_bin(r[1] & 0x7F);
  int h  = bcd_to_bin(r[2] & 0x3F);   // mask forces 24h regardless of bit 6
  int d  = bcd_to_bin(r[4] & 0x3F);
  int mo = bcd_to_bin(r[5] & 0x1F);   // strip century bit (bit 7) for DS3231
  int y  = bcd_to_bin(r[6]) + 2000;   // year is 00..99 relative to 2000

  if (mo < 1 || mo > 12 || d < 1 || d > 31) return false;   // corrupt

  *out_utc = utc_ymdhms_to_epoch(y, mo, d, h, mi, s);
  return true;
}

bool ds1307_write_unix(uint32_t utc) {
  if (!s_present) return false;

  time_t t = (time_t)utc;
  struct tm tm;
  gmtime_r(&t, &tm);   // TZ-independent UTC breakdown

  if (tm.tm_year + 1900 < 2000 || tm.tm_year + 1900 > 2099) return false;

  uint8_t r[7];
  r[0] = bin_to_bcd(tm.tm_sec) & 0x7F;      // clears CH bit (DS1307)
  r[1] = bin_to_bcd(tm.tm_min);
  r[2] = bin_to_bcd(tm.tm_hour) & 0x3F;     // 24h mode (bit 6 = 0)
  r[3] = (uint8_t)(tm.tm_wday + 1);         // DOW 1..7 (just stored, not validated)
  r[4] = bin_to_bcd(tm.tm_mday);
  r[5] = bin_to_bcd(tm.tm_mon + 1) & 0x7F;  // century bit (bit 7) cleared
  r[6] = bin_to_bcd(tm.tm_year - 100);      // years since 2000

  if (!write_regs(REG_SECONDS, r, 7)) return false;

  // DS3231: clear OSF in the status register so future reads return
  // valid time. Read-modify-write to preserve the other status bits
  // (EN32kHz, alarm flags, etc.). Harmless on DS1307 — we just leave a
  // 0x00 in SRAM byte 0x0F, which is what we want anyway.
  uint8_t status = 0;
  if (read_regs(REG_STATUS, &status, 1)) {
    status &= 0x7F;     // clear bit 7 (OSF)
    write_regs(REG_STATUS, &status, 1);
  }
  return true;
}
