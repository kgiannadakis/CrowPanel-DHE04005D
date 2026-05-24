// translate.cpp - Google Translate via free unofficial API
// NOTE: Uses the unofficial translate.googleapis.com endpoint (no API key).
//       TLS certificate validation is disabled (setInsecure).
//       This is NOT the official Google Cloud Translation API.
//
// Inline drain model, ported verbatim from the DIS02050A (S3) build.
// An earlier P4 iteration used a dedicated FreeRTOS task with mutex
// and a webdash-toggle "settle window" to work around AsyncTCP heap
// contention; that was fragile (mbedtls ALLOC_FAILED under load,
// stale bubble pointers when chat navigation raced the task) and
// it's been replaced with the simpler one-per-loop drain. Running
// the TLS handshake on the Arduino loop() task keeps it serialised
// with every other consumer of the mbedtls heap, so contention
// disappears.
//
// Queue entries are now keyed on (chat_key, text) rather than on an
// lv_obj_t* bubble pointer. This means the queue survives chat
// switches, screen rebuilds, and chat_clear() — the pointer-lifetime
// problem that required translate_invalidate_bubbles() is gone.
// When a translation completes, it's appended to the chat file;
// if that chat is currently visible, the matching bubble is located
// by body text and live-updated under lvgl_lock.

#include "translate.h"
#include "app_globals.h"
#include "display.h"       // lvgl_lock / lvgl_unlock
#include "ui_theme.h"
#include "utils.h"
#include "persistence.h"

#include "ui.h"
#include "ui_homescreen.h"

#include <string.h>

#if defined(ESP32)
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#endif

extern bool g_wifi_connected;

// --- Language table ------------------------------------------------
// Languages whose scripts the bundled fonts can render.
// Montserrat covers ASCII + Latin Extended + Greek fallback.

struct LangEntry { const char* name; const char* code; };

static const LangEntry s_langs[] = {
    {"English", "en"},
    {"Greek",   "el"},
    {"Dutch",   "nl"},
    {"German",  "de"},
    {"Italian", "it"},
    {"French",  "fr"},
};
static const int LANG_COUNT = sizeof(s_langs) / sizeof(s_langs[0]);

const char* translate_lang_code(int idx) {
    if (idx < 0 || idx >= LANG_COUNT) return "en";
    return s_langs[idx].code;
}

int translate_lang_count() { return LANG_COUNT; }

static char s_lang_list[256] = "";

const char* translate_lang_list() {
    if (!s_lang_list[0]) {
        s_lang_list[0] = '\0';
        for (int i = 0; i < LANG_COUNT; i++) {
            if (i > 0) strcat(s_lang_list, "\n");
            strcat(s_lang_list, s_langs[i].name);
        }
    }
    return s_lang_list;
}

// --- Request queue -------------------------------------------------
// Sized to cover "open a chat with a lot of freshly received, never
// translated messages" — load_chat_from_file() enqueues every
// untranslated RX line in one pass. 16 slots is enough to absorb a
// typical history batch without dropping work.

#define TRANSLATE_QUEUE_SIZE 6
// Keep per-request payload modest to avoid large HTTP body/String churn
// on constrained internal RAM when WiFi+LVGL are both active.
#define TRANSLATE_TEXT_MAX 192

struct TranslateRequest {
    char chat_key[20];
    char text[TRANSLATE_TEXT_MAX];
};

static TranslateRequest s_queue[TRANSLATE_QUEUE_SIZE];
static int s_queue_head = 0;
static int s_queue_tail = 0;
static uint32_t s_translate_backoff_until_ms = 0;
static uint8_t s_translate_fail_streak = 0;
static bool s_translate_mem_blocked = false;
static uint32_t s_translate_mem_blocked_since_ms = 0;
static bool s_translate_http_fallback = false;
static uint32_t s_translate_http_fallback_until_ms = 0;

