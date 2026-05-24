// web_dashboard_https.cpp — HTTPS half of the dashboard (RETIRED).
//
// The secure dashboard used to run an mbedTLS server on port 443 via
// esp_https_server so the browser Geolocation API (which is gated to
// secure origins) could provide phone coordinates. In practice the
// P4's internal-SRAM budget — after ESP-Hosted SDIO + ESPAsyncWebServer
// + LVGL partial buffers — wasn't big enough to host a concurrent
// mbedTLS server reliably. Browsers hit
//   PR_CONNECT_RESET_ERROR
// because esp_tls_create_server_session was returning
// MBEDTLS_ERR_SSL_ALLOC_FAILED mid-handshake, and every failed retry
// leaked an SDIO RX buffer (panic in sdio_rx_get_buffer /
// sdio_push_data_to_queue).
//
// The position form on the plain-HTTP dashboard still works for manual
// lat/lon entry — users type coordinates and tap Save Position.
// Phone-GPS-via-geolocation is not offered on this build.
//
// This translation unit is now a stub so the rest of the project
// doesn't need to be touched (no platformio.ini source-filter changes,
// no lingering extern references). If HTTPS ever comes back, restore
// the implementation from git history.

#if defined(ESP32)
// (No symbols exported — web_dashboard.cpp no longer references this
// file, and dashboard_cert.h / esp_https_server.h are no longer
// included by any build input. Both can be deleted on the next
// cleanup pass; leaving them in tree for now to preserve history.)
#endif
