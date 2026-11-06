#pragma once
#include "repeater_password_policy.h"

// Local LittleFS credentials, keyed by the full repeater public key. Not encrypted.
bool repeater_password_load(const uint8_t* key, char out[RepeaterPassword::kBufferBytes]);
bool repeater_password_save(const uint8_t* key, const char* password);
bool repeater_password_forget(const uint8_t* key);
