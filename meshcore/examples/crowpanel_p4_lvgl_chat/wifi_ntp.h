#pragma once
// ============================================================
// wifi_ntp.h — WiFi connection, NTP time sync, credential storage
// ============================================================

#include <Arduino.h>

// WiFi state
extern bool g_wifi_enabled;
extern bool g_wifi_connected;
extern bool g_wifi_has_saved_network;
extern char g_wifi_ssid[33];

// Most recent STA_DISCONNECTED reason code (from wifi_err_reason_t).
// Non-zero after a failed association. Cleared to 0 on successful connect.
extern uint8_t g_wifi_last_disc_reason;
// Short human-readable string for the current disconnect reason,
// or nullptr if the reason is not one we translate or is 0.
const char* wifi_last_error_text();

// Initialization
void wifi_init();

// Connection control
void wifi_connect_saved();
void wifi_disconnect();
void wifi_toggle(bool enable);

// Network scanning (results accessed via wifi_scan_ssid/wifi_scan_rssi after ready)
String wifi_scan_ssid(int idx);
int  wifi_scan_rssi(int idx);

// Credential management (single-slot convenience wrappers — always
// act on whatever slot is currently ACTIVE). Multi-slot support is
// the API further down.
void wifi_save_credentials(const char* ssid, const char* password);
void wifi_forget_credentials();
bool wifi_has_credentials();
void wifi_load_credentials();

// Multi-slot saved-network storage. Up to 5 networks are persisted
// in NVS. At any moment exactly one is "active" — that's the SSID
// g_wifi_ssid reflects and the one wifi_connect_saved() connects to.
// The UI calls wifi_slot_activate() to switch between stored
// networks without re-typing the password, and wifi_slot_forget() to
// remove one specific network without disturbing the others.
int         wifi_slot_count();              // number of filled slots (0..5)
const char* wifi_slot_ssid(int idx);        // "" if idx empty / out of range
int         wifi_slot_active_index();       // -1 if no slot is active
bool        wifi_slot_activate(int idx);    // switch + reconnect
bool        wifi_slot_forget(int idx);      // clear that slot

// NTP time sync
void wifi_ntp_sync();

// Scanning (deferred synchronous)
void wifi_request_scan();          // call from UI — sets flag for loop()
bool wifi_scan_results_ready();    // true when results available
int  wifi_scan_result_count();     // number of networks found (after ready)
void wifi_scan_results_consumed(); // UI done reading — clear results

// Call from loop() — manages reconnection, periodic NTP sync, deferred scan
void wifi_loop();
