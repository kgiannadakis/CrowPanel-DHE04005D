// telegram_bridge.cpp — Bidirectional Telegram Bot API bridge
// Outbound: queued ring buffer, drained 1/loop
// Inbound: short-poll getUpdates every 7 seconds
// Channels → group chat with forum topics (one topic per channel)
// PMs      → private 1-on-1 chat with the bot owner

#include "app_globals.h"
#include "telegram_bridge.h"
#include "features_ui.h"
#include "mesh_api.h"
#include "utils.h"

#if defined(ESP32)
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_task_wdt.h>
#endif

extern bool g_wifi_connected;

// ── Settings ────────────────────────────────────────────────

static char s_token[72] = "";
static char s_chat_id[24] = "";       // group chat ID (for channels)
static char s_pm_chat_id[24] = "";    // private chat ID (for PMs, auto-detected)
static char s_status[64] = "Disabled";

// ── Forum topic cache (channels only) ──────────────────────
// Maps channel names to Telegram thread IDs in the group.

#define MAX_TOPICS 16
struct TopicEntry {
    char name[32];        // "#channelname"
    int32_t thread_id;    // Telegram message_thread_id
};
static TopicEntry s_topics[MAX_TOPICS];
static int s_topic_count = 0;
static bool s_forum_supported = true;

static int32_t topic_lookup(const char* name) {
    for (int i = 0; i < s_topic_count; i++) {
        if (strcasecmp(s_topics[i].name, name) == 0)
            return s_topics[i].thread_id;
    }
    return 0;
}

static void topic_store(const char* name, int32_t thread_id) {
    for (int i = 0; i < s_topic_count; i++) {
        if (strcasecmp(s_topics[i].name, name) == 0) {
            s_topics[i].thread_id = thread_id;
            return;
        }
    }
    if (s_topic_count < MAX_TOPICS) {
        strncpy(s_topics[s_topic_count].name, name, 31);
        s_topics[s_topic_count].name[31] = '\0';
        s_topics[s_topic_count].thread_id = thread_id;
        s_topic_count++;
    }
}

static void topics_save_nvs() {
#if defined(ESP32)
    Preferences prefs;
    prefs.begin("tgtopics", false);
    prefs.putInt("count", s_topic_count);
    for (int i = 0; i < s_topic_count; i++) {
        char kn[8], kt[8];
        snprintf(kn, sizeof(kn), "n%d", i);
        snprintf(kt, sizeof(kt), "t%d", i);
        prefs.putString(kn, s_topics[i].name);
        prefs.putInt(kt, s_topics[i].thread_id);
    }
    prefs.end();
#endif
}

static void topics_load_nvs() {
#if defined(ESP32)
    Preferences prefs;
    prefs.begin("tgtopics", true);
    s_topic_count = prefs.getInt("count", 0);
    if (s_topic_count > MAX_TOPICS) s_topic_count = MAX_TOPICS;
    for (int i = 0; i < s_topic_count; i++) {
        char kn[8], kt[8];
        snprintf(kn, sizeof(kn), "n%d", i);
        snprintf(kt, sizeof(kt), "t%d", i);
        String n = prefs.getString(kn, "");
        strncpy(s_topics[i].name, n.c_str(), 31);
        s_topics[i].name[31] = '\0';
        s_topics[i].thread_id = prefs.getInt(kt, 0);
    }
    prefs.end();
#endif
}

// ── Outbound queue ──────────────────────────────────────────

#define TG_QUEUE_SIZE 8
struct TgOutbound {
    char text[300];
    char topic_key[32];   // topic key for channels, empty for PMs
    bool is_pm;           // true = send to private chat, false = send to group
};
static TgOutbound s_queue[TG_QUEUE_SIZE];
static int s_queue_head = 0;
static int s_queue_tail = 0;

static void queue_push(const char* text, const char* topic_key, bool is_pm) {
    int next = (s_queue_head + 1) % TG_QUEUE_SIZE;
    if (next == s_queue_tail) return;
    strncpy(s_queue[s_queue_head].text, text, 299);
    s_queue[s_queue_head].text[299] = '\0';
    strncpy(s_queue[s_queue_head].topic_key, topic_key ? topic_key : "", 31);
    s_queue[s_queue_head].topic_key[31] = '\0';
    s_queue[s_queue_head].is_pm = is_pm;
    s_queue_head = next;
}

