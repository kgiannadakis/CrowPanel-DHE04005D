// translate.cpp - Google Translate via free unofficial API
// NOTE: Uses the unofficial translate.googleapis.com endpoint (no API key).
//       TLS certificate validation is disabled (setInsecure).
//       This is NOT the official Google Cloud Translation API.
// Queues requests, drains one per loop iteration.

#include "translate.h"
#include "app_globals.h"
#include "ui_theme.h"
#include "utils.h"
#include "persistence.h"

#include <string.h>

#if defined(ESP32)
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_task_wdt.h>
#endif

extern bool g_wifi_connected;

// Language table
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

// Request queue

#define TRANSLATE_QUEUE_SIZE 6

struct TranslateRequest {
    char text[384];
    char chat_key[20];   // if non-empty, append translation to this chat file
    lv_obj_t* bubble;    // if non-null, also show in this live bubble
};

static TranslateRequest s_queue[TRANSLATE_QUEUE_SIZE];
static int s_queue_head = 0;
static int s_queue_tail = 0;

// URL encoding

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

// JSON parsing helpers

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

// HTTP translation

#if defined(ESP32)

static bool do_translate(const char* text, const char* target_lang, char* result, int result_size) {
    if (!text || !text[0] || !target_lang || !target_lang[0]) return false;

    WiFiClientSecure client;
    client.setInsecure();  // No cert validation (unofficial endpoint)
    HTTPClient http;
    http.setTimeout(8000);

    String url = "https://translate.googleapis.com/translate_a/single?client=gtx&sl=auto&tl=";
    url += target_lang;
    url += "&dt=t&q=";
    url_encode_into(url, text);

    if (!http.begin(client, url)) {
        serialmon_append("Translate: http.begin failed");
        return false;
    }
    http.addHeader("User-Agent", "Mozilla/5.0");

    esp_task_wdt_reset();
    int code = http.GET();

    if (code != 200) {
        char logbuf[64];
        snprintf(logbuf, sizeof(logbuf), "Translate: HTTP %d", code);
        serialmon_append(logbuf);
        http.end();
        return false;
    }

    String body = http.getString();
    http.end();

    String translated;
    if (!extract_translated_text(body, translated)) {
        serialmon_append("Translate: parse error");
        return false;
    }

    utf8_strlcpy(result, (size_t)result_size, translated.c_str());
    return true;
}

#endif // ESP32

// Show translation in bubble

#define TRANSLATE_LABEL_TAG 0xBEEF

static bool bubble_already_translated(lv_obj_t* bubble) {
    uint32_t cnt = lv_obj_get_child_cnt(bubble);
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t* child = lv_obj_get_child(bubble, i);
        if (child && lv_obj_get_user_data(child) == (void*)(uintptr_t)TRANSLATE_LABEL_TAG)
            return true;
    }
    return false;
}

static void show_translation_in_bubble(lv_obj_t* bubble, const char* translated) {
    if (!bubble || !translated || !translated[0]) return;

    lv_obj_t* parent = lv_obj_get_parent(bubble);
    if (!parent) return;

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

// Public API

void translate_init() {
    // Settings loaded by persistence.cpp
}

void translate_invalidate_bubbles() {
    // Called when chat panel is cleared; drop all pending requests.
    s_queue_head = s_queue_tail = 0;
}

void translate_request(const char* text, lv_obj_t* bubble) {
    if (!text || !text[0] || !bubble) {
        serialmon_append("Translate: request rejected (null)");
        return;
    }
    if (!g_wifi_connected) {
        serialmon_append("Translate: request rejected (no wifi)");
        return;
    }

    if (bubble_already_translated(bubble)) return;

    for (int i = s_queue_tail; i != s_queue_head; i = (i + 1) % TRANSLATE_QUEUE_SIZE) {
        if (s_queue[i].bubble == bubble) return;
    }

    int next = (s_queue_head + 1) % TRANSLATE_QUEUE_SIZE;
    if (next == s_queue_tail) {
        serialmon_append("Translate: queue full");
        return;
    }

    TranslateRequest& req = s_queue[s_queue_head];
    utf8_strlcpy(req.text, sizeof(req.text), text);
    req.chat_key[0] = '\0';
    req.bubble = bubble;
    s_queue_head = next;

    char logbuf[80];
    snprintf(logbuf, sizeof(logbuf), "Translate: queued '%.40s' -> %s", text, translate_lang_code(g_translate_lang_idx));
    serialmon_append(logbuf);
}

void translate_request_to_file(const char* text, const char* chat_key, lv_obj_t* bubble) {
    if (!text || !text[0] || !chat_key || !chat_key[0]) return;
    if (!g_wifi_connected) return;

    int next = (s_queue_head + 1) % TRANSLATE_QUEUE_SIZE;
    if (next == s_queue_tail) return;  // queue full

    TranslateRequest& req = s_queue[s_queue_head];
    utf8_strlcpy(req.text, sizeof(req.text), text);
    strncpy(req.chat_key, chat_key, sizeof(req.chat_key) - 1);
    req.chat_key[sizeof(req.chat_key) - 1] = '\0';
    req.bubble = bubble;  // can be null if not viewing chat
    s_queue_head = next;

    char logbuf[80];
    snprintf(logbuf, sizeof(logbuf), "Translate(file): queued '%.30s' -> %s", text, translate_lang_code(g_translate_lang_idx));
    serialmon_append(logbuf);
}

void translate_loop() {
#if defined(ESP32)
    if (!g_wifi_connected) return;
    if (s_queue_head == s_queue_tail) return;

    TranslateRequest& req = s_queue[s_queue_tail];
    s_queue_tail = (s_queue_tail + 1) % TRANSLATE_QUEUE_SIZE;

    const char* lang = translate_lang_code(g_translate_lang_idx);

    char result[384];
    esp_task_wdt_reset();
    if (do_translate(req.text, lang, result, sizeof(result))) {
        if (strcmp(result, req.text) != 0) {
            // Show in live bubble if available
            if (req.bubble) {
                show_translation_in_bubble(req.bubble, result);
            }
            // Append translation inline to the last RX line in the chat file
            if (req.chat_key[0]) {
                append_translation_to_last_rx(String(req.chat_key), result);
            }
        }
    }
#endif
}
