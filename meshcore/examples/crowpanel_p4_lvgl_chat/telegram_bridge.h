#pragma once
// telegram_bridge.h — Bidirectional Telegram Bot API bridge

void tgbridge_init();
void tgbridge_loop();
void tgbridge_save_settings();
void tgbridge_set_token(const char* token);
void tgbridge_set_chat_id(const char* chat_id);
void tgbridge_populate_ui();

// PM: sender + text. Topic thread is keyed by the remote contact name.
void tgbridge_forward_pm(const char* sender, const char* remote_name, const char* text, bool is_outgoing);

// Channel: sender + text (separate). Topic thread is keyed by channel name.
void tgbridge_forward_channel(const char* channel_name, const char* sender, const char* text);

const char* tgbridge_status_text();