static bool queue_empty() { return s_queue_head == s_queue_tail; }

// ── Inbound state ───────────────────────────────────────────

static int32_t s_update_offset = 0;
static uint32_t s_last_poll_ms = 0;
static const uint32_t POLL_INTERVAL_MS = 7000;

// ── HTML escape helper ──────────────────────────────────────

static void html_escape_into(String& out, const char* text, int max_len) {
    for (int i = 0; text[i] && i < max_len; i++) {
        char c = text[i];
        if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '&') out += "&amp;";
        else out += c;
    }
}

// ── JSON body builder helper ────────────────────────────────

static void json_append_escaped(String& body, const char* text, int max_len) {
    for (int i = 0; text[i] && i < max_len; i++) {
        char c = text[i];
        if (c == '"') body += "\\\"";
        else if (c == '\\') body += "\\\\";
        else if (c == '\n') body += "\\n";
        else body += c;
    }
}

// ── HTTP helpers ────────────────────────────────────────────

#if defined(ESP32)

static int32_t tg_create_forum_topic(const char* topic_name) {
    if (!s_token[0] || !s_chat_id[0] || !s_forum_supported) return 0;

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setTimeout(8000);

    char url[200];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/createForumTopic", s_token);

    if (!http.begin(client, url)) return 0;
    http.addHeader("Content-Type", "application/json");

    String body = "{\"chat_id\":\"";
    body += s_chat_id;
    body += "\",\"name\":\"";
    json_append_escaped(body, topic_name, 60);
    body += "\"}";

    int code = http.POST(body);
    String resp = http.getString();
    http.end();

    if (code != 200) {
        char logbuf[120];
        snprintf(logbuf, sizeof(logbuf), "TG: createForumTopic failed HTTP %d: %.60s", code, resp.c_str());
        serialmon_append(logbuf);
        if (resp.indexOf("PEER_ID_NOT_MODIFIED") >= 0 || resp.indexOf("not a forum") >= 0) {
            s_forum_supported = false;
            serialmon_append("TG: group is not a forum, threads disabled");
        }
        return 0;
    }

    int tid_pos = resp.indexOf("\"message_thread_id\"");
    if (tid_pos < 0) return 0;
    int colon = resp.indexOf(':', tid_pos);
    if (colon < 0) return 0;
    int32_t tid = (int32_t)resp.substring(colon + 1, colon + 15).toInt();

    char logbuf[80];
    snprintf(logbuf, sizeof(logbuf), "TG: created topic '%s' id=%ld", topic_name, (long)tid);
    serialmon_append(logbuf);
    return tid;
}

static int32_t tg_get_or_create_topic(const char* topic_key) {
    if (!s_forum_supported) return 0;

    int32_t tid = topic_lookup(topic_key);
    if (tid != 0) return tid;

    esp_task_wdt_reset();
    tid = tg_create_forum_topic(topic_key);
    if (tid != 0) {
        topic_store(topic_key, tid);
        topics_save_nvs();
    }
    return tid;
}

// Send a message to either the group (with optional topic) or the private PM chat
static bool tg_send_message(const char* text, const char* topic_key, bool to_private) {
    if (!s_token[0]) {
        serialmon_append("TG send: no token");
        return false;
    }

    // Choose destination chat
    const char* dest_chat = to_private ? s_pm_chat_id : s_chat_id;
    if (!dest_chat[0]) {
        if (to_private)
            serialmon_append("TG send: no PM chat ID (send /start to bot in private)");
        else
            serialmon_append("TG send: no group chat ID");
        return false;
    }

    // Resolve forum topic (only for group messages)
    int32_t thread_id = 0;
    if (!to_private && topic_key && topic_key[0]) {
        thread_id = tg_get_or_create_topic(topic_key);
    }

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setTimeout(8000);

    char url[200];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", s_token);

    if (!http.begin(client, url)) {
        serialmon_append("TG send: http.begin failed");
        return false;
    }
    http.addHeader("Content-Type", "application/json");

    String body = "{\"chat_id\":\"";
    body += dest_chat;
    body += "\",\"parse_mode\":\"HTML\"";
    if (thread_id != 0) {
        body += ",\"message_thread_id\":";
        body += String(thread_id);
    }
    body += ",\"text\":\"";
    json_append_escaped(body, text, 280);
    body += "\"}";

    int code = http.POST(body);
    http.end();

    if (code != 200) {
        char logbuf[80];
        snprintf(logbuf, sizeof(logbuf), "TG send: HTTP %d (dest=%s)", code, to_private ? "pm" : "group");
        serialmon_append(logbuf);

        if (thread_id != 0 && code == 400) {
            topic_store(topic_key, 0);
        }
    }

    return (code == 200);
}

