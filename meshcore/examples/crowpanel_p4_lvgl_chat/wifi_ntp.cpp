// ============================================================
// wifi_ntp.cpp — WiFi connection, NTP time sync, credential storage
// P4 port of v11's wifi_ntp.cpp. Differences:
//   * WiFi runs over ESP-Hosted SDIO to the C6 coprocessor, so wifi_init()
//     must call WiFi.setPins() before WiFi.mode().
//   * DHE04005D has no coin-cell RTC, so wifi_write_time_to_rtc() is a
//     no-op (NTP is the only time source after boot).
//   * Auth-class disconnects (wrong password, no AP, handshake timeout)
//     trigger a hard teardown of the WiFi stack so ESP-Hosted's SDIO RX
//     buffer pool doesn't leak and panic the device. User has to press
//     Connect in Settings to retry.
//   * C6 firmware verification / upgrade is out of scope for this image
//     and lives in a separate selector firmware.
// ============================================================

#include "wifi_ntp.h"
#include "app_globals.h"
#include "utils.h"
#include "settings_cb.h"
#include "board_config.h"

#include <WiFi.h>
#include <Preferences.h>
#include <time.h>
#include <target.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

// ---- State ----
bool g_wifi_enabled = false;
bool g_wifi_connected = false;
bool g_wifi_has_saved_network = false;
char g_wifi_ssid[33] = {};

static char s_wifi_pass[65] = {};
static Preferences s_wifi_prefs;

// ---- Multi-slot saved-network storage ----
// Up to WIFI_MAX_SLOTS known networks in NVS. `g_wifi_ssid` /
// `s_wifi_pass` always mirror the ACTIVE slot so every existing
// call-site (WiFi.begin, status display, etc) keeps working without
// knowing about slots. Slot 0 is also where the legacy single-slot
// storage (keys "ssid"/"pass") gets migrated on first boot after
// this firmware version lands.
#define WIFI_MAX_SLOTS 5

struct WifiSlot {
    char ssid[33];   // empty string if slot is unused
    char pass[65];
};
static WifiSlot s_slots[WIFI_MAX_SLOTS] = {};
static int8_t   s_active_slot = -1;  // -1 = no slot selected
static uint32_t s_last_ntp_sync_ms = 0;
static uint32_t s_reconnect_ms = 0;
static bool s_ntp_synced_once = false;
static bool s_pins_configured = false;
static SemaphoreHandle_t s_ntp_sem = nullptr;
static TaskHandle_t s_ntp_task = nullptr;
// Exposed for the Settings UI so it can render "Wrong password" / "Network
// not found" etc. when a connect attempt fails.
uint8_t g_wifi_last_disc_reason = 0;

const char* wifi_last_error_text() {
  // Map a handful of common wifi_err_reason_t values to short, user-facing
  // strings. All other codes fall through to nullptr so the caller can keep
  // the generic "Saved: <SSID> (not connected)" phrasing.
  switch (g_wifi_last_disc_reason) {
    case 15:    // WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT
    case 202:   // WIFI_REASON_AUTH_FAIL
      return "Wrong password";
    case 201:   // WIFI_REASON_NO_AP_FOUND
      return "Network not found";
    case 200:   // WIFI_REASON_BEACON_TIMEOUT (AP stopped broadcasting)
      return "AP out of range";
    case 204:   // WIFI_REASON_HANDSHAKE_TIMEOUT
      return "Authentication timeout";
    case 205:   // WIFI_REASON_CONNECTION_FAIL
      return "Connection failed";
    default:
      return nullptr;
  }
}
static const uint32_t NTP_SYNC_INTERVAL_MS = 3600000UL;   // hourly re-sync
// Cold-boot tends to lose the first WiFi.begin() to the ESP-Hosted SDIO RPC
// init on the C6. Instead of waiting 30 s for the retry, back off 3→6→15→30.
static const uint32_t RECONNECT_INTERVALS_MS[] = { 3000, 6000, 15000, 30000 };
static uint8_t        s_reconnect_attempt = 0;

// Deferred scan state
static volatile bool s_scan_requested = false;
static bool s_scan_results_ready = false;
static int  s_scan_result_count = 0;

// ---- P4-specific: wire SDIO pins + event callback once ----

// Flag set by the event callback when an auth-class disconnect fires.
// wifi_loop() drains it on the next tick and fully tears down WiFi — the
// C6 ESP-Hosted stack otherwise keeps retrying internally regardless of
// WiFi.setAutoReconnect(false), and every retry leaks one SDIO RX buffer
// until sdio_rx_get_buffer asserts.
static volatile bool s_teardown_on_loop = false;

