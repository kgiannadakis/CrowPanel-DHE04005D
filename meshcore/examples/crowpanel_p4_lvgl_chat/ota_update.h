#pragma once
// ota_update.h — OTA firmware update from GitHub Releases

void ota_init();
void ota_loop();
void ota_check_for_update();
void ota_set_repo(const char* repo);
void ota_save_settings();
void ota_populate_ui();

bool ota_is_checking();
bool ota_is_updating();
uint8_t ota_progress_percent();
const char* ota_status_text();
