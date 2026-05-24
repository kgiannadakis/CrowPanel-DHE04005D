#include "ds3231.h"
#include "i2c_bus.h"
#include <driver/i2c_master.h>
#include <esp_log.h>
#include <string.h>
#include <time.h>

static const char* TAG = "rtc";

static constexpr uint8_t  RTC_ADDR        = 0x68;
static constexpr uint32_t RTC_FREQ_HZ     = 100000;
static constexpr int      I2C_TIMEOUT_MS  = 100;

static constexpr uint8_t REG_SECONDS = 0x00;
static constexpr uint8_t REG_STATUS  = 0x0F;

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

bool ds3231_begin(void) {
  if (s_dev) return s_present;

  i2c_master_bus_handle_t bus = i2c1_bus_handle();
  if (!bus) {
    ESP_LOGE(TAG, "I2C bus unavailable");
    return false;
  }

  i2c_device_config_t cfg = {};
  cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  cfg.device_address  = RTC_ADDR;
  cfg.scl_speed_hz    = RTC_FREQ_HZ;
  if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) {
    ESP_LOGE(TAG, "i2c_master_bus_add_device failed");
    s_dev = nullptr;
    return false;
  }

  uint8_t sec = 0, status = 0;
  if (!read_regs(REG_SECONDS, &sec, 1)) {
    ESP_LOGW(TAG, "No DS3231 at 0x%02X — disabled", RTC_ADDR);
    i2c_master_bus_rm_device(s_dev);
    s_dev = nullptr;
    return false;
  }
  read_regs(REG_STATUS, &status, 1);

  s_present = true;
  ESP_LOGI(TAG, "DS3231 detected at 0x%02X (OSF=%d)",
           RTC_ADDR, (status & 0x80) ? 1 : 0);
  return true;
}

bool ds3231_present(void) { return s_present; }

bool ds3231_read_unix(uint32_t* out_utc) {
  if (!s_present || !out_utc) return false;

  uint8_t status = 0;
  if (read_regs(REG_STATUS, &status, 1) && (status & 0x80)) {
    return false;
  }

  uint8_t r[7];
  if (!read_regs(REG_SECONDS, r, 7)) return false;

  int s  = bcd_to_bin(r[0] & 0x7F);
  int mi = bcd_to_bin(r[1] & 0x7F);
  int h  = bcd_to_bin(r[2] & 0x3F);
  int d  = bcd_to_bin(r[4] & 0x3F);
  int mo = bcd_to_bin(r[5] & 0x1F);
  int y  = bcd_to_bin(r[6]) + 2000;

  if (mo < 1 || mo > 12 || d < 1 || d > 31) return false;

  *out_utc = utc_ymdhms_to_epoch(y, mo, d, h, mi, s);
  return true;
}

bool ds3231_write_unix(uint32_t utc) {
  if (!s_present) return false;

  time_t t = (time_t)utc;
  struct tm tm;
  gmtime_r(&t, &tm);

  if (tm.tm_year + 1900 < 2000 || tm.tm_year + 1900 > 2099) return false;

  uint8_t r[7];
  r[0] = bin_to_bcd(tm.tm_sec) & 0x7F;
  r[1] = bin_to_bcd(tm.tm_min);
  r[2] = bin_to_bcd(tm.tm_hour) & 0x3F;
  r[3] = (uint8_t)(tm.tm_wday + 1);
  r[4] = bin_to_bcd(tm.tm_mday);
  r[5] = bin_to_bcd(tm.tm_mon + 1) & 0x7F;
  r[6] = bin_to_bcd(tm.tm_year - 100);

  if (!write_regs(REG_SECONDS, r, 7)) return false;

  uint8_t status = 0;
  if (read_regs(REG_STATUS, &status, 1)) {
    status &= 0x7F;
    write_regs(REG_STATUS, &status, 1);
  }
  return true;
}