// True when an ongoing drop should trigger an automatic reconnect
// attempt later (set by the event handler on non-auth disconnects).
// wifi_loop() drains it by calling wifi_connect_saved() once the
// backoff timer elapses.
static volatile bool s_auto_reconnect_pending = false;

// Deadline (millis) for a deferred connect queued by wifi_toggle(true).
// Toggling WiFi ON synchronously right after WIFI_OFF is reliably
// broken: WiFi.begin() fires before the C6's hosted WiFi subsystem
// has finished re-booting, association fails with NO_AP_FOUND, which
// our event handler then treats as auth-fatal and tears down. By
// setting this to millis()+1500 instead of calling begin() inline,
// wifi_loop() can fire the first begin() once the C6 is ready — the
// LVGL task also doesn't block on the connect for ~400 ms.
// 0 means no deferred connect queued.
static uint32_t s_settle_connect_ms = 0;

// Deferred WiFi.begin() timestamp, armed by wifi_connect_saved() when
// the radio mode had to change to STA. 0 means no pending begin.
// Replaces the old inline `delay(500)` that blocked the main loop on
// every auto-reconnect tick.
static uint32_t s_begin_at_ms = 0;

static void wifi_p4_event_cb(arduino_event_id_t ev, arduino_event_info_t info) {
  // IMPORTANT: this callback must NOT write g_wifi_connected. wifi_loop() is
  // the sole owner of that flag (derived from WiFi.status()), so that its
  // `was_connected = g_wifi_connected` / `g_wifi_connected = …` pair can
  // reliably detect the disconnected→connected edge and fire the one-shot
  // UI refresh.
  switch (ev) {
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
      uint8_t r = info.wifi_sta_disconnected.reason;
      g_wifi_last_disc_reason = r;
      Serial.printf("[wifi] disc reason=%u\n", (unsigned)r);
      g_deferred_wifi_status_dirty = true;
      // Auth/credential/no-AP class failures are fatal for the C6's
      // internal retry loop; the next wifi_loop tick will fully tear
      // the stack down. Auto-reconnect is NOT safe for these because
      // each failed association leaks an SDIO RX buffer.
      //
      // DO NOT include reason 8 (WIFI_REASON_AUTH_LEAVE) here — that's
      // the code emitted when WE call WiFi.disconnect() ourselves
      // (e.g. on toggle-off). Marking it auth-fatal previously caused
      // a race: toggle-off → our disconnect fires a reason=8 event →
      // s_teardown_on_loop set → on the next loop tick (which might
      // run AFTER toggle-on has scheduled s_settle_connect_ms),
      // teardown path clears s_settle_connect_ms, so the pending
      // reconnect never fires and UI stays at "Saved: … (not
      // connected)" until reboot. Credit: Codex diagnosed this.
      // Only TRUE auth failures go through the hard-teardown path,
      // because those are the ones where the C6 keeps leaking SDIO
      // RX buffers on each retry. Transient signal-loss conditions
      // (BEACON_TIMEOUT, NO_AP_FOUND, CONNECTION_FAIL) must NOT tear
      // down — the teardown leaves ESP-Hosted in a broken state and
      // subsequent begin()/scanNetworks() all return ESP_FAIL with
      // "ensure_slave_bus_ready failed" / "ESP-Hosted link not yet up"
      // until the chip is rebooted.
      //
      // Reason code semantics (from esp_wifi_types_generic.h):
      //   15  WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT → wrong password
      //   200 WIFI_REASON_BEACON_TIMEOUT         → AP stopped broadcasting (transient)
      //   201 WIFI_REASON_NO_AP_FOUND            → AP not in range (transient)
      //   202 WIFI_REASON_AUTH_FAIL              → wrong password
      //   204 WIFI_REASON_HANDSHAKE_TIMEOUT      → wrong password / very weak signal
      //   205 WIFI_REASON_CONNECTION_FAIL        → generic (transient)
      //
      // Keeping 15, 202, 204 auth-fatal. 200, 201, 205 go through
      // the soft auto-reconnect backoff.
      const bool auth_fatal = (r == 15 || r == 202 || r == 204);
      if (auth_fatal) {
        s_teardown_on_loop = true;
      } else if (g_wifi_enabled) {
        // Non-fatal disconnect (signal lost, AP reboot, AP out of
        // range, beacon timeout). Safe to auto-reconnect — schedule
        // a retry via the existing backoff. wifi_loop picks it up.
        s_auto_reconnect_pending = true;
      }
      break;
    }
    default: break;
  }
}

