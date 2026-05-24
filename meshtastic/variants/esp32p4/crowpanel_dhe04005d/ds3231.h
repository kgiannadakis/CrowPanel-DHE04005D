#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool ds3231_begin(void);

bool ds3231_present(void);

bool ds3231_read_unix(uint32_t* out_utc);

bool ds3231_write_unix(uint32_t utc);

#ifdef __cplusplus
}
#endif
