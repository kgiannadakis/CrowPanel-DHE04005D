// ota_update.cpp — OTA firmware update from GitHub Releases (non-blocking chunked)

#include "app_globals.h"
#include "ota_update.h"
#include "features_ui.h"
#include "utils.h"

#if defined(ESP32)
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <esp_task_wdt.h>
#endif

extern bool g_wifi_connected;

// ── State ───────────────────────────────────────────────────

static char s_repo[128] = "";
static char s_status[80] = "";
static bool s_checking = false;

enum OtaState { OTA_IDLE, OTA_DOWNLOADING, OTA_DONE, OTA_FAILED };
static OtaState s_state = OTA_IDLE;
static uint8_t  s_progress = 0;
static int      s_total_size = 0;
static int      s_written = 0;

#if defined(ESP32)
static WiFiClientSecure* s_client = nullptr;
static HTTPClient*       s_http = nullptr;
static WiFiClient*       s_stream = nullptr;
#endif

// ── NVS ─────────────────────────────────────────────────────

void ota_init() {
#if defined(ESP32)
    Preferences prefs;
    prefs.begin("ota", true);
    String repo = prefs.getString("repo", "kgiannadakis/CrowPanel-DIS02050A");
    strncpy(s_repo, repo.c_str(), sizeof(s_repo) - 1);
    prefs.end();
#endif
}

void ota_save_settings() {
#if defined(ESP32)
    Preferences prefs;
    prefs.begin("ota", false);
    prefs.putString("repo", s_repo);
    prefs.end();
#endif
}

void ota_set_repo(const char* repo) {
    strncpy(s_repo, repo ? repo : "", sizeof(s_repo) - 1);
    s_repo[sizeof(s_repo) - 1] = '\0';
    ota_save_settings();
}

void ota_populate_ui() {
    if (ui_ota_repo_ta && s_repo[0]) {
        lv_textarea_set_text(ui_ota_repo_ta, s_repo);
    }
}

// ── Status accessors ────────────────────────────────────────

bool ota_is_checking()       { return s_checking; }
bool ota_is_updating()       { return s_state == OTA_DOWNLOADING; }
uint8_t ota_progress_percent() { return s_progress; }
const char* ota_status_text() { return s_status; }

// ── Cleanup ─────────────────────────────────────────────────

#if defined(ESP32)
static void ota_cleanup_http() {
    if (s_http) { s_http->end(); delete s_http; s_http = nullptr; }
    if (s_client) { delete s_client; s_client = nullptr; }
    s_stream = nullptr;
}
#endif

// ── Check for update (called from button press) ─────────────