static void wifi_configure_pins_once() {
  if (s_pins_configured) return;
  WiFi.setPins(
    WIFI_HOSTED_SDIO_PIN_CLK, WIFI_HOSTED_SDIO_PIN_CMD,
    WIFI_HOSTED_SDIO_PIN_D0,  WIFI_HOSTED_SDIO_PIN_D1,
    WIFI_HOSTED_SDIO_PIN_D2,  WIFI_HOSTED_SDIO_PIN_D3,
    WIFI_HOSTED_SDIO_PIN_RESET);
  WiFi.onEvent(wifi_p4_event_cb);
  s_pins_configured = true;
}

// ---- Credential management (multi-slot) ----

// Write all slots + active index to NVS. Called from save/forget paths.
static void wifi_persist_slots() {
  s_wifi_prefs.begin("wifi", false);
  for (int i = 0; i < WIFI_MAX_SLOTS; ++i) {
    char k[8];
    snprintf(k, sizeof(k), "ssid%d", i);
    s_wifi_prefs.putString(k, s_slots[i].ssid);
    snprintf(k, sizeof(k), "pass%d", i);
    s_wifi_prefs.putString(k, s_slots[i].pass);
  }
  s_wifi_prefs.putChar("active", s_active_slot);
  s_wifi_prefs.end();
}

// Mirror the active slot's credentials into the globals every existing
// caller uses (WiFi.begin, the UI label, etc.). Also updates the
// "has saved network" flag.
static void wifi_sync_active_globals() {
  if (s_active_slot >= 0 && s_active_slot < WIFI_MAX_SLOTS
      && s_slots[s_active_slot].ssid[0]) {
    strncpy(g_wifi_ssid, s_slots[s_active_slot].ssid, sizeof(g_wifi_ssid) - 1);
    g_wifi_ssid[sizeof(g_wifi_ssid) - 1] = '\0';
    strncpy(s_wifi_pass, s_slots[s_active_slot].pass, sizeof(s_wifi_pass) - 1);
    s_wifi_pass[sizeof(s_wifi_pass) - 1] = '\0';
    g_wifi_has_saved_network = true;
  } else {
    g_wifi_ssid[0] = '\0';
    s_wifi_pass[0] = '\0';
    g_wifi_has_saved_network = false;
  }
}

void wifi_load_credentials() {
  s_wifi_prefs.begin("wifi", true);

  // Read the 5 slots.
  for (int i = 0; i < WIFI_MAX_SLOTS; ++i) {
    char k[8];
    snprintf(k, sizeof(k), "ssid%d", i);
    String s = s_wifi_prefs.getString(k, "");
    snprintf(k, sizeof(k), "pass%d", i);
    String p = s_wifi_prefs.getString(k, "");
    strncpy(s_slots[i].ssid, s.c_str(), sizeof(s_slots[i].ssid) - 1);
    s_slots[i].ssid[sizeof(s_slots[i].ssid) - 1] = '\0';
    strncpy(s_slots[i].pass, p.c_str(), sizeof(s_slots[i].pass) - 1);
    s_slots[i].pass[sizeof(s_slots[i].pass) - 1] = '\0';
  }
  s_active_slot  = (int8_t)s_wifi_prefs.getChar("active", -1);
  g_wifi_enabled = s_wifi_prefs.getUChar("enabled", 0) != 0;

  // One-shot migration from the legacy single-slot schema. Old
  // firmwares stored "ssid"/"pass" at the top level. If slot 0 is
  // empty but those keys exist, copy them forward and clear the
  // old keys so this only runs once.
  if (s_slots[0].ssid[0] == '\0') {
    String legacy_ssid = s_wifi_prefs.getString("ssid", "");
    if (legacy_ssid.length() > 0) {
      String legacy_pass = s_wifi_prefs.getString("pass", "");
      strncpy(s_slots[0].ssid, legacy_ssid.c_str(), sizeof(s_slots[0].ssid) - 1);
      s_slots[0].ssid[sizeof(s_slots[0].ssid) - 1] = '\0';
      strncpy(s_slots[0].pass, legacy_pass.c_str(), sizeof(s_slots[0].pass) - 1);
      s_slots[0].pass[sizeof(s_slots[0].pass) - 1] = '\0';
      s_wifi_prefs.end();
      // Re-open writable to persist migration + remove old keys.
      s_wifi_prefs.begin("wifi", false);
      s_wifi_prefs.putString("ssid0", s_slots[0].ssid);
      s_wifi_prefs.putString("pass0", s_slots[0].pass);
      s_wifi_prefs.remove("ssid");
      s_wifi_prefs.remove("pass");
      if (s_active_slot < 0) {
        s_active_slot = 0;
        s_wifi_prefs.putChar("active", 0);
      }
      s_wifi_prefs.end();
      serialmon_append("WiFi: migrated legacy credentials to slot 0");
      s_wifi_prefs.begin("wifi", true);
    }
  }

  s_wifi_prefs.end();

  // Ensure s_active_slot points at a non-empty slot if possible.
  if (s_active_slot < 0 || s_active_slot >= WIFI_MAX_SLOTS
      || s_slots[s_active_slot].ssid[0] == '\0') {
    s_active_slot = -1;
    for (int i = 0; i < WIFI_MAX_SLOTS; ++i) {
      if (s_slots[i].ssid[0]) { s_active_slot = (int8_t)i; break; }
    }
  }

  wifi_sync_active_globals();
}

