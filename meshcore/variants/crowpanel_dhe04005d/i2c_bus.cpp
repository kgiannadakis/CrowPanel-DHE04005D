#include "i2c_bus.h"
#include "board_config.h"
#include <esp_log.h>

static const char* TAG = "i2c_bus";
static i2c_master_bus_handle_t s_bus = NULL;

i2c_master_bus_handle_t i2c1_bus_handle(void) {
  if (s_bus != NULL) return s_bus;

  i2c_master_bus_config_t cfg = {};
  cfg.clk_source              = I2C_CLK_SRC_DEFAULT;
  cfg.i2c_port                = I2C_NUM_0;   // the new driver has no legacy claim now
  cfg.sda_io_num              = (gpio_num_t)I2C_GPIO_SDA;
  cfg.scl_io_num              = (gpio_num_t)I2C_GPIO_SCL;
  cfg.glitch_ignore_cnt       = 7;
  cfg.flags.enable_internal_pullup = true;

  esp_err_t err = i2c_new_master_bus(&cfg, &s_bus);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "i2c_new_master_bus failed: %d", (int)err);
    s_bus = NULL;
    return NULL;
  }
  ESP_LOGI(TAG, "I2C1 master bus ready (SCL=%d SDA=%d)", I2C_GPIO_SCL, I2C_GPIO_SDA);
  return s_bus;
}