#if defined(ESP32)
// On P4 LVGL runs on its own task (see display.cpp::lvgl_task), while
// translate_loop runs on the main Arduino task. Producers can call
// translate_request_to_file from either — LVGL task via long-press /
// load_chat_from_file, main task via on_contact_recv / channel RX.
// A lightweight mutex makes the enqueue/dequeue indices safe across
// tasks without reintroducing the old async-worker architecture. On
// DIS02050A this wasn't needed because LovyanGFX drew LVGL inline and
// everything ran on a single task.
static SemaphoreHandle_t s_q_mux = nullptr;
static inline void q_lock()   { if (s_q_mux) xSemaphoreTake(s_q_mux, portMAX_DELAY); }
static inline void q_unlock() { if (s_q_mux) xSemaphoreGive(s_q_mux); }
#else
static inline void q_lock()   {}
static inline void q_unlock() {}
#endif

// --- URL encoding / JSON helpers (identical to DIS02050A) ----------

static void url_encode_into(String& out, const char* str) {
    while (*str) {
        char c = *str;
        if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += c;
        } else if (c == ' ') {
            out += '+';
        } else {
            char hex[4];
            snprintf(hex, sizeof(hex), "%%%02X", (unsigned char)c);
            out += hex;
        }
        str++;
    }
}

