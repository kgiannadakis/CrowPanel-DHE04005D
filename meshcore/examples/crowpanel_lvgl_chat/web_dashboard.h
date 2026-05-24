#pragma once
// web_dashboard.h — Async web server with REST API and WebSocket

void webdash_init();
void webdash_loop();
void webdash_start();
void webdash_stop();
void webdash_save_settings();

void webdash_broadcast_message(const char* source, const char* text, bool outgoing, const char* target_key = nullptr);
const char* webdash_status_text();