void ota_check_for_update() {
#if defined(ESP32)
    if (!g_wifi_connected) {
        snprintf(s_status, sizeof(s_status), "WiFi required");
        g_deferred_features_dirty = true;
        return;
    }
    if (s_checking || s_state == OTA_DOWNLOADING) return;
    if (!s_repo[0]) {
        snprintf(s_status, sizeof(s_status), "Set GitHub repo first");
        g_deferred_features_dirty = true;
        return;
    }

    s_checking = true;
    snprintf(s_status, sizeof(s_status), "Checking...");
    g_deferred_features_dirty = true;

    // Build API URL
    char url[256];
    snprintf(url, sizeof(url), "https://api.github.com/repos/%s/releases/latest", s_repo);

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(10000);
    http.setUserAgent("ESP32-OTA/1.0");

    esp_task_wdt_reset();

    if (!http.begin(client, url)) {
        snprintf(s_status, sizeof(s_status), "Failed to connect");
        s_checking = false;
        g_deferred_features_dirty = true;
        return;
    }

    int code = http.GET();
    esp_task_wdt_reset();

    if (code != 200) {
        snprintf(s_status, sizeof(s_status), "HTTP %d", code);
        http.end();
        s_checking = false;
        g_deferred_features_dirty = true;
        return;
    }

    String body = http.getString();
    http.end();
    esp_task_wdt_reset();

    // Parse tag_name from JSON
    int tag_pos = body.indexOf("\"tag_name\"");
    if (tag_pos < 0) {
        snprintf(s_status, sizeof(s_status), "No release found");
        s_checking = false;
        g_deferred_features_dirty = true;
        return;
    }

    // Extract tag value: find the next quoted string
    int tag_start = body.indexOf('"', tag_pos + 10);
    int tag_end = body.indexOf('"', tag_start + 1);
    if (tag_start < 0 || tag_end < 0 || (tag_end - tag_start) > 24) {
        snprintf(s_status, sizeof(s_status), "Parse error");
        s_checking = false;
        g_deferred_features_dirty = true;
        return;
    }

    String tag = body.substring(tag_start + 1, tag_end);
    // Strip leading 'v' if present
    const char* remote_ver = tag.c_str();
    if (remote_ver[0] == 'v' || remote_ver[0] == 'V') remote_ver++;

    char logbuf[128];
    snprintf(logbuf, sizeof(logbuf), "OTA: local=%s remote=%s", FIRMWARE_VERSION, remote_ver);
    serialmon_append(logbuf);

    if (strcmp(remote_ver, FIRMWARE_VERSION) == 0) {
        snprintf(s_status, sizeof(s_status), "Up to date (v%s)", FIRMWARE_VERSION);
        s_checking = false;
        g_deferred_features_dirty = true;
        return;
    }

    // Find firmware.bin download URL in browser_download_url
    int dl_pos = body.indexOf("\"browser_download_url\"");
    // Find one that ends with .bin
    char download_url[256] = "";
    while (dl_pos >= 0) {
        int url_start = body.indexOf('"', dl_pos + 21);
        int url_end = body.indexOf('"', url_start + 1);
        if (url_start >= 0 && url_end >= 0 && (url_end - url_start) < 250) {
            String asset_url = body.substring(url_start + 1, url_end);
            if (asset_url.endsWith(".bin")) {
                strncpy(download_url, asset_url.c_str(), sizeof(download_url) - 1);
                break;
            }
        }
        dl_pos = body.indexOf("\"browser_download_url\"", dl_pos + 21);
    }

    if (!download_url[0]) {
        snprintf(s_status, sizeof(s_status), "No .bin asset in release");
        s_checking = false;
        g_deferred_features_dirty = true;
        return;
    }

    snprintf(logbuf, sizeof(logbuf), "OTA: downloading %s", download_url);
    serialmon_append(logbuf);

    // Start download
    s_client = new WiFiClientSecure();
    s_client->setInsecure();
    s_http = new HTTPClient();
    s_http->setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    s_http->setTimeout(15000);

    if (!s_http->begin(*s_client, download_url)) {
        snprintf(s_status, sizeof(s_status), "Download connect failed");
        ota_cleanup_http();
        s_checking = false;
        g_deferred_features_dirty = true;
        return;
    }

    code = s_http->GET();
    esp_task_wdt_reset();

    if (code != 200) {
        snprintf(s_status, sizeof(s_status), "Download HTTP %d", code);
        ota_cleanup_http();
        s_checking = false;
        g_deferred_features_dirty = true;
        return;
    }

    s_total_size = s_http->getSize();
    if (s_total_size <= 0) {
        snprintf(s_status, sizeof(s_status), "Unknown file size");
        ota_cleanup_http();
        s_checking = false;
        g_deferred_features_dirty = true;
        return;
    }

    if (!Update.begin(s_total_size)) {
        snprintf(s_status, sizeof(s_status), "Not enough space");
        ota_cleanup_http();
        s_checking = false;
        g_deferred_features_dirty = true;
        return;
    }

    s_stream = s_http->getStreamPtr();
    s_written = 0;
    s_progress = 0;
    s_state = OTA_DOWNLOADING;
    s_checking = false;
    snprintf(s_status, sizeof(s_status), "Downloading v%s...", tag.c_str());
    g_deferred_features_dirty = true;
#endif
}

// ── Loop: process download chunks ───────────────────────────

void ota_loop() {
#if defined(ESP32)
    if (s_state != OTA_DOWNLOADING || !s_stream) return;

    esp_task_wdt_reset();

    int avail = s_stream->available();
    if (avail > 0) {
        uint8_t buf[4096];
        int len = s_stream->readBytes(buf, min(avail, (int)sizeof(buf)));
        if (len > 0) {
            size_t written = Update.write(buf, len);
            if (written != (size_t)len) {
                snprintf(s_status, sizeof(s_status), "Write error");
                Update.abort();
                ota_cleanup_http();
                s_state = OTA_FAILED;
                g_deferred_features_dirty = true;
                return;
            }
            s_written += len;
            s_progress = (uint8_t)((uint64_t)s_written * 100 / s_total_size);
            g_deferred_features_dirty = true;
        }
    }

    // Check completion
    if (s_written >= s_total_size) {
        if (Update.end(true)) {
            snprintf(s_status, sizeof(s_status), "Done! Rebooting...");
            s_state = OTA_DONE;
            g_deferred_features_dirty = true;
            serialmon_append("OTA complete, rebooting");
            ota_cleanup_http();
            delay(1000);
            ESP.restart();
        } else {
            snprintf(s_status, sizeof(s_status), "Verify failed");
            s_state = OTA_FAILED;
            g_deferred_features_dirty = true;
            ota_cleanup_http();
        }
    }

    // Timeout check: if no data for 30 seconds, abort
    static uint32_t s_last_data_ms = 0;
    if (avail > 0) {
        s_last_data_ms = millis();
    } else if (s_last_data_ms > 0 && (millis() - s_last_data_ms) > 30000) {
        snprintf(s_status, sizeof(s_status), "Download timeout");
        Update.abort();
        ota_cleanup_http();
        s_state = OTA_FAILED;
        g_deferred_features_dirty = true;
        s_last_data_ms = 0;
    } else if (s_last_data_ms == 0) {
        s_last_data_ms = millis();
    }
#endif
}