static void append_utf8_codepoint(String& out, uint32_t cp) {
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

static void skip_json_ws_and_commas(const String& body, int& i) {
    while (i < (int)body.length()) {
        char c = body[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',') i++;
        else break;
    }
}

static bool parse_json_string_at(const String& body, int& i, String& out) {
    if (i >= (int)body.length() || body[i] != '"') return false;

    i++;
    out = "";
    while (i < (int)body.length()) {
        char c = body[i++];
        if (c == '"') return true;
        if (c != '\\') {
            out += c;
            continue;
        }
        if (i >= (int)body.length()) return false;

        char esc = body[i++];
        switch (esc) {
            case '"':  out += '"';  break;
            case '\\': out += '\\'; break;
            case '/':  out += '/';  break;
            case 'b':  out += '\b'; break;
            case 'f':  out += '\f'; break;
            case 'n':  out += '\n'; break;
            case 'r':  out += '\r'; break;
            case 't':  out += '\t'; break;
            case 'u': {
                if (i + 4 > (int)body.length()) return false;
                String hex = body.substring(i, i + 4);
                uint32_t cp = (uint32_t)strtoul(hex.c_str(), nullptr, 16);
                i += 4;

                if (cp >= 0xD800 && cp <= 0xDBFF &&
                    i + 6 <= (int)body.length() &&
                    body[i] == '\\' && body[i + 1] == 'u') {
                    String low_hex = body.substring(i + 2, i + 6);
                    uint32_t low = (uint32_t)strtoul(low_hex.c_str(), nullptr, 16);
                    if (low >= 0xDC00 && low <= 0xDFFF) {
                        cp = 0x10000 + (((cp - 0xD800) << 10) | (low - 0xDC00));
                        i += 6;
                    }
                }

                append_utf8_codepoint(out, cp);
                break;
            }
            default:
                out += esc;
                break;
        }
    }
    return false;
}

static bool skip_json_array(const String& body, int& i) {
    if (i >= (int)body.length() || body[i] != '[') return false;

    int depth = 0;
    while (i < (int)body.length()) {
        char c = body[i++];
        if (c == '"') {
            i--;
            String ignored;
            if (!parse_json_string_at(body, i, ignored)) return false;
            continue;
        }
        if (c == '[') depth++;
        else if (c == ']') {
            depth--;
            if (depth == 0) return true;
        }
    }
    return false;
}

static void utf8_strlcpy(char* dst, size_t dst_size, const char* src) {
    if (!dst || dst_size == 0) return;
    dst[0] = '\0';
    if (!src) return;

    size_t w = 0;
    const uint8_t* r = (const uint8_t*)src;
    while (*r && w + 1 < dst_size) {
        uint8_t c = *r;
        if (c < 0x80) {
            dst[w++] = (char)c;
            r++;
            continue;
        }

        int slen = utf8_seq_len(c);
        bool ok = (slen >= 2 && slen <= 4);
        if (ok) {
            for (int j = 0; j < slen; j++) {
                if (!r[j]) { ok = false; break; }
            }
        }
        if (!ok || !utf8_valid_seq(r, slen)) {
            r++;
            continue;
        }
        if (w + (size_t)slen >= dst_size) break;

        for (int j = 0; j < slen; j++) dst[w++] = (char)r[j];
        r += slen;
    }
    dst[w] = '\0';
}

// Google returns [[["part1","orig1",...],["part2","orig2",...]], ...].
// Join all translated segments from the first translation array.
static bool extract_translated_text(const String& body, String& out) {
    int start = body.indexOf("[[[");
    if (start < 0) return false;

    int i = start + 2;
    out = "";

    while (i < (int)body.length()) {
        skip_json_ws_and_commas(body, i);
        if (i >= (int)body.length()) break;
        if (body[i] == ']') break;
        if (body[i] != '[') return false;

        int segment_start = i;
        i++;
        skip_json_ws_and_commas(body, i);

        String segment;
        if (!parse_json_string_at(body, i, segment)) return false;
        out += segment;

        i = segment_start;
        if (!skip_json_array(body, i)) return false;
    }

    return out.length() > 0;
}

// --- HTTP translation ----------------------------------------------

#if defined(ESP32)

static bool do_translate_https(const char* text, const char* target_lang, char* result, int result_size) {
    if (!text || !text[0] || !target_lang || !target_lang[0]) return false;

    WiFiClientSecure client;
    client.setInsecure();  // No cert validation (unofficial endpoint)
    HTTPClient http;
    http.setTimeout(4500);

    String url = "https://translate.googleapis.com/translate_a/single?client=gtx&sl=auto&tl=";
    url += target_lang;
    url += "&dt=t&q=";
    url_encode_into(url, text);

    if (!http.begin(client, url)) {
        Serial.println("Translate: http.begin failed");
        return false;
    }
    http.addHeader("User-Agent", "Mozilla/5.0");

    esp_task_wdt_reset();
    int code = http.GET();

    if (code != 200) {
        Serial.printf("Translate: HTTP %d\n", code);
        http.end();
        return false;
    }

    String body = http.getString();
    http.end();

    String translated;
    if (!extract_translated_text(body, translated)) {
        Serial.println("Translate: parse error");
        return false;
    }

    utf8_strlcpy(result, (size_t)result_size, translated.c_str());
    return true;
}

static bool do_translate_http(const char* text, const char* target_lang, char* result, int result_size) {
    if (!text || !text[0] || !target_lang || !target_lang[0]) return false;

    WiFiClient client;
    HTTPClient http;
    http.setTimeout(3500);

    String url = "http://translate.googleapis.com/translate_a/single?client=gtx&sl=auto&tl=";
    url += target_lang;
    url += "&dt=t&q=";
    url_encode_into(url, text);

    if (!http.begin(client, url)) {
        Serial.println("Translate: http.begin failed (http)");
        return false;
    }
    http.addHeader("User-Agent", "Mozilla/5.0");

    esp_task_wdt_reset();
    int code = http.GET();
    if (code != 200) {
        Serial.printf("Translate: HTTP %d (http)\n", code);
        http.end();
        return false;
    }

    String body = http.getString();
    http.end();

    String translated;
    if (!extract_translated_text(body, translated)) {
        Serial.println("Translate: parse error (http)");
        return false;
    }

    utf8_strlcpy(result, (size_t)result_size, translated.c_str());
    return true;
}

#endif // ESP32

// --- UI live-update ------------------------------------------------
// TRANSLATE_LABEL_TAG comes from translate.h; chat_ui.cpp's disk-restore
// path uses it too so bubble_already_translated() can distinguish a
// real message body from a cached translation label.

// A bubble is a flex-column container; the body text lives in one of
// its labels. Look for a 14pt Montserrat label whose text equals
// source. The 14pt header label is recolored and never matches a
// plain body, so this distinguishes body from header reliably.
static lv_obj_t* find_body_label_in_bubble(lv_obj_t* bubble, const char* source) {
    if (!bubble || !source) return nullptr;
    uint32_t cnt = lv_obj_get_child_cnt(bubble);
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t* child = lv_obj_get_child(bubble, i);
        if (!child) continue;
        // Skip the translation label itself (tagged) and non-labels.
        if (lv_obj_get_user_data(child) == (void*)(uintptr_t)TRANSLATE_LABEL_TAG) continue;
        if (!lv_obj_check_type(child, &lv_label_class)) continue;
        const char* txt = lv_label_get_text(child);
        if (!txt) continue;
        if (strcmp(txt, source) == 0) return child;
    }
    return nullptr;
}