void wifi_save_credentials(const char* ssid, const char* password) {
  if (!ssid || !ssid[0]) return;

  // Find a slot for these credentials, priority order:
  //   1. Slot whose SSID already matches — update its password.
  //   2. First empty slot — fill.
  //   3. No room: overwrite the currently-active slot (treating
  //      this as "re-save this network with a new password").
  int target = -1;
  for (int i = 0; i < WIFI_MAX_SLOTS; ++i) {
    if (strcmp(s_slots[i].ssid, ssid) == 0) { target = i; break; }
  }
  if (target < 0) {
    for (int i = 0; i < WIFI_MAX_SLOTS; ++i) {
      if (s_slots[i].ssid[0] == '\0') { target = i; break; }
    }
  }
  if (target < 0) {
    target = (s_active_slot >= 0) ? s_active_slot : 0;
  }

  strncpy(s_slots[target].ssid, ssid, sizeof(s_slots[target].ssid) - 1);
  s_slots[target].ssid[sizeof(s_slots[target].ssid) - 1] = '\0';
  strncpy(s_slots[target].pass, password ? password : "",
          sizeof(s_slots[target].pass) - 1);
  s_slots[target].pass[sizeof(s_slots[target].pass) - 1] = '\0';
  s_active_slot = (int8_t)target;

  wifi_sync_active_globals();
  wifi_persist_slots();
}

void wifi_forget_credentials() {
  // Forget the active slot, promote another filled slot to active
  // if one exists. Matches the old "Forget" UX: clear the network
  // you're connected to, keep everything else.
  if (s_active_slot >= 0 && s_active_slot < WIFI_MAX_SLOTS) {
    s_slots[s_active_slot].ssid[0] = '\0';
    s_slots[s_active_slot].pass[0] = '\0';
  }
  s_active_slot = -1;
  for (int i = 0; i < WIFI_MAX_SLOTS; ++i) {
    if (s_slots[i].ssid[0]) { s_active_slot = (int8_t)i; break; }
  }

  wifi_sync_active_globals();
  wifi_persist_slots();
  wifi_disconnect();
}

bool wifi_has_credentials() {
  return g_wifi_has_saved_network && g_wifi_ssid[0] != '\0';
}

// ---- Slot enumeration API (used by the Saved Networks dropdown) ----

int wifi_slot_count() {
  int n = 0;
  for (int i = 0; i < WIFI_MAX_SLOTS; ++i) {
    if (s_slots[i].ssid[0]) ++n;
  }
  return n;
}

const char* wifi_slot_ssid(int idx) {
  if (idx < 0 || idx >= WIFI_MAX_SLOTS) return "";
  return s_slots[idx].ssid;
}

int wifi_slot_active_index() {
  return s_active_slot;
}

bool wifi_slot_activate(int idx) {
  if (idx < 0 || idx >= WIFI_MAX_SLOTS) return false;
  if (s_slots[idx].ssid[0] == '\0') return false;  // empty slot
  if (idx == s_active_slot) return true;           // already active

  s_active_slot = (int8_t)idx;
  wifi_sync_active_globals();
  wifi_persist_slots();

  // Kick a fresh connect if Wi-Fi is enabled so the new slot
  // actually becomes the associated network.
  if (g_wifi_enabled) {
    s_reconnect_attempt = 0;  // reset backoff for the new target
    wifi_connect_saved();
  }
  return true;
}

