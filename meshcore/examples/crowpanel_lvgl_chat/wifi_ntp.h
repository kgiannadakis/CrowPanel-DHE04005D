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

// Initialization
void wifi_init();

// Connection control
void wifi_connect_saved();
void wifi_disconnect();
void wifi_toggle(bool enable);

// Network scanning (results accessed via wifi_scan_ssid/wifi_scan_rssi after ready)
String wifi_scan_ssid(int idx);
int  wifi_scan_rssi(int idx);

// Credential management
void wifi_save_credentials(const char* ssid, const char* password);
void wifi_forget_credentials();
bool wifi_has_credentials();
void wifi_load_credentials();

// NTP time sync
void wifi_ntp_sync();

// Scanning (deferred synchronous)
void wifi_request_scan();          // call from UI — sets flag for loop()
bool wifi_scan_results_ready();    // true when results available
int  wifi_scan_result_count();     // number of networks found (after ready)
void wifi_scan_results_consumed(); // UI done reading — clear results

// Call from loop() — manages reconnection, periodic NTP sync, deferred scan
void wifi_loop();