static bool bubble_already_translated(lv_obj_t* bubble) {
    uint32_t cnt = lv_obj_get_child_cnt(bubble);
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t* child = lv_obj_get_child(bubble, i);
        if (child && lv_obj_get_user_data(child) == (void*)(uintptr_t)TRANSLATE_LABEL_TAG)
            return true;
    }
    return false;
}

// Walk ui_chatpanel in reverse and find the newest bubble whose body
// label matches source and that hasn't been translated yet. Returns
// the BUBBLE (not the label) so callers can parent the translation
// child to it. Caller must hold lvgl_lock.
static lv_obj_t* find_bubble_for_source(const char* source) {
    if (!ui_chatpanel || !source) return nullptr;
    uint32_t cnt = lv_obj_get_child_cnt(ui_chatpanel);
    // Iterate in reverse so we attach to the MOST RECENT matching
    // bubble — same ordering as append_translation_to_last_rx() uses
    // on the chat file.
    for (uint32_t i = cnt; i-- > 0; ) {
        lv_obj_t* bubble = lv_obj_get_child(ui_chatpanel, i);
        if (!bubble) continue;
        // Skip outgoing (TX) bubbles. chat_add() sets a non-zero
        // translate_x style only on out=true bubbles to push them
        // right; we don't translate the user's own sent messages,
        // and without this filter a bare-text match ("hello" typed
        // and "hello" received) could land the translation on the
        // wrong side of the chat.
        if (lv_obj_get_style_translate_x(bubble, LV_PART_MAIN) != 0) continue;
        if (bubble_already_translated(bubble)) continue;
        if (find_body_label_in_bubble(bubble, source)) return bubble;
    }
    return nullptr;
}