bool wifi_slot_forget(int idx) {
  if (idx < 0 || idx >= WIFI_MAX_SLOTS) return false;
  if (s_slots[idx].ssid[0] == '\0') return true;

  // Forgetting the active slot tears down the connection; forgetting
  // a non-active slot is just a storage update, no disconnect.
  bool was_active = (idx == s_active_slot);
  s_slots[idx].ssid[0] = '\0';
  s_slots[idx].pass[0] = '\0';

  if (was_active) {
    s_active_slot = -1;
    for (int i = 0; i < WIFI_MAX_SLOTS; ++i) {
      if (s_slots[i].ssid[0]) { s_active_slot = (int8_t)i; break; }
    }
    wifi_sync_active_globals();
    wifi_disconnect();
  }
  wifi_persist_slots();
  return true;
}

static void wifi_save_enabled(bool en) {
  s_wifi_prefs.begin("wifi", false);
  s_wifi_prefs.putUChar("enabled", en ? 1 : 0);
  s_wifi_prefs.end();
}

// ---- RTC hardware time persistence ----
// DHE04005D has no coin-cell RTC, so there is nothing to persist to. Left as
// a stub so the single call site in wifi_ntp_sync() stays source-compatible
// with v11 — if a future variant adds a battery-backed RTC, fill this in.
static void wifi_write_time_to_rtc(uint32_t /*utc_epoch*/) { /* no-op on P4 */ }

// ---- NTP sync ----

static void wifi_ntp_sync_blocking() {
  if (!g_wifi_connected) return;

  configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");

  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 5000)) {
    time_t now;
    time(&now);
    uint32_t epoch = (uint32_t)now;
    mesh_set_time_epoch(epoch);
    tz_update_offset_now();   // recalculate DST with accurate UTC time
    wifi_write_time_to_rtc(epoch);
    g_deferred_timelabel_dirty = true;
    s_last_ntp_sync_ms = millis();
    s_ntp_synced_once = true;

    char buf[48];
    snprintf(buf, sizeof(buf), "NTP sync OK: %04d-%02d-%02d %02d:%02d UTC",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min);
    serialmon_append(buf);
  } else {
    serialmon_append("NTP sync failed");
  }
}

static void wifi_ntp_task(void*) {
  for (;;) {
    if (xSemaphoreTake(s_ntp_sem, portMAX_DELAY) != pdTRUE) continue;
    wifi_ntp_sync_blocking();
  }
}

void wifi_ntp_sync() {
  if (!s_ntp_sem) {
    wifi_ntp_sync_blocking();
    return;
  }
  xSemaphoreGive(s_ntp_sem);
}

// ---- Connection control ----

void wifi_init() {
  Serial.println("[wifi] wifi_init() begin");
  if (!s_ntp_task) {
    s_ntp_sem = xSemaphoreCreateBinary();
    // 6 KB stack. Was 2 KB when wifi_ntp_sync only did configTime +
    // settimeofday, but now it also flows through rtc_clock.setCurrentTime
    // which on this variant performs DS3231 I2C write + verify-read +
    // Serial.printf — each adds locals, and the cache-access-error panic
    // we hit at +9.13 s on Core 1 was a stack overrun in this task right
    // after "RTC: chip set to X (verified)" printed.
    if (!s_ntp_sem ||
        xTaskCreate(wifi_ntp_task, "wifiNtp", 6144, nullptr, 1, &s_ntp_task) != pdPASS) {
      s_ntp_task = nullptr;
      serialmon_append("NTP worker init failed");
    }
  }
  wifi_configure_pins_once();
  wifi_load_credentials();
  Serial.printf("[wifi] creds loaded: ssid=\"%s\" has_saved=%d enabled=%d\n",
                g_wifi_ssid, (int)g_wifi_has_saved_network, (int)g_wifi_enabled);

  if (g_wifi_enabled && wifi_has_credentials()) {
    // Match the proven old-path sequence: set mode, give the hosted link
    // ~500 ms to settle, then begin(). Skipping the settle window caused
    // WiFi.begin() to race with the ESP-Hosted SDIO init and never
    // associate.
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);
    delay(500);
    Serial.printf("[wifi] WiFi.begin(\"%s\")\n", g_wifi_ssid);
    WiFi.begin(g_wifi_ssid, s_wifi_pass);
    {
    uint8_t idx = s_reconnect_attempt < (sizeof(RECONNECT_INTERVALS_MS)/sizeof(RECONNECT_INTERVALS_MS[0]))
                  ? s_reconnect_attempt
                  : (uint8_t)((sizeof(RECONNECT_INTERVALS_MS)/sizeof(RECONNECT_INTERVALS_MS[0])) - 1);
    s_reconnect_ms = millis() + RECONNECT_INTERVALS_MS[idx];
    s_reconnect_attempt++;
  }

    char msg[80];
    snprintf(msg, sizeof(msg), "WiFi: connecting to %s...", g_wifi_ssid);
    serialmon_append(msg);
  } else {
    Serial.println("[wifi] skipping auto-connect (not enabled or no creds)");
  }
}