// Save the private PM chat ID to NVS
static void save_pm_chat_id() {
#if defined(ESP32)
    Preferences prefs;
    prefs.begin("tgbridge", false);
    prefs.putString("pmchatid", s_pm_chat_id);
    prefs.end();
#endif
}

static void tg_poll_incoming() {
    if (!s_token[0]) return;
    // Need at least one chat ID configured to poll
    if (!s_chat_id[0] && !s_pm_chat_id[0]) return;

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setTimeout(5000);

    char url[256];
    snprintf(url, sizeof(url),
        "https://api.telegram.org/bot%s/getUpdates?offset=%ld&timeout=0&limit=5",
        s_token, (long)s_update_offset);

    if (!http.begin(client, url)) {
        serialmon_append("TG poll: http.begin failed");
        return;
    }
    int code = http.GET();

    if (code != 200) {
        char logbuf[64];
        snprintf(logbuf, sizeof(logbuf), "TG poll: HTTP %d", code);
        serialmon_append(logbuf);
        http.end();
        return;
    }

    String body = http.getString();
    http.end();

    int pos = 0;
    while (true) {
        int uid_pos = body.indexOf("\"update_id\"", pos);
        if (uid_pos < 0) break;

        int colon = body.indexOf(':', uid_pos + 11);
        if (colon < 0) break;
        int32_t uid = (int32_t)body.substring(colon + 1, colon + 15).toInt();
        if (uid >= s_update_offset) s_update_offset = uid + 1;

        int next_uid = body.indexOf("\"update_id\"", uid_pos + 11);
        int block_end = (next_uid > 0) ? next_uid : body.length();

        // Detect chat type: look for "type":"private" within this update
        int chat_pos = body.indexOf("\"chat\"", uid_pos);
        bool is_private = false;
        if (chat_pos >= 0 && chat_pos < block_end) {
            int type_pos = body.indexOf("\"type\"", chat_pos);
            if (type_pos >= 0 && type_pos < block_end) {
                is_private = (body.indexOf("\"private\"", type_pos) >= 0 &&
                              body.indexOf("\"private\"", type_pos) < block_end);
            }
        }

        // Auto-detect PM chat ID from private messages
        if (is_private && chat_pos >= 0) {
            int id_pos = body.indexOf("\"id\"", chat_pos);
            if (id_pos >= 0 && id_pos < block_end) {
                int id_colon = body.indexOf(':', id_pos + 4);
                if (id_colon >= 0) {
                    // Extract the numeric ID
                    int id_start = id_colon + 1;
                    while (id_start < block_end && body[id_start] == ' ') id_start++;
                    int id_end = id_start;
                    while (id_end < block_end && (body[id_end] == '-' || (body[id_end] >= '0' && body[id_end] <= '9'))) id_end++;
                    String new_pm_id = body.substring(id_start, id_end);
                    if (new_pm_id.length() > 0 && strcmp(new_pm_id.c_str(), s_pm_chat_id) != 0) {
                        strncpy(s_pm_chat_id, new_pm_id.c_str(), sizeof(s_pm_chat_id) - 1);
                        s_pm_chat_id[sizeof(s_pm_chat_id) - 1] = '\0';
                        save_pm_chat_id();
                        char logbuf[80];
                        snprintf(logbuf, sizeof(logbuf), "TG: PM chat ID set: %s", s_pm_chat_id);
                        serialmon_append(logbuf);
                    }
                }
            }
        }

        // Find "text" field
        int text_pos = body.indexOf("\"text\"", uid_pos);
        if (text_pos < 0 || text_pos >= block_end) {
            pos = block_end;
            continue;
        }

        int t_start = body.indexOf('"', text_pos + 6);
        int t_end = body.indexOf('"', t_start + 1);
        if (t_start < 0 || t_end < 0) {
            pos = block_end;
            continue;
        }

        String msg_text = body.substring(t_start + 1, t_end);
        msg_text.replace("\\n", "\n");
        msg_text.replace("\\\"", "\"");
        msg_text.replace("\\\\", "\\");

        if (msg_text.length() == 0 || msg_text.length() > 240) {
            pos = block_end;
            continue;
        }

        char logbuf[128];

        // Handle /start — just a greeting, confirm PM link
        String cmd = msg_text;
        cmd.toLowerCase();

        if (is_private && cmd == "/start") {
            // Send confirmation back to the private chat
            const char* confirm = "MeshCore bridge linked. Your PMs will appear here.";
            tg_send_message(confirm, "", true);
            pos = block_end;
            continue;
        }

        snprintf(logbuf, sizeof(logbuf), "TG RX [%s]: %.90s",
                 is_private ? "PM" : "group", msg_text.c_str());
        serialmon_append(logbuf);

        // Route commands (accepted from both private and group chat)
        if (cmd.startsWith("/pm ")) {
            String rest = msg_text.substring(4);
            int sp = rest.indexOf(' ');
            if (sp > 0) {
                String name = rest.substring(0, sp);
                String text_part = rest.substring(sp + 1);
                bool found = false;
                for (int i = 0; i < dd_contacts_count; i++) {
                    if (name.equalsIgnoreCase(dd_contacts[i].name)) {
                        mesh_send_text_to_contact(dd_contacts[i].contact_id.pub_key, text_part.c_str());
                        snprintf(logbuf, sizeof(logbuf), "TG->PM %s: %.80s", dd_contacts[i].name, text_part.c_str());
                        serialmon_append(logbuf);
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    snprintf(logbuf, sizeof(logbuf), "TG: contact '%.20s' not found", name.c_str());
                    serialmon_append(logbuf);
                }
            } else {
                serialmon_append("TG: usage: /pm ContactName message");
            }
        } else if (cmd.startsWith("/ch ")) {
            String rest = msg_text.substring(4);
            int sp = rest.indexOf(' ');
            if (sp > 0) {
                String name = rest.substring(0, sp);
                String text_part = rest.substring(sp + 1);
                bool found = false;
                for (int i = 0; i < dd_channels_count; i++) {
                    if (name.equalsIgnoreCase(dd_channels[i].name)) {
                        mesh_send_text_to_channel(dd_channels[i].channel_idx, text_part.c_str());
                        snprintf(logbuf, sizeof(logbuf), "TG->CH #%s: %.80s", dd_channels[i].name, text_part.c_str());
                        serialmon_append(logbuf);
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    snprintf(logbuf, sizeof(logbuf), "TG: channel '%.20s' not found", name.c_str());
                    serialmon_append(logbuf);
                }
            } else {
                serialmon_append("TG: usage: /ch ChannelName message");
            }
        } else {
            snprintf(logbuf, sizeof(logbuf), "TG: ignored (use /pm or /ch): %.60s", msg_text.c_str());
            serialmon_append(logbuf);
        }

        pos = block_end;
    }
}

#endif // ESP32

// ── Public API ──────────────────────────────────────────────

void tgbridge_init() {
#if defined(ESP32)
    Preferences prefs;
    prefs.begin("tgbridge", true);
    g_tgbridge_enabled = prefs.getBool("enabled", false);
    String tok = prefs.getString("token", "");
    String cid = prefs.getString("chatid", "");
    String pmcid = prefs.getString("pmchatid", "");
    strncpy(s_token, tok.c_str(), sizeof(s_token) - 1);
    strncpy(s_chat_id, cid.c_str(), sizeof(s_chat_id) - 1);
    strncpy(s_pm_chat_id, pmcid.c_str(), sizeof(s_pm_chat_id) - 1);
    prefs.end();
    topics_load_nvs();
#endif
}

void tgbridge_save_settings() {
#if defined(ESP32)
    Preferences prefs;
    prefs.begin("tgbridge", false);
    prefs.putBool("enabled", g_tgbridge_enabled);
    prefs.putString("token", s_token);
    prefs.putString("chatid", s_chat_id);
    prefs.end();
#endif
}

static void trim_trailing(char* s) {
    int len = strlen(s);
    while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\n' || s[len-1] == '\r' || s[len-1] == '\t')) {
        s[--len] = '\0';
    }
}

void tgbridge_set_token(const char* token) {
    strncpy(s_token, token ? token : "", sizeof(s_token) - 1);
    s_token[sizeof(s_token) - 1] = '\0';
    trim_trailing(s_token);
    char logbuf[80];
    snprintf(logbuf, sizeof(logbuf), "TG token set: len=%d first=%.6s", (int)strlen(s_token), s_token);
    serialmon_append(logbuf);
    tgbridge_save_settings();
}

void tgbridge_set_chat_id(const char* chat_id) {
    strncpy(s_chat_id, chat_id ? chat_id : "", sizeof(s_chat_id) - 1);
    s_chat_id[sizeof(s_chat_id) - 1] = '\0';
    trim_trailing(s_chat_id);
    char logbuf[80];
    snprintf(logbuf, sizeof(logbuf), "TG chat ID set: [%s]", s_chat_id);
    serialmon_append(logbuf);
    tgbridge_save_settings();
}

void tgbridge_populate_ui() {
    if (ui_tg_token_ta && s_token[0]) {
        lv_textarea_set_text(ui_tg_token_ta, s_token);
    }
    if (ui_tg_chatid_ta && s_chat_id[0]) {
        lv_textarea_set_text(ui_tg_chatid_ta, s_chat_id);
    }
}

void tgbridge_forward_pm(const char* sender, const char* remote_name, const char* text, bool is_outgoing) {
    if (!g_tgbridge_enabled || !text || !text[0]) return;

    // Format: bold sender name, then message on next line
    String html = "<b>";
    html_escape_into(html, sender ? sender : "?", 30);
    html += "</b>\n";
    html_escape_into(html, text, 240);

    char buf[300];
    strncpy(buf, html.c_str(), 299);
    buf[299] = '\0';
    // PMs go to private chat, no topic needed
    queue_push(buf, "", true);
}

void tgbridge_forward_channel(const char* channel_name, const char* sender, const char* text) {
    if (!g_tgbridge_enabled || !text || !text[0]) return;

    String html = "<b>";
    html_escape_into(html, sender ? sender : "?", 30);
    html += "</b>\n";
    html_escape_into(html, text, 240);

    char topic_key[32];
    snprintf(topic_key, sizeof(topic_key), "#%.30s", channel_name ? channel_name : "?");

    char buf[300];
    strncpy(buf, html.c_str(), 299);
    buf[299] = '\0';
    // Channels go to group with topic
    queue_push(buf, topic_key, false);
}

void tgbridge_loop() {
#if defined(ESP32)
    if (!g_tgbridge_enabled || !g_wifi_connected) return;

    static bool s_logged_active = false;
    if (!s_logged_active) {
        char logbuf[128];
        snprintf(logbuf, sizeof(logbuf), "TG bridge active: group=%s pm=%s",
                 s_chat_id[0] ? s_chat_id : "(none)",
                 s_pm_chat_id[0] ? s_pm_chat_id : "(none, send /start to bot)");
        serialmon_append(logbuf);
        s_logged_active = true;
    }

    // Drain one outbound message per loop
    if (!queue_empty()) {
        esp_task_wdt_reset();
        TgOutbound& item = s_queue[s_queue_tail];
        bool ok = tg_send_message(item.text, item.topic_key, item.is_pm);
        s_queue_tail = (s_queue_tail + 1) % TG_QUEUE_SIZE;
        if (!ok) {
            snprintf(s_status, sizeof(s_status), "Send failed");
            g_deferred_features_dirty = true;
        }
    }

    // Poll for incoming messages
    if ((millis() - s_last_poll_ms) >= POLL_INTERVAL_MS) {
        s_last_poll_ms = millis();
        esp_task_wdt_reset();
        tg_poll_incoming();
    }
#endif
}

const char* tgbridge_status_text() {
    if (!g_tgbridge_enabled) return "Disabled";
#if defined(ESP32)
    if (!g_wifi_connected) return "WiFi required";
    if (!s_token[0]) return "Set bot token";
    if (!s_chat_id[0]) return "Set group chat ID";
    if (!s_pm_chat_id[0]) return "Active (send /start to bot for PMs)";
    return "Active";
#else
    return "Not supported";
#endif
}
