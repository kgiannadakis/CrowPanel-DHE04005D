#pragma once
// ============================================================
// utils.h — Utility functions (sanitization, time, base64, etc.)
// ============================================================

#include <Arduino.h>
#include "app_globals.h"

// UTF-8 validation
int utf8_seq_len(uint8_t lead);
bool utf8_valid_seq(const uint8_t* p, int len);

// Text sanitization
void sanitize_ascii_inplace(char* s);
String sanitize_ascii_string(const char* s);
String sanitize_for_font_string(const char* s, const lv_font_t* font);

// Time
void mesh_set_time_epoch(uint32_t epoch);
void update_timelabel();
String time_string_now();

// SNR
int snr_to_bars(int8_t snr);
void snr_contact_update(const uint8_t* pub_key, int8_t snr);
int8_t snr_contact_get(const uint8_t* pub_key);

// Notification helpers
bool has_any_unread();
int notify_contact_find(const uint8_t* pub_key);
int notify_channel_find(int channel_idx);
void notify_contact_inc(const uint8_t* pub_key);
void notify_contact_clear(const uint8_t* pub_key);
uint16_t notify_contact_get(const uint8_t* pub_key);
void notify_channel_inc(int channel_idx);
void notify_channel_clear(int channel_idx);
uint16_t notify_channel_get(int channel_idx);

// Base64
String b64_encode_bytes(const uint8_t* data, size_t len);
String packet_signal_str(const mesh::Packet* pkt);

// Hashtag / channel helpers
String normalize_hashtag(const char* in);
void hashtag_to_secret16(const String& tag, uint8_t out16[16]);
String secret16_to_base64(const uint8_t sec16[16]);

// Misc
uint32_t parse_u32(const char* t, uint32_t fallback);
String fmt_duration(uint32_t secs);

// Serial monitor
void serialmon_append(const char* line);
void serialmon_append_color(uint32_t rgb, const char* line);

// Deferred message push
void deferred_msg_push(bool out, const char* txt, const char* sig = "", bool live_status = false, uint32_t msg_ts = 0);

// I2C helpers
bool i2c_ok(uint8_t a);
void i2c_cmd(uint8_t c);

// Screen helpers
void note_touch_activity();
void wake_on_event();
void screen_sleep();
void screen_wake_soft(uint8_t target);
void screen_wake();