void wifi_connect_saved() {
  if (!wifi_has_credentials()) {
    Serial.println("[wifi] connect_saved: no creds");
    return;
  }

  // Pressing Connect (or the reconnect timer firing) is an explicit
  // "enable WiFi" — flip the flag and persist so wifi_loop() also tracks
  // state transitions and the UI status refreshes.
  if (!g_wifi_enabled) {
    g_wifi_enabled = true;
    wifi_save_enabled(true);
  }

  // Clear any stale error so the UI doesn't flash last attempt's
  // "Wrong password" before the new attempt has a chance to resolve.
  g_wifi_last_disc_reason = 0;
  // A connect attempt is about to start; drop any pending
  // auto-reconnect flag so we don't double-fire.
  s_auto_reconnect_pending = false;

  wifi_configure_pins_once();

  // The old sequence did WiFi.mode(WIFI_STA) + delay(500) + WiFi.begin()
  // unconditionally. That 500 ms block fires on every auto-reconnect
  // attempt (wifi_loop → wifi_connect_saved when the backoff timer
  // elapses), and when the saved AP is out of range it produced a
  // visible LVGL hitch every 5-30 s.
  //
  // Split it: only pay the settle delay when the radio mode actually
  // changed (fresh boot, after wifi_toggle off/on, after a teardown
  // on auth failure). If we're already in STA, skip straight to
  // WiFi.begin() — begin() handles a re-association from STA just
  // fine and doesn't need the extra pause.
  wifi_mode_t cur_mode = WiFi.getMode();
  if (cur_mode != WIFI_STA && cur_mode != WIFI_AP_STA) {
    WiFi.mode(WIFI_STA);
    // Defer the begin() to wifi_loop after the settle window. We
    // still need to announce the pending action to the user and arm
    // the reconnect timer; the actual begin() fires on the next
    // wifi_loop iteration once s_begin_at_ms has elapsed.
    s_begin_at_ms = millis() + 500;
  } else {
    // Radio already in STA — begin() immediately, no settle needed.
    s_begin_at_ms = 0;
    Serial.printf("[wifi] begin(\"%s\")\n", g_wifi_ssid);
    WiFi.begin(g_wifi_ssid, s_wifi_pass);
  }

  char msg[80];
  snprintf(msg, sizeof(msg), "WiFi: reconnecting to %s...", g_wifi_ssid);
  serialmon_append(msg);
  {
    uint8_t idx = s_reconnect_attempt < (sizeof(RECONNECT_INTERVALS_MS)/sizeof(RECONNECT_INTERVALS_MS[0]))
                  ? s_reconnect_attempt
                  : (uint8_t)((sizeof(RECONNECT_INTERVALS_MS)/sizeof(RECONNECT_INTERVALS_MS[0])) - 1);
    s_reconnect_ms = millis() + RECONNECT_INTERVALS_MS[idx];
    s_reconnect_attempt++;
  }
}

void wifi_disconnect() {
  // Drop the association but KEEP the hosted WiFi stack up. The
  // ESP-Hosted SDIO RPC doesn't have a clean "stop + restart WiFi"
  // path — calling WiFi.disconnect(true)/WIFI_OFF leaves the C6 in
  // a state where the next WIFI_STA + begin() reliably fails with
  // NO_AP_FOUND until the device is cold-rebooted. Leaving the
  // stack running costs a little idle power but makes toggle
  // off→on actually work without a reboot.
  WiFi.disconnect(false);
  g_wifi_connected = false;
  s_reconnect_ms = 0;
  s_begin_at_ms  = 0;   // cancel any deferred begin()
}

