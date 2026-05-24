// wifi_features_stub.cpp
// -----------------------------------------------------------------------------
// Temporary no-op implementations for wifi_ntp, ota_update, telegram_bridge,
// web_dashboard, and translate. These APIs are declared by the headers copied
// from the v11 build, and main.cpp + settings_cb.cpp call into them.
//
// TODO (next phase): replace each stub group with a real port that uses the
// C6's WiFi (already working via ESP-Hosted). Start with wifi_ntp + ota_update,
// then telegram + web_dashboard + translate. Drop symbols from here as they
// get real impls in dedicated .cpp files.
//
// Keeping this as ONE file means we can gut it piece-by-piece without breaking
// builds in between.
// -----------------------------------------------------------------------------

#include <Arduino.h>
#include <lvgl.h>

#include "wifi_ntp.h"
#include "ota_update.h"
#include "telegram_bridge.h"
#include "web_dashboard.h"
#include "translate.h"

// wifi_ntp: real implementation lives in wifi_ntp.cpp — nothing to stub.

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

// web_dashboard: real implementation lives in web_dashboard.cpp.

// translate: real implementation lives in translate.cpp.
