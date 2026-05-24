#if defined(CROWPANEL_DHE04005D) && defined(ARCH_ESP32P4)

#include "esp_err.h"

extern "C" esp_err_t __wrap_esp_console_cmd_register(const void *)
{
    return ESP_OK;
}

#endif
