#pragma once
// translate.h — Google Translate integration (free unofficial API)
//
// Inline drain model (matches DIS02050A). translate_loop() runs on the
// Arduino loop() task and processes at most one queue entry per call,
// so there's no task/mutex/semaphore stack and no coupling to the web
// dashboard's AsyncTCP lifecycle. Translations are identified by
// (chat_key, source_text) — no bubble pointers are stored, so the
// queue survives chat navigation and UI teardown without any
// stale-pointer hazard.
//
// A completed translation is ALWAYS written to the chat file. If the
// chat identified by chat_key is the one currently visible
// (g_current_chat_key), the corresponding bubble is also live-updated
// in place. If the user navigated away before the translation
// completed, the file write is enough — when they reopen the chat,
// load_chat_from_file() parses the {{TR}} marker and renders the
// inline translation automatically.

// lv_obj user_data marker set on every translation label we create,
// so find_bubble_for_source() in translate.cpp can skip bubbles that
// already carry one. Exposed so chat_ui.cpp's disk-restore path in
// chat_add() can tag the labels it creates from {{TR}} markers too
// — otherwise a subsequent manual long-press translate would stack a
// second translation label on top of a bubble that was already
// translated-from-disk.
#define TRANSLATE_LABEL_TAG 0xBEEF

void translate_init();
void translate_loop();

// Queue a translation. text is the body to translate; chat_key is the
// destination chat file key (ct_<hex> or ch_<idx>). Dedup is by
// (chat_key, text) so repeating the same body in the same chat
// short-circuits to a no-op while a prior request is still pending.
void translate_request_to_file(const char* text, const char* chat_key);

// Language helpers
const char* translate_lang_code(int idx);
const char* translate_lang_list();  // newline-separated for lv_dropdown
int translate_lang_count();
