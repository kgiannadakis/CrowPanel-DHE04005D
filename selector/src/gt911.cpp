#include "gt911.h"
#include "i2c_bus.h"
#include "stc8.h"
#include "board_config.h"

#include <Arduino.h>
#include <esp_log.h>
#include <driver/gpio.h>

static const char* TAG = "gt911";

// GT911 slave addresses. The chip selects one based on INT level during reset.
// We'll probe both — on CrowPanel the default is 0x5D, but 0x14 occasionally
// shows up depending on how the STC8 reset sequence lands.
static constexpr uint8_t GT911_ADDR_A = 0x5D;
static constexpr uint8_t GT911_ADDR_B = 0x14;

// Registers we care about
static constexpr uint16_t REG_STATUS        = 0x814E;  // touch status + point count
static constexpr uint16_t REG_POINT1        = 0x814F;  // first point data
static constexpr uint16_t REG_PRODUCT_ID    = 0x8140;  // "911\0"

static constexpr uint32_t I2C_FREQ_HZ = 400000;
static constexpr int      I2C_TIMEOUT_MS = 50;

static i2c_master_dev_handle_t s_dev = nullptr;
static uint16_t s_panel_w = 800;
static uint16_t s_panel_h = 480;

// ---- helpers ----------------------------------------------------------------

static bool add_device(uint8_t addr) {
  i2c_master_bus_handle_t bus = i2c1_bus_handle();
  if (!bus) return false;
  i2c_device_config_t cfg = {};
  cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  cfg.device_address  = addr;
  cfg.scl_speed_hz    = I2C_FREQ_HZ;
  if (s_dev) {
    i2c_master_bus_rm_device(s_dev);
    s_dev = nullptr;
  }
  return i2c_master_bus_add_device(bus, &cfg, &s_dev) == ESP_OK;
}

static bool read_reg(uint16_t reg, uint8_t* buf, size_t len) {
  uint8_t addr[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
  return i2c_master_transmit_receive(s_dev, addr, 2, buf, len, I2C_TIMEOUT_MS) == ESP_OK;
}

static bool write_reg(uint16_t reg, uint8_t value) {
  uint8_t buf[3] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF), value };
  return i2c_master_transmit(s_dev, buf, 3, I2C_TIMEOUT_MS) == ESP_OK;
}

// ---- public API -------------------------------------------------------------

bool gt911_init(uint16_t panel_w, uint16_t panel_h) {
  s_panel_w = panel_w;
  s_panel_h = panel_h;

  // RST (IO36) is wired directly to the GT911 reset line on this board
  // (0R resistor per schematic). STC8 P1.2 is only a weak 27K secondary path,
  // so drive IO36 from the P4 directly — that's what actually resets the chip.
  gpio_config_t rst_cfg = {};
  rst_cfg.pin_bit_mask = 1ULL << Touch_GPIO_RST;
  rst_cfg.mode         = GPIO_MODE_OUTPUT;
  gpio_config(&rst_cfg);

  // INT as plain input. With INT low during the RST rising edge the chip
  // selects slave address 0x5D; with INT high it picks 0x14. We leave INT
  // floating here, which in practice yields 0x5D on CrowPanel.
  gpio_config_t ic = {};
  ic.pin_bit_mask = 1ULL << Touch_GPIO_INT;
  ic.mode         = GPIO_MODE_INPUT;
  gpio_config(&ic);

  // Reset pulse. Drive LOW ≥10 ms, then HIGH, then let the chip finish init.
  gpio_set_level((gpio_num_t)Touch_GPIO_RST, 0);
  // Also toggle the STC8-owned path so both ends agree the chip is in reset.
  stc8_gpio_set_level(STC8_GPIO_OUT_TP_RST, 0);
  delay(20);
  gpio_set_level((gpio_num_t)Touch_GPIO_RST, 1);
  stc8_gpio_set_level(STC8_GPIO_OUT_TP_RST, 1);
  delay(60);

  // Try the primary address first, then the alternate.
  uint8_t pid[4] = {0};
  for (uint8_t addr : { GT911_ADDR_A, GT911_ADDR_B }) {
    if (!add_device(addr)) continue;
    if (read_reg(REG_PRODUCT_ID, pid, sizeof(pid))
        && pid[0] == '9' && pid[1] == '1' && pid[2] == '1') {
      ESP_LOGI(TAG, "GT911 found at 0x%02X, product_id=\"%c%c%c%c\"",
               addr, pid[0], pid[1], pid[2], pid[3]);
      return true;
    }
  }
  ESP_LOGE(TAG, "GT911 not responding on 0x5D or 0x14 (pid=%02X %02X %02X %02X)",
           pid[0], pid[1], pid[2], pid[3]);
  return false;
}

bool gt911_read(uint16_t* x, uint16_t* y) {
  if (!s_dev) return false;

  uint8_t status = 0;
  if (!read_reg(REG_STATUS, &status, 1)) return false;

  // A properly responding GT911 returns 0 here when idle. A silent bus
  // returns 0xFF for everything and (0xFF & 0x80) passes, producing
  // ghost touches. Reject the all-ones pattern explicitly.
  if (status == 0xFF) { write_reg(REG_STATUS, 0); return false; }

  bool touched = (status & 0x80) && ((status & 0x0F) > 0);
  uint16_t tx = 0, ty = 0;

  if (touched) {
    // Layout starting at 0x814F: [trackId, xLo, xHi, yLo, yHi, sizeLo, sizeHi, rsvd]
    uint8_t p[8] = {0};
    if (read_reg(REG_POINT1, p, sizeof(p))) {
      tx = (uint16_t)p[1] | ((uint16_t)p[2] << 8);
      ty = (uint16_t)p[3] | ((uint16_t)p[4] << 8);
    } else {
      touched = false;
    }
  }

  // ALWAYS clear the status register so the chip refills the touch buffer —
  // even when the buffer-ready bit was not set this cycle.
  write_reg(REG_STATUS, 0);

  if (touched) {
    if (tx >= s_panel_w) tx = s_panel_w - 1;
    if (ty >= s_panel_h) ty = s_panel_h - 1;
    if (x) *x = tx;
    if (y) *y = ty;
  }
  return touched;
}
