#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool gt911_init(uint16_t panel_w, uint16_t panel_h);

bool gt911_read(uint16_t* x, uint16_t* y);

#ifdef __cplusplus
}
#endif