void wifi_toggle(bool enable) {
  g_wifi_enabled = enable;
  wifi_save_enabled(enable);

  // Always drop stale reconnect state when toggling. Without this, a
  // prior maxed-out backoff (30 s) or a stale teardown flag from a
  // previous session would delay or outright block the fresh
  // connect attempt — user sees the toggle flip but the icon stays
  // red for 30 s before anything happens.
  s_reconnect_ms            = 0;
  s_reconnect_attempt       = 0;
  s_teardown_on_loop        = false;
  s_auto_reconnect_pending  = false;
  s_settle_connect_ms       = 0;
  s_begin_at_ms             = 0;
  g_wifi_last_disc_reason   = 0;

  if (enable) {
    wifi_configure_pins_once();
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);
    // Defer the first WiFi.begin() so (a) the LVGL task doesn't
    // block on the SDIO RPC from the toggle handler and (b) the C6
    // has time to re-init its association state machine. 2 s has
    // been the empirically-reliable figure on this board; shorter
    // windows (800 ms, 1500 ms) still saw the first post-toggle
    // begin() come back without actually associating, requiring a
    // reboot to recover.
    if (wifi_has_credentials()) {
      s_settle_connect_ms = millis() + 2000;
    }
  } else {
    // Deliberately NOT calling WiFi.mode(WIFI_OFF) — see the comment
    // in wifi_disconnect(). Dropping the association is enough to
    // stop traffic; the stack stays up so the next toggle-on can
    // reliably re-associate.
    wifi_disconnect();
  }
}

// ---- Scanning (deferred synchronous) ----

void wifi_request_scan() {
  s_scan_results_ready = false;
  s_scan_result_count = 0;
  s_scan_requested = true;
  serialmon_append("WiFi: scan requested...");
}

bool wifi_scan_results_ready() { return s_scan_results_ready; }
int  wifi_scan_result_count()  { return s_scan_result_count; }
void wifi_scan_results_consumed() {
  s_scan_results_ready = false;
  s_scan_result_count = 0;
}

String wifi_scan_ssid(int idx) { return WiFi.SSID(idx); }
int    wifi_scan_rssi(int idx) { return WiFi.RSSI(idx); }

static void wifi_do_sync_scan() {
  wifi_configure_pins_once();
  wifi_mode_t mode = WiFi.getMode();
  const bool needs_mode_change = (mode != WIFI_STA && mode != WIFI_AP_STA);
  if (needs_mode_change) {
    WiFi.mode(WIFI_STA);
  }

  // If we're currently associated, just scan — ESP-Hosted supports
  // scanning from WL_CONNECTED and we must NOT drop the user's live
  // session. If we're NOT connected, the radio may be stuck mid-
  // teardown (e.g. right after tapping "Forget", or after a
  // NO_AP_FOUND settled us back to STA but still in a transient
  // state). Cancel any in-flight association and give the C6 a
  // settle window before asking for a scan — without this,
  // scanNetworks() reliably returns 0.
  const bool currently_connected = (WiFi.status() == WL_CONNECTED);
  if (!currently_connected) {
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false);
    // 2 s settle universally when not connected. WiFi.disconnect() is
    // async on ESP-Hosted — the C6 reports disconnected to the host
    // with a lag that has been measured up to ~1.5 s in NO_AP_FOUND
    // and just-after-Forget cases. A 600 ms delay was empirically
    // too short; a user-initiated Scan happens rarely enough that
    // paying the extra 1.4 s to guarantee a non-zero result is fine.
    delay(2000);
  }

  WiFi.scanDelete();
  int n = WiFi.scanNetworks(false, false, false, 300);

  if (n > 0) {
    s_scan_result_count = n;
    char msg[48];
    snprintf(msg, sizeof(msg), "WiFi: scan found %d network(s)", n);
    serialmon_append(msg);
  } else {
    s_scan_result_count = 0;
    serialmon_append("WiFi: scan found 0 networks");
  }
  s_scan_results_ready = true;
}

// ---- Loop ----