// Add the translation label to a specific bubble. Caller must hold
// lvgl_lock.
static void attach_translation_label(lv_obj_t* bubble, const char* translated) {
    if (!bubble || !translated || !translated[0]) return;
    if (bubble_already_translated(bubble)) return;

    lv_obj_t* trans_lbl = lv_label_create(bubble);
    lv_obj_set_user_data(trans_lbl, (void*)(uintptr_t)TRANSLATE_LABEL_TAG);
    lv_label_set_long_mode(trans_lbl, LV_LABEL_LONG_WRAP);
    String safeTranslation = sanitize_for_font_string(translated, &lv_font_montserrat_14);
    lv_label_set_text(trans_lbl, safeTranslation.c_str());
    lv_obj_set_width(trans_lbl, lv_pct(100));
    lv_obj_set_style_text_color(trans_lbl, lv_color_hex(TH_TEXT3), 0);
    lv_obj_set_style_text_font(trans_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_pad_top(trans_lbl, 4, 0);
}

// --- Public API ----------------------------------------------------

void translate_init() {
#if defined(ESP32)
    if (!s_q_mux) s_q_mux = xSemaphoreCreateMutex();
#endif
    // Language / g_auto_translate_enabled settings are loaded by
    // persistence.cpp. There's no worker task to spawn — translate_loop
    // runs directly off the main Arduino loop.
}

void translate_request_to_file(const char* text, const char* chat_key) {
    if (!text || !text[0]) return;
    if (!chat_key || !chat_key[0]) return;

    char safe_text[TRANSLATE_TEXT_MAX];
    {
        String cleaned = sanitize_for_font_string(text, &lv_font_montserrat_14);
        // Bound request length before queueing/networking to reduce heap churn.
        if (cleaned.length() >= (TRANSLATE_TEXT_MAX - 1)) {
            cleaned = cleaned.substring(0, TRANSLATE_TEXT_MAX - 1);
        }
        utf8_strlcpy(safe_text, sizeof(safe_text), cleaned.c_str());
    }
    if (!safe_text[0]) return;

    bool queued = false;
    bool full   = false;
    bool dropped_oldest = false;

    q_lock();
    // Dedup + enqueue under one critical section. Producers call from
    // either the LVGL task (long-press, load_chat_from_file Layer C)
    // or the main Arduino task (on_contact_recv, channel RX); without
    // this lock the head/tail indices can tear and the dedup scan can
    // observe an inconsistent snapshot.
    bool duplicate = false;
    for (int i = s_queue_tail; i != s_queue_head; i = (i + 1) % TRANSLATE_QUEUE_SIZE) {
        if (strcmp(s_queue[i].chat_key, chat_key) == 0 &&
            strcmp(s_queue[i].text, safe_text) == 0) {
            duplicate = true;
            break;
        }
    }
    if (!duplicate) {
        int next = (s_queue_head + 1) % TRANSLATE_QUEUE_SIZE;
        if (next == s_queue_tail) {
            // Queue full: drop oldest to keep freshest incoming context.
            s_queue_tail = (s_queue_tail + 1) % TRANSLATE_QUEUE_SIZE;
            dropped_oldest = true;
            full = true;
        }
        TranslateRequest& req = s_queue[s_queue_head];
        strncpy(req.chat_key, chat_key, sizeof(req.chat_key) - 1);
        req.chat_key[sizeof(req.chat_key) - 1] = '\0';
        utf8_strlcpy(req.text, sizeof(req.text), safe_text);
        s_queue_head = (s_queue_head + 1) % TRANSLATE_QUEUE_SIZE;
        queued = true;
    }
    q_unlock();

    if (!queued) return;
    if (dropped_oldest || full) Serial.println("Translate: queue full, dropped oldest");

    static uint32_t s_last_queue_log_ms = 0;
    uint32_t now_q = millis();
    if (now_q - s_last_queue_log_ms > 2000) {
        s_last_queue_log_ms = now_q;
        Serial.printf("Translate: queued '%.40s' -> %s\n",
                      safe_text, translate_lang_code(g_translate_lang_idx));
    }
}

void translate_loop() {
#if defined(ESP32)
    if (!g_wifi_connected || WiFi.status() != WL_CONNECTED) return;

    // Peek the queue first without dequeuing. If empty, we skip the
    // rate-limit + heap-probe work (heap_caps_get_free_size walks a
    // free-block list) — this function runs on every main-loop tick.
    q_lock();
    const bool queue_empty = (s_queue_head == s_queue_tail);
    q_unlock();
    if (queue_empty) return;

    // --- Rate limit / backoff -----------------------------------------
    // Cap translate to at most one TLS handshake per 500 ms. Each
    // handshake needs ~30-40 KB of contiguous DMA-capable internal
    // SRAM for mbedtls AES. Back-to-back handshakes race SDIO RX for
    // the same memory pool and can push us into OOM → the sdio_drv
    // sdio_rx_get_buffer assert that crashed the device when a big
    // chat-open enqueued 15 messages at once. 500 ms gives WiFi stack
    // + SDIO time to refill their buffers between ours.
    static uint32_t s_last_attempt_ms = 0;
    const uint32_t now_ms = millis();
    if ((int32_t)(now_ms - s_translate_backoff_until_ms) < 0) return;
    if (now_ms - s_last_attempt_ms < 500) return;

    // --- Memory guard -------------------------------------------------
    // Skip this tick if DMA-capable internal SRAM is too low to safely
    // run a TLS handshake. 48 KB covers the common mbedtls peak with
    // headroom so we don't try-and-fail (which fragments the heap).
    const size_t dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA);
    const size_t int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    const size_t int_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const bool mem_too_low =
        (dma_free < 48 * 1024 || int_free < 72 * 1024 ||
         dma_largest < 28 * 1024 || int_largest < 28 * 1024);
    if (mem_too_low) {
        static uint32_t s_last_warn_ms = 0;
        if (s_translate_mem_blocked_since_ms == 0) s_translate_mem_blocked_since_ms = now_ms;
        if (now_ms - s_last_warn_ms > 15000) {
            Serial.printf("Translate: low mem skip dma=%u int=%u dma_lg=%u int_lg=%u\n",
                          (unsigned)dma_free, (unsigned)int_free,
                          (unsigned)dma_largest, (unsigned)int_largest);
            s_last_warn_ms = now_ms;
        }
        s_translate_mem_blocked = true;
        if ((now_ms - s_translate_mem_blocked_since_ms) > 60000UL) {
            s_translate_http_fallback = true;
            s_translate_http_fallback_until_ms = now_ms + 120000UL;
        }
        // Brief cooling window to avoid tight-loop probing when memory is low.
        s_translate_backoff_until_ms = now_ms + 1500;
        return;
    }
    if (s_translate_mem_blocked) {
        Serial.printf("Translate: mem recovered dma=%u int=%u dma_lg=%u int_lg=%u\n",
                      (unsigned)dma_free, (unsigned)int_free,
                      (unsigned)dma_largest, (unsigned)int_largest);
        s_translate_mem_blocked = false;
        s_translate_mem_blocked_since_ms = 0;
        s_translate_http_fallback = false;
        s_translate_http_fallback_until_ms = 0;
    }

    // Now actually dequeue — we know we can proceed.
    TranslateRequest req;
    bool have = false;
    q_lock();
    if (s_queue_head != s_queue_tail) {
        req = s_queue[s_queue_tail];
        s_queue_tail = (s_queue_tail + 1) % TRANSLATE_QUEUE_SIZE;
        have = true;
    }
    q_unlock();
    if (!have) return;  // Race: enqueue drained between peek and dequeue.

    s_last_attempt_ms = now_ms;  // We committed — start the rate-limit timer.

    const char* lang = translate_lang_code(g_translate_lang_idx);

    char result[TRANSLATE_TEXT_MAX];
    esp_task_wdt_reset();
    if (s_translate_http_fallback &&
        (int32_t)(now_ms - s_translate_http_fallback_until_ms) >= 0) {
        s_translate_http_fallback = false;
        s_translate_http_fallback_until_ms = 0;
    }
    const bool ok = s_translate_http_fallback
                      ? do_translate_http(req.text, lang, result, sizeof(result))
                      : do_translate_https(req.text, lang, result, sizeof(result));
    if (!ok) {
        // Exponential-ish backoff to reduce TLS/SDIO pressure on repeated fails.
        if (s_translate_fail_streak < 6) s_translate_fail_streak++;
        uint32_t backoff_ms = 1000u << (s_translate_fail_streak - 1);  // 1s..32s
        if (backoff_ms > 12000u) backoff_ms = 12000u;
        s_translate_backoff_until_ms = now_ms + backoff_ms;
        if (!s_translate_http_fallback && s_translate_fail_streak >= 3) {
            s_translate_http_fallback = true;
            s_translate_http_fallback_until_ms = now_ms + 120000UL;
        }
        Serial.printf("Translate: fail streak=%u backoff=%lums mode=%s\n",
                      (unsigned)s_translate_fail_streak,
                      (unsigned long)backoff_ms,
                      s_translate_http_fallback ? "http" : "https");
        return;
    }
    s_translate_fail_streak = 0;
    s_translate_backoff_until_ms = 0;

    // Google returns the source verbatim for no-op translations (e.g.
    // the user's target language matches the already-typed language).
    // Skip persistence in that case — there's nothing to show.
    if (strcmp(result, req.text) == 0) return;

    // Always persist to disk, keyed by body-text match so that the
    // translation lands on the correct RX line even when multiple
    // untranslated messages are pending.
    append_translation_to_last_rx(String(req.chat_key), req.text, result);

    // If the user is currently viewing this chat, update the visible
    // bubble in place so the translation appears without needing a
    // chat reload. Take lvgl_lock FIRST, then check g_current_chat_key
    // under the lock — every writer to that key (load_chat_from_file,
    // exit_chat_mode, delete-confirm) runs from LVGL event callbacks
    // under lvgl_lock, so this check is race-free.
    if (lvgl_lock(100)) {
        if (strcmp(g_current_chat_key, req.chat_key) == 0) {
            lv_obj_t* bubble = find_bubble_for_source(req.text);
            if (bubble) attach_translation_label(bubble, result);
        }
        lvgl_unlock();
    }
#endif
}
