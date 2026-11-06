// Compatibility APIs for features not implemented in the CrowPanel P4 build.
// Wi-Fi/NTP, HTTP dashboard and translation have real implementations in their
// own modules. OTA and Telegram remain no-ops; see docs/crowpanel.md.
#include <stdint.h>

#include "ota_update.h"
#include "telegram_bridge.h"

// =================== ota_update stubs ========================================
void ota_init() {}
void ota_loop() {}
void ota_check_for_update() {}
void ota_set_repo(const char*) {}
void ota_save_settings() {}
void ota_populate_ui() {}
bool ota_is_checking()    { return false; }
bool ota_is_updating()    { return false; }
uint8_t ota_progress_percent() { return 0; }
const char* ota_status_text()  { return ""; }

// =================== telegram_bridge stubs ===================================
void tgbridge_init() {}
void tgbridge_loop() {}
void tgbridge_save_settings() {}
void tgbridge_set_token(const char*) {}
void tgbridge_set_chat_id(const char*) {}
void tgbridge_populate_ui() {}
void tgbridge_forward_pm(const char*, const char*, const char*, bool) {}
void tgbridge_forward_channel(const char*, const char*, const char*) {}
const char* tgbridge_status_text() { return ""; }