void wifi_loop() {
  if (s_teardown_on_loop) {
    s_teardown_on_loop = false;
    // Kill the WiFi stack HARD so the C6 stops its internal retry loop.
    // The user has to press Connect again from Settings to try once more.
    Serial.println("[wifi] teardown after auth failure (stops C6 retry leak)");
    WiFi.disconnect(true, false);
    s_reconnect_ms = 0;
    s_reconnect_attempt = 0;
    s_settle_connect_ms = 0;     // cancel any queued settle-connect
    s_begin_at_ms       = 0;     // cancel any deferred begin()
    // Also latch g_wifi_enabled OFF and drop any pending auto-
    // reconnect. Without this, a stale code path (reconnect timer
    // fallback, UI toggle state, etc.) can re-enter wifi_connect_saved
    // before ESP-Hosted finishes its teardown, which in turn triggers
    // a broken "reconfigure" of the C6 transport — ESP-Hosted then
    // returns ESP_FAIL on every subsequent call and any downstream
    // code path that touches a WiFi-owned FreeRTOS handle crashes
    // (including the PNG decoder on Maps entry, via a shared lock).
    g_wifi_enabled           = false;
    s_auto_reconnect_pending = false;
    serialmon_append("WiFi stopped — press Connect to retry");
  }

  // Deferred connect queued by wifi_toggle(true). Fires once the C6
  // has had time to finish re-booting its hosted WiFi subsystem.
  if (s_settle_connect_ms != 0 && (int32_t)(millis() - s_settle_connect_ms) >= 0
      && g_wifi_enabled && wifi_has_credentials()) {
    s_settle_connect_ms = 0;
    wifi_connect_saved();
  }

  // Deferred WiFi.begin() following a mode change inside
  // wifi_connect_saved(). The 500 ms settle used to block the main
  // loop via delay(); now we check the timestamp here and fire the
  // begin() on a later iteration when the C6 has had time to settle.
  if (s_begin_at_ms != 0 && (int32_t)(millis() - s_begin_at_ms) >= 0
      && wifi_has_credentials()) {
    s_begin_at_ms = 0;
    Serial.printf("[wifi] begin(\"%s\") [deferred]\n", g_wifi_ssid);
    WiFi.begin(g_wifi_ssid, s_wifi_pass);
  }

  if (s_scan_requested) {
    s_scan_requested = false;
    wifi_do_sync_scan();
  }

  // State tracking always runs — an explicit wifi_connect_saved() from the
  // Connect button can associate even if g_wifi_enabled was still false a
  // moment earlier, and we still want the UI label to flip to "Connected".
  bool was_connected = g_wifi_connected;
  g_wifi_connected = (WiFi.status() == WL_CONNECTED);

  if (g_wifi_connected && !was_connected) {
    s_reconnect_attempt      = 0;       // reset backoff for next outage
    s_auto_reconnect_pending = false;   // any pending retry satisfied
    s_reconnect_ms           = 0;
    g_wifi_last_disc_reason  = 0;       // clear any stale error
    char msg[64];
    snprintf(msg, sizeof(msg), "WiFi: connected to %s (%s, RSSI %d dBm)",
             g_wifi_ssid, WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
    serialmon_append(msg);
    g_deferred_wifi_status_dirty = true;
    wifi_ntp_sync();
  }

  if (!g_wifi_connected && was_connected) {
    serialmon_append("WiFi: disconnected");
    g_deferred_wifi_status_dirty = true;
  }

  // Auto-reconnect for non-auth disconnects only. Auth-class failures
  // still go through the hard teardown (see s_teardown_on_loop above)
  // because each failed association leaks an SDIO RX buffer on the
  // C6 link. For signal-loss / AP-reboot disconnects, though, the
  // re-association succeeds with the same credentials and the leak
  // doesn't happen — so we retry with an exponential backoff.
  if (s_auto_reconnect_pending && g_wifi_enabled && !g_wifi_connected
      && wifi_has_credentials() && !s_teardown_on_loop) {
    if (s_reconnect_ms == 0) {
      // First tick after the disconnect: arm the backoff.
      const uint8_t idx = s_reconnect_attempt <
          (sizeof(RECONNECT_INTERVALS_MS) / sizeof(RECONNECT_INTERVALS_MS[0]))
            ? s_reconnect_attempt
            : (uint8_t)((sizeof(RECONNECT_INTERVALS_MS) /
                         sizeof(RECONNECT_INTERVALS_MS[0])) - 1);
      s_reconnect_ms = millis() + RECONNECT_INTERVALS_MS[idx];
    } else if ((int32_t)(millis() - s_reconnect_ms) >= 0) {
      // Timer elapsed — fire a connect attempt and clear the pending
      // flag. wifi_connect_saved() re-arms s_reconnect_ms internally
      // for the NEXT backoff tick in case this attempt also fails.
      s_auto_reconnect_pending = false;
      wifi_connect_saved();
    }
  }

  if (g_wifi_connected && s_ntp_synced_once &&
      (millis() - s_last_ntp_sync_ms) > NTP_SYNC_INTERVAL_MS) {
    wifi_ntp_sync();
  }
}
