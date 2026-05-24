#include "configuration.h"
#if HAS_WIFI
#include "NodeDB.h"
#include "RTC.h"
#include "concurrency/Periodic.h"
#include "mesh/wifi/WiFiAPClient.h"

#include "main.h"
#include "mesh/api/WiFiServerAPI.h"
#include "target_specific.h"
#include <WiFi.h>

#if defined(ARCH_ESP32P4) && defined(CROWPANEL_DHE04005D)
#include "board_config.h"
#endif

#if HAS_ETHERNET && defined(USE_WS5500)
#include <ETHClass2.h>
#define ETH ETH2
#endif // HAS_ETHERNET

#include <WiFiUdp.h>
#ifdef ARCH_ESP32
#if !MESHTASTIC_EXCLUDE_WEBSERVER
#include "mesh/http/WebServer.h"
#endif
#include <ESPmDNS.h>
#include <esp_wifi.h>
static void WiFiEvent(WiFiEvent_t event);
#elif defined(ARCH_RP2040)
#include <SimpleMDNS.h>
#endif

#ifndef DISABLE_NTP
#include "Throttle.h"
#include <NTPClient.h>
#endif

#if HAS_TFT && USE_MCUI
#include "graphics/mcui/data/McClock.h"
#endif

#if !MESHTASTIC_EXCLUDE_TZ && defined(ARCH_ESP32)
#include <HTTPClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

using namespace concurrency;

// NTP
WiFiUDP ntpUDP;

#ifndef DISABLE_NTP
NTPClient timeClient(ntpUDP, config.network.ntp_server);
#endif

uint8_t wifiDisconnectReason = 0;

// Stores our hostname
char ourHost[16];

// To replace blocking wifi connect delay with a non-blocking sleep
static unsigned long wifiReconnectStartMillis = 0;
static bool wifiReconnectPending = false;

bool APStartupComplete = 0;

unsigned long lastrun_ntp = 0;

bool needReconnect = true;   // If we create our reconnector, run it once at the beginning
bool isReconnecting = false; // If we are currently reconnecting
static constexpr int32_t kReconnectPollFastMs = 100;
static constexpr int32_t kReconnectPollNormalMs = 1000;
static constexpr int32_t kReconnectIdleMs = 300000;

#if defined(ARCH_ESP32P4) && defined(CROWPANEL_DHE04005D)
static constexpr bool kCrowpanelMinimalWifiServices = true;
#else
static constexpr bool kCrowpanelMinimalWifiServices = false;
#endif

#ifndef DISABLE_NTP
static bool s_ntpClientStarted = false;
#if defined(ARCH_ESP32P4) && defined(CROWPANEL_DHE04005D)
static uint32_t s_crowpanelWifiConnectedAtMs = 0;
static constexpr uint32_t kCrowpanelNtpStartDelayMs = 45000;
#endif
#endif

#ifdef ARCH_ESP32
static bool s_wifiEventHandlersRegistered = false;
#endif

// ---------------------------------------------------------------------------
// Auto-timezone: fetch IANA timezone from IP geolocation, map to POSIX TZ
// ---------------------------------------------------------------------------
#if !MESHTASTIC_EXCLUDE_TZ && defined(ARCH_ESP32)
static bool tzAutoDetected = false;
static volatile bool tzDetectInFlight = false;
static volatile bool tzResultReady = false;
static bool tzResultSuccess = false;
static char tzDetectedDef[sizeof(config.device.tzdef)] = {};

struct IanaToPosix {
    const char *iana;
    const char *posix;
};

// Common IANA → POSIX mappings (from nayarsystems/posix_tz_db)
static const IanaToPosix tzLookup[] = {
    // Europe
    {"Europe/London", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Europe/Dublin", "IST-1GMT0,M10.5.0,M3.5.0/1"},
    {"Europe/Lisbon", "WET0WEST,M3.5.0/1,M10.5.0"},
    {"Europe/Paris", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Berlin", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Brussels", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Amsterdam", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Rome", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Madrid", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Vienna", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Zurich", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Warsaw", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Prague", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Budapest", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Stockholm", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Oslo", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Copenhagen", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Helsinki", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Athens", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Bucharest", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Sofia", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Kyiv", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Istanbul", "TRT-3"},
    {"Europe/Moscow", "MSK-3"},
    // Americas
    {"America/New_York", "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Chicago", "CST6CDT,M3.2.0,M11.1.0"},
    {"America/Denver", "MST7MDT,M3.2.0,M11.1.0"},
    {"America/Phoenix", "MST7"},
    {"America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0"},
    {"America/Anchorage", "AKST9AKDT,M3.2.0,M11.1.0"},
    {"America/Sao_Paulo", "BRT3"},
    {"America/Argentina/Buenos_Aires", "ART3"},
    {"America/Toronto", "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Vancouver", "PST8PDT,M3.2.0,M11.1.0"},
    {"America/Mexico_City", "CST6"},
    // Asia / Pacific
    {"Asia/Tokyo", "JST-9"},
    {"Asia/Shanghai", "CST-8"},
    {"Asia/Hong_Kong", "HKT-8"},
    {"Asia/Singapore", "SGT-8"},
    {"Asia/Kolkata", "IST-5:30"},
    {"Asia/Dubai", "GST-4"},
    {"Asia/Seoul", "KST-9"},
    {"Asia/Bangkok", "ICT-7"},
    {"Asia/Taipei", "CST-8"},
    // Oceania
    {"Australia/Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Australia/Melbourne", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Australia/Perth", "AWST-8"},
    {"Australia/Adelaide", "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
    {"Pacific/Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
    {"Pacific/Honolulu", "HST10"},
    // Africa
    {"Africa/Cairo", "EET-2EEST,M4.5.5/0,M10.5.4/24"},
    {"Africa/Johannesburg", "SAST-2"},
    {"Africa/Lagos", "WAT-1"},
};

static const char *lookupPosixTz(const char *iana)
{
    for (size_t i = 0; i < sizeof(tzLookup) / sizeof(tzLookup[0]); i++) {
        if (strcmp(iana, tzLookup[i].iana) == 0)
            return tzLookup[i].posix;
    }
    return nullptr;
}

// Build a basic POSIX TZ string from a UTC offset like "+02:00" or "-05:30"
// No DST rules, but better than GMT0
static void posixFromUtcOffset(const char *offset, char *out, size_t outLen)
{
    int sign = 1, hours = 0, minutes = 0;
    const char *p = offset;
    if (*p == '-') {
        sign = -1;
        p++;
    } else if (*p == '+') {
        p++;
    }
    hours = atoi(p);
    const char *colon = strchr(p, ':');
    if (colon)
        minutes = atoi(colon + 1);

    // POSIX TZ sign is inverted: UTC+2 → "UTC-2"
    int posixSign = -sign;
    if (minutes > 0)
        snprintf(out, outLen, "UTC%c%d:%02d", posixSign > 0 ? '+' : '-', hours, minutes);
    else
        snprintf(out, outLen, "UTC%c%d", posixSign > 0 ? '+' : '-', hours);

    // Handle UTC+0 edge case
    if (hours == 0 && minutes == 0)
        strncpy(out, "UTC0", outLen);
}

static void copyTzDef(char *out, size_t outLen, const char *tz)
{
    if (!out || outLen == 0) return;
    strncpy(out, tz ? tz : "", outLen - 1);
    out[outLen - 1] = '\0';
}

static uint8_t tzRetryCount = 0;
static const uint8_t TZ_MAX_RETRIES = 5;

// Try to extract IANA timezone from a JSON response (works for worldtimeapi and ip-api)
static bool parseTimezoneFromJson(const String &payload, char *out, size_t outLen)
{
    // Try "timezone":"Region/City" (worldtimeapi format)
    int tzIdx = payload.indexOf("\"timezone\":\"");
    if (tzIdx < 0) {
        // Try "tz":"Region/City" (ip-api.com format when fields=tz)
        tzIdx = payload.indexOf("\"tz\":\"");
        if (tzIdx >= 0)
            tzIdx += 6;
    } else {
        tzIdx += 12;
    }

    if (tzIdx < 0)
        return false;

    int tzEnd = payload.indexOf('"', tzIdx);
    if (tzEnd <= tzIdx)
        return false;

    String iana = payload.substring(tzIdx, tzEnd);
    const char *posix = lookupPosixTz(iana.c_str());

    if (posix) {
        LOG_INFO("Auto-TZ: %s -> %s", iana.c_str(), posix);
        copyTzDef(out, outLen, posix);
        return true;
    }

    // Fallback: parse "utc_offset" (worldtimeapi) or "offset" (ip-api)
    int offIdx = payload.indexOf("\"utc_offset\":\"");
    if (offIdx >= 0) {
        offIdx += 14;
        int offEnd = payload.indexOf('"', offIdx);
        if (offEnd > offIdx) {
            String utcOff = payload.substring(offIdx, offEnd);
            char posixBuf[16];
            posixFromUtcOffset(utcOff.c_str(), posixBuf, sizeof(posixBuf));
            LOG_WARN("Auto-TZ: %s not in lookup, using offset %s -> %s", iana.c_str(), utcOff.c_str(), posixBuf);
            copyTzDef(out, outLen, posixBuf);
            return true;
        }
    }

    // Last resort: parse numeric "offset":7200 (ip-api format, seconds from UTC)
    int numOffIdx = payload.indexOf("\"offset\":");
    if (numOffIdx >= 0) {
        numOffIdx += 9; // skip past "offset":
        int offsetSec = payload.substring(numOffIdx).toInt();
        if (offsetSec != 0 || payload.charAt(numOffIdx) == '0') {
            int hours = offsetSec / 3600;
            int mins = abs((offsetSec % 3600) / 60);
            char posixBuf[16];
            // POSIX sign is inverted: UTC+2 (offset=7200) → "UTC-2"
            if (mins > 0)
                snprintf(posixBuf, sizeof(posixBuf), "UTC%+d:%02d", -hours, mins);
            else
                snprintf(posixBuf, sizeof(posixBuf), "UTC%+d", -hours);
            if (offsetSec == 0)
                strncpy(posixBuf, "UTC0", sizeof(posixBuf));
            LOG_WARN("Auto-TZ: using numeric offset %d -> %s", offsetSec, posixBuf);
            copyTzDef(out, outLen, posixBuf);
            return true;
        }
    }

    LOG_WARN("Auto-TZ: could not parse timezone from response");
    return false;
}

static void autoDetectTimezoneTask(void *)
{
    char detected[sizeof(config.device.tzdef)] = {};
    bool success = false;

    const char *apis[] = {
        "http://worldtimeapi.org/api/ip",
        "http://ip-api.com/json/?fields=status,timezone,offset"
    };

    for (int i = 0; i < 2 && !success; i++) {
        HTTPClient http;
        http.setConnectTimeout(4000);
        http.setTimeout(4000);
        http.begin(apis[i]);
        int httpCode = http.GET();

        if (httpCode == 200) {
            String payload = http.getString();
            LOG_DEBUG("Auto-TZ API %d response: %s", i, payload.c_str());
            success = parseTimezoneFromJson(payload, detected, sizeof(detected));
        } else {
            LOG_WARN("Auto-TZ API %d failed: %d", i, httpCode);
        }
        http.end();
    }

    if (success)
        copyTzDef(tzDetectedDef, sizeof(tzDetectedDef), detected);
    tzResultSuccess = success;
    tzResultReady = true;
    tzDetectInFlight = false;
    vTaskDelete(nullptr);
}

// Returns true while a worker is in flight, so reconnectWiFi can poll soon.
static bool autoDetectTimezone()
{
    if (tzAutoDetected || !WiFi.isConnected())
        return false;

    if (tzResultReady) {
        tzResultReady = false;
        if (tzResultSuccess) {
            copyTzDef(config.device.tzdef, sizeof(config.device.tzdef), tzDetectedDef);
            setenv("TZ", config.device.tzdef, 1);
            tzset();

            time_t now;
            time(&now);
            struct tm local;
            localtime_r(&now, &local);
            LOG_INFO("Timezone set to %s (localtime: %02d:%02d:%02d)", config.device.tzdef, local.tm_hour, local.tm_min,
                     local.tm_sec);
            tzAutoDetected = true;

            // Persist on the main loop. The HTTP worker never touches NodeDB.
            if (nodeDB) nodeDB->saveToDisk(SEGMENT_CONFIG);
        } else {
            tzRetryCount++;
            if (tzRetryCount >= TZ_MAX_RETRIES) {
                LOG_WARN("Auto-TZ: giving up after %d retries", TZ_MAX_RETRIES);
                tzAutoDetected = true;
            }
        }
        return false;
    }

    if (tzDetectInFlight)
        return true;

    // Skip if user already has a real timezone configured (set via phone/menu)
    // "GMT0" and "UTC0" are boot fallbacks, not deliberate user choices — still auto-detect
    bool isDefault = (config.device.tzdef[0] == 0) ||
                     (strcmp(config.device.tzdef, "GMT0") == 0) ||
                     (strcmp(config.device.tzdef, "UTC0") == 0);
    if (!isDefault) {
        LOG_INFO("Auto-TZ: skipped, tzdef already set to %s", config.device.tzdef);
        tzAutoDetected = true;
        return false;
    }

    LOG_INFO("Auto-TZ: attempting detection (try %d/%d)", tzRetryCount + 1, TZ_MAX_RETRIES);

    tzDetectInFlight = true;
    tzResultReady = false;
    tzResultSuccess = false;
    BaseType_t ok = xTaskCreate(autoDetectTimezoneTask, "AutoTZ", 6144, nullptr, 1, nullptr);
    if (ok != pdPASS) {
        LOG_WARN("Auto-TZ: failed to start worker task");
        tzDetectInFlight = false;
        tzRetryCount++;
        if (tzRetryCount >= TZ_MAX_RETRIES) {
            LOG_WARN("Auto-TZ: giving up after %d retries", TZ_MAX_RETRIES);
            tzAutoDetected = true;
        }
        return false;
    }
    return true;
}
#endif // !MESHTASTIC_EXCLUDE_TZ && ARCH_ESP32

WiFiUDP syslogClient;
meshtastic::Syslog syslog(syslogClient);

Periodic *wifiReconnect;

#ifdef USE_WS5500
// Startup Ethernet
bool initEthernet()
{
    if ((config.network.eth_enabled) && (ETH.begin(ETH_PHY_W5500, 1, ETH_CS_PIN, ETH_INT_PIN, ETH_RST_PIN, SPI3_HOST,
                                                   ETH_SCLK_PIN, ETH_MISO_PIN, ETH_MOSI_PIN))) {
        WiFi.onEvent(WiFiEvent);
#if !MESHTASTIC_EXCLUDE_WEBSERVER
        createSSLCert(); // For WebServer
#endif
        return true;
    }

    return false;
}
#endif

static void onNetworkConnected()
{
    if (!APStartupComplete) {
        // Start web server
        LOG_INFO("Start network services");

        if (kCrowpanelMinimalWifiServices) {
#ifndef DISABLE_NTP
#if defined(ARCH_ESP32P4) && defined(CROWPANEL_DHE04005D)
            s_crowpanelWifiConnectedAtMs = millis();
            s_ntpClientStarted = false;
#endif
#endif
        } else {
        // start mdns
        if (!MDNS.begin("Meshtastic")) {
            LOG_ERROR("Error setting up mDNS responder!");
        } else {
            LOG_INFO("mDNS Host: Meshtastic.local");
            MDNS.addService("meshtastic", "tcp", SERVER_API_DEFAULT_PORT);
// ESPmDNS (ESP32) and SimpleMDNS (RP2040) have slightly different APIs for adding TXT records
#ifdef ARCH_ESP32
            MDNS.addServiceTxt("meshtastic", "tcp", "shortname", String(owner.short_name));
            MDNS.addServiceTxt("meshtastic", "tcp", "id", String(nodeDB->getNodeId().c_str()));
            MDNS.addServiceTxt("meshtastic", "tcp", "pio_env", optstr(APP_ENV));
            // ESP32 prints obtained IP address in WiFiEvent
#elif defined(ARCH_RP2040)
            MDNS.addServiceTxt("meshtastic", "shortname", owner.short_name);
            MDNS.addServiceTxt("meshtastic", "id", nodeDB->getNodeId().c_str());
            MDNS.addServiceTxt("meshtastic", "pio_env", optstr(APP_ENV));
            LOG_INFO("Obtained IP address: %s", WiFi.localIP().toString().c_str());
#endif
        }

#ifndef DISABLE_NTP
        timeClient.begin();
        timeClient.setUpdateInterval(60 * 60); // Update once an hour
        s_ntpClientStarted = true;
#endif

        if (config.network.rsyslog_server[0]) {
            LOG_INFO("Start Syslog client");
            // Defaults
            int serverPort = 514;
            const char *serverAddr = config.network.rsyslog_server;
            String server = String(serverAddr);
            int delimIndex = server.indexOf(':');
            if (delimIndex > 0) {
                String port = server.substring(delimIndex + 1, server.length());
                server[delimIndex] = 0;
                serverPort = port.toInt();
                serverAddr = server.c_str();
            }
            syslog.server(serverAddr, serverPort);
            syslog.deviceHostname(getDeviceName());
            syslog.appName("Meshtastic");
            syslog.defaultPriority(LOGLEVEL_USER);
            syslog.enable();
        }

#if defined(ARCH_ESP32) && !MESHTASTIC_EXCLUDE_WEBSERVER
        initWebServer();
#endif
        }
#if !MESHTASTIC_EXCLUDE_SOCKETAPI
        // Stock Meshtastic skips the TCP API server on COLOR displays. On
        // this build (CROWPANEL_DHE04005D) the on-device mcui Settings tab
        // owns all configuration and there is no on-device GPS to forward,
        // so we deliberately keep stock behaviour: no API server, no phone
        // app TCP connection, no exposure to the lwip-via-PSRAM heap-
        // corruption crash that occurs when a phone sends the first
        // protobuf. Position is set manually from mcui Settings instead.
        if (config.display.displaymode != meshtastic_Config_DisplayConfig_DisplayMode_COLOR) {
            initApiServer();
        }
#endif
        APStartupComplete = true;
    }

#if HAS_UDP_MULTICAST
    if (udpHandler && config.network.enabled_protocols & meshtastic_Config_NetworkConfig_ProtocolFlags_UDP_BROADCAST) {
        udpHandler->start();
    }
#endif
}

static int32_t reconnectWiFi()
{
    const char *wifiName = config.network.wifi_ssid;
    const char *wifiPsw = config.network.wifi_psk;
    const uint32_t nowMs = millis();

    if (config.network.wifi_enabled && needReconnect) {

        if (!*wifiPsw) // Treat empty password as no password
            wifiPsw = NULL;

        needReconnect = false;
        isReconnecting = true;

        // Make sure we clear old connection credentials
#ifdef ARCH_ESP32
        WiFi.disconnect(false, true);
#elif defined(ARCH_RP2040)
        WiFi.disconnect(false);
#endif
        LOG_INFO("Reconnecting to WiFi access point %s", wifiName);

        // Start the non-blocking wait for 5 seconds
        wifiReconnectStartMillis = nowMs;
        wifiReconnectPending = true;
        // Do not attempt to connect yet, wait for the next invocation
        return 5000; // Schedule next check soon
    }

    // Check if we are ready to proceed with the WiFi connection after the 5s wait
    if (wifiReconnectPending) {
        const bool wifiConnected = WiFi.isConnected();
        if (nowMs - wifiReconnectStartMillis >= 5000) {
            if (!wifiConnected) {
#ifdef CONFIG_IDF_TARGET_ESP32C3
                WiFi.mode(WIFI_MODE_NULL);
                WiFi.useStaticBuffers(true);
                WiFi.mode(WIFI_STA);
#endif
                WiFi.begin(wifiName, wifiPsw);
            }
            isReconnecting = false;
            wifiReconnectPending = false;
        } else {
            // Still waiting for 5s to elapse
            return kReconnectPollFastMs; // Check again soon
        }
    }

    const bool wifiConnected = WiFi.isConnected();

#ifndef DISABLE_NTP
    if (!wifiConnected) {
        s_ntpClientStarted = false;
#if defined(ARCH_ESP32P4) && defined(CROWPANEL_DHE04005D)
        s_crowpanelWifiConnectedAtMs = 0;
#endif
    } else if (kCrowpanelMinimalWifiServices && !s_ntpClientStarted) {
#if defined(ARCH_ESP32P4) && defined(CROWPANEL_DHE04005D)
        if (s_crowpanelWifiConnectedAtMs == 0) {
            s_crowpanelWifiConnectedAtMs = nowMs;
        }
        if ((nowMs - s_crowpanelWifiConnectedAtMs) >= kCrowpanelNtpStartDelayMs) {
            timeClient.begin();
            timeClient.setUpdateInterval(60 * 60); // Update once an hour
            s_ntpClientStarted = true;
        } else {
            // Keep polling frequently until delayed NTP start window has elapsed.
            return kReconnectPollNormalMs;
        }
#endif
    }

    // NTP re-sync policy:
    //  - Default Meshtastic behaviour is to re-sync every 12 hours while
    //    WiFi is connected.
    //  - With mcui's on-board RTC (DS3231 on the DHE04005D, battery-backed
    //    and TCXO-stabilised) we only need to sync ONCE — the chip holds
    //    the time across reboots, so subsequent boots restore time from
    //    the chip without touching the internet. Once the chip has been
    //    written (mcclock_save fired below on the first successful sync)
    //    we skip all further NTP requests in this boot.
    bool skip_ntp = false;
#if HAS_TFT && USE_MCUI
    if (mcui::mcclock_has_rtc() && mcui::mcclock_has_valid_time() && lastrun_ntp != 0) {
        skip_ntp = true;
    }
#endif
    if (s_ntpClientStarted && !skip_ntp && wifiConnected &&
        (!Throttle::isWithinTimespanMs(lastrun_ntp, 43200000) || (lastrun_ntp == 0))) { // every 12 hours
        if (timeClient.update()) {
            struct timeval tv;
            tv.tv_sec = timeClient.getEpochTime();
            tv.tv_usec = 0;

            perhapsSetRTC(RTCQualityNTP, &tv);
            lastrun_ntp = millis();

#if HAS_TFT && USE_MCUI
            // Persist to the on-board RTC (DS3231) so next boot restores
            // from the chip without needing WiFi / NTP at all.
            mcui::mcclock_save((uint32_t)tv.tv_sec);
#endif
        }
    }
#endif

    bool tz_pending = false;
#if !MESHTASTIC_EXCLUDE_TZ && defined(ARCH_ESP32)
    // Auto-detect timezone from IP geolocation (runs independently, retries on failure)
    if (wifiConnected)
        tz_pending = autoDetectTimezone();
#endif

    if (config.network.wifi_enabled && !wifiConnected) {
#ifdef ARCH_RP2040 // (ESP32 handles this in WiFiEvent)
        needReconnect = APStartupComplete;
#endif
        return kReconnectPollNormalMs; // check once per second
    } else {
#ifdef ARCH_RP2040
        onNetworkConnected(); // will only do anything once
#endif
        return tz_pending ? kReconnectPollNormalMs : kReconnectIdleMs; // every 5 minutes, or poll the Auto-TZ worker
    }
}

bool isWifiAvailable()
{

    if (config.network.wifi_enabled && (config.network.wifi_ssid[0])) {
        return true;
#ifdef USE_WS5500
    } else if (config.network.eth_enabled) {
        return true;
#endif
#ifndef ARCH_PORTDUINO
    } else if (WiFi.status() == WL_CONNECTED) {
        // it's likely we have wifi now, but user intends to turn it off in config!
        return true;
#endif
    } else {
        return false;
    }
}

// Disable WiFi
void deinitWifi()
{
    LOG_INFO("WiFi deinit");

    if (isWifiAvailable()) {
#ifdef ARCH_ESP32
        WiFi.disconnect(true, false);
#elif defined(ARCH_RP2040)
        WiFi.disconnect(true);
#endif
        WiFi.mode(WIFI_OFF);
        LOG_INFO("WiFi Turned Off");
        // WiFi.printDiag(Serial);
    }
}

// Startup WiFi
bool initWifi()
{
    if (config.network.wifi_enabled && config.network.wifi_ssid[0]) {

        const char *wifiName = config.network.wifi_ssid;
        const char *wifiPsw = config.network.wifi_psk;

#ifndef ARCH_RP2040
#if !MESHTASTIC_EXCLUDE_WEBSERVER
        createSSLCert(); // For WebServer
#endif
        WiFi.persistent(false); // Disable flash storage for WiFi credentials
#endif
        if (!*wifiPsw) // Treat empty password as no password
            wifiPsw = NULL;

        if (*wifiName) {
            uint8_t dmac[6];
            getMacAddr(dmac);
            snprintf(ourHost, sizeof(ourHost), "Meshtastic-%02x%02x", dmac[4], dmac[5]);

#if defined(ARCH_ESP32P4) && defined(CROWPANEL_DHE04005D)
            // IMPORTANT: pins are already configured during initVariant() prewarm.
            // Calling WiFi.setPins() again after ESP-Hosted is initialized logs an
            // error and can destabilize hosted state on this target.
#endif
            if (WiFi.getMode() == WIFI_MODE_NULL) {
#if defined(ARCH_ESP32P4) && defined(CROWPANEL_DHE04005D)
                // Prefer static WiFi buffers on ESP-Hosted CrowPanel to reduce
                // allocator churn and lower risk of TLSF corruption during
                // first TCP socket allocations (MQTT connect path).
                WiFi.useStaticBuffers(true);
#endif
                WiFi.mode(WIFI_STA);
            }
            WiFi.setHostname(ourHost);

            if (config.network.address_mode == meshtastic_Config_NetworkConfig_AddressMode_STATIC &&
                config.network.ipv4_config.ip != 0) {
#ifdef ARCH_ESP32
                WiFi.config(config.network.ipv4_config.ip, config.network.ipv4_config.gateway, config.network.ipv4_config.subnet,
                            config.network.ipv4_config.dns);
#elif defined(ARCH_RP2040)
                WiFi.config(config.network.ipv4_config.ip, config.network.ipv4_config.dns, config.network.ipv4_config.gateway,
                            config.network.ipv4_config.subnet);
#endif
            }
#ifdef ARCH_ESP32
            if (!s_wifiEventHandlersRegistered) {
                WiFi.onEvent(WiFiEvent);
                WiFi.onEvent(
                    [](WiFiEvent_t event, WiFiEventInfo_t info) {
                        LOG_WARN("WiFi lost connection. Reason: %d", info.wifi_sta_disconnected.reason);

                        /*
                            If we are disconnected from the AP for some reason,
                            save the error code.

                            For a reference to the codes:
                                https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/wifi.html#wi-fi-reason-code
                        */
                        wifiDisconnectReason = info.wifi_sta_disconnected.reason;
                    },
                    WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
                s_wifiEventHandlersRegistered = true;
            }
            WiFi.setAutoReconnect(true);
            WiFi.setSleep(false);

            // This is needed to improve performance.
            esp_wifi_set_ps(WIFI_PS_NONE); // Disable radio power saving
#endif
            LOG_DEBUG("JOINING WIFI soon: ssid=%s", wifiName);
            if (!wifiReconnect) {
                wifiReconnect = new Periodic("WifiConnect", reconnectWiFi);
            } else {
                wifiReconnect->setIntervalFromNow(0);
            }
        }
        return true;
    } else {
        LOG_INFO("Not using WIFI");
        return false;
    }
}

#if HAS_WIFI && !defined(ARCH_PORTDUINO)
bool wifiConnectNowBlocking(uint32_t timeoutMs)
{
    if (!(config.network.wifi_enabled && config.network.wifi_ssid[0])) {
        return false;
    }

    const char *wifiName = config.network.wifi_ssid;
    const char *wifiPsw = config.network.wifi_psk;
    if (!wifiPsw || wifiPsw[0] == '\0') {
        wifiPsw = nullptr;
    }

    WiFi.disconnect(false, true);
    delay(50);
    LOG_INFO("CrowPanel early WiFi connect to %s", wifiName);
    WiFi.begin(wifiName, wifiPsw);

    const uint32_t startMs = millis();
    while (!WiFi.isConnected() && (millis() - startMs) < timeoutMs) {
        delay(100);
    }

    const bool connected = WiFi.isConnected();
#if defined(ARCH_ESP32P4) && defined(CROWPANEL_DHE04005D)
    if (connected) {
        // This link was established pre-framebuffer, on the clean PSRAM
        // heap (and the MQTT prewarm holds a socket on it). reconnectWiFi()
        // starts with needReconnect=true and its first pass would otherwise
        // WiFi.disconnect() this healthy link (ASSOC_LEAVE) and rebuild it
        // AFTER esp_lcd_new_rgb_panel() corrupts the heap -> block_locate_free.
        // Clear the flag so the state machine leaves the live connection
        // alone; a genuine later drop still re-arms it via STA_DISCONNECTED.
        needReconnect = false;
    }
#endif
    LOG_INFO("CrowPanel early WiFi connect result: %s", connected ? "connected" : "timeout");
    return connected;
}
#endif

#ifdef ARCH_ESP32
#if ESP_ARDUINO_VERSION <= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
// Most of the next 12 lines of code are adapted from espressif/arduino-esp32
// Licensed under the GNU Lesser General Public License v2.1
// https://github.com/espressif/arduino-esp32/blob/1f038677eb2eaf5e9ca6b6074486803c15468bed/libraries/WiFi/src/WiFiSTA.cpp#L755
esp_netif_t *get_esp_interface_netif(esp_interface_t interface);
IPv6Address GlobalIPv6()
{
    esp_ip6_addr_t addr;
    if (WiFiGenericClass::getMode() == WIFI_MODE_NULL) {
        return IPv6Address();
    }
    if (esp_netif_get_ip6_global(get_esp_interface_netif(ESP_IF_WIFI_STA), &addr)) {
        return IPv6Address();
    }
    return IPv6Address(addr.addr);
}
#endif
// Called by the Espressif SDK to
static void WiFiEvent(WiFiEvent_t event)
{
    switch (event) {
    case ARDUINO_EVENT_WIFI_READY:
        LOG_INFO("WiFi interface ready");
        break;
    case ARDUINO_EVENT_WIFI_SCAN_DONE:
        LOG_INFO("Completed scan for access points");
        break;
    case ARDUINO_EVENT_WIFI_STA_START:
        LOG_INFO("WiFi station started");
        break;
    case ARDUINO_EVENT_WIFI_STA_STOP:
        LOG_INFO("WiFi station stopped");
        syslog.disable();
        break;
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
        LOG_INFO("Connected to access point");
        if (config.network.ipv6_enabled) {
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
            if (!WiFi.enableIPv6()) {
                LOG_WARN("Failed to enable IPv6");
            }
#else
            if (!WiFi.enableIpV6()) {
                LOG_WARN("Failed to enable IPv6");
            }
#endif
        }
#ifdef WIFI_LED
        digitalWrite(WIFI_LED, HIGH);
#endif
        break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
        LOG_INFO("Disconnected from WiFi access point");
#ifdef WIFI_LED
        digitalWrite(WIFI_LED, LOW);
#endif
#if HAS_UDP_MULTICAST
        if (udpHandler) {
            udpHandler->stop();
        }
#endif
        if (!isReconnecting) {
            WiFi.disconnect(false, true);
            syslog.disable();
            needReconnect = true;
            if (wifiReconnect) {
                wifiReconnect->setIntervalFromNow(kReconnectPollNormalMs);
            }
        }
        break;
    case ARDUINO_EVENT_WIFI_STA_AUTHMODE_CHANGE:
        LOG_INFO("Authentication mode of access point has changed");
        break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        LOG_INFO("Obtained IP address: %s", WiFi.localIP().toString().c_str());
        onNetworkConnected();
        break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP6:
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
        LOG_INFO("Obtained Local IP6 address: %s", WiFi.linkLocalIPv6().toString().c_str());
        LOG_INFO("Obtained GlobalIP6 address: %s", WiFi.globalIPv6().toString().c_str());
#else
        LOG_INFO("Obtained Local IP6 address: %s", WiFi.localIPv6().toString().c_str());
        LOG_INFO("Obtained GlobalIP6 address: %s", GlobalIPv6().toString().c_str());
#endif
        break;
    case ARDUINO_EVENT_WIFI_STA_LOST_IP:
        LOG_INFO("Lost IP address and IP address is reset to 0");
#if HAS_UDP_MULTICAST
        if (udpHandler) {
            udpHandler->stop();
        }
#endif
        if (!isReconnecting) {
            WiFi.disconnect(false, true);
            syslog.disable();
            needReconnect = true;
            if (wifiReconnect) {
                wifiReconnect->setIntervalFromNow(kReconnectPollNormalMs);
            }
        }
        break;
    case ARDUINO_EVENT_WPS_ER_SUCCESS:
        LOG_INFO("WiFi Protected Setup (WPS): succeeded in enrollee mode");
        break;
    case ARDUINO_EVENT_WPS_ER_FAILED:
        LOG_INFO("WiFi Protected Setup (WPS): failed in enrollee mode");
        break;
    case ARDUINO_EVENT_WPS_ER_TIMEOUT:
        LOG_INFO("WiFi Protected Setup (WPS): timeout in enrollee mode");
        break;
    case ARDUINO_EVENT_WPS_ER_PIN:
        LOG_INFO("WiFi Protected Setup (WPS): pin code in enrollee mode");
        break;
    case ARDUINO_EVENT_WPS_ER_PBC_OVERLAP:
        LOG_INFO("WiFi Protected Setup (WPS): push button overlap in enrollee mode");
        break;
    case ARDUINO_EVENT_WIFI_AP_START:
        LOG_INFO("WiFi access point started");
#ifdef WIFI_LED
        digitalWrite(WIFI_LED, HIGH);
#endif
        break;
    case ARDUINO_EVENT_WIFI_AP_STOP:
        LOG_INFO("WiFi access point stopped");
#ifdef WIFI_LED
        digitalWrite(WIFI_LED, LOW);
#endif
        break;
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
        LOG_INFO("Client connected");
        break;
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
        LOG_INFO("Client disconnected");
        break;
    case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED:
        LOG_INFO("Assigned IP address to client");
        break;
    case ARDUINO_EVENT_WIFI_AP_PROBEREQRECVED:
        LOG_INFO("Received probe request");
        break;
    case ARDUINO_EVENT_WIFI_AP_GOT_IP6:
        LOG_INFO("IPv6 is preferred");
        break;
    case ARDUINO_EVENT_WIFI_FTM_REPORT:
        LOG_INFO("Fast Transition Management report");
        break;
    case ARDUINO_EVENT_ETH_START:
        LOG_INFO("Ethernet started");
        break;
    case ARDUINO_EVENT_ETH_STOP:
        syslog.disable();
        LOG_INFO("Ethernet stopped");
        break;
    case ARDUINO_EVENT_ETH_CONNECTED:
        LOG_INFO("Ethernet connected");
        break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
        syslog.disable();
        LOG_INFO("Ethernet disconnected");
        break;
    case ARDUINO_EVENT_ETH_GOT_IP:
#ifdef USE_WS5500
        LOG_INFO("Obtained IP address: %s, %u Mbps, %s", ETH.localIP().toString().c_str(), ETH.linkSpeed(),
                 ETH.fullDuplex() ? "FULL_DUPLEX" : "HALF_DUPLEX");
        onNetworkConnected();
#endif
        break;
    case ARDUINO_EVENT_ETH_GOT_IP6:
#ifdef USE_WS5500
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
        LOG_INFO("Obtained Local IP6 address: %s", ETH.linkLocalIPv6().toString().c_str());
        LOG_INFO("Obtained GlobalIP6 address: %s", ETH.globalIPv6().toString().c_str());
#else
        LOG_INFO("Obtained IP6 address: %s", ETH.localIPv6().toString().c_str());
#endif
#endif
        break;
    case ARDUINO_EVENT_SC_SCAN_DONE:
        LOG_INFO("SmartConfig: Scan done");
        break;
    case ARDUINO_EVENT_SC_FOUND_CHANNEL:
        LOG_INFO("SmartConfig: Found channel");
        break;
    case ARDUINO_EVENT_SC_GOT_SSID_PSWD:
        LOG_INFO("SmartConfig: Got SSID and password");
        break;
    case ARDUINO_EVENT_SC_SEND_ACK_DONE:
        LOG_INFO("SmartConfig: Send ACK done");
        break;
    case ARDUINO_EVENT_PROV_INIT:
        LOG_INFO("Provision Init");
        break;
    case ARDUINO_EVENT_PROV_DEINIT:
        LOG_INFO("Provision Stopped");
        break;
    case ARDUINO_EVENT_PROV_START:
        LOG_INFO("Provision Started");
        break;
    case ARDUINO_EVENT_PROV_END:
        LOG_INFO("Provision End");
        break;
    case ARDUINO_EVENT_PROV_CRED_RECV:
        LOG_INFO("Provision Credentials received");
        break;
    case ARDUINO_EVENT_PROV_CRED_FAIL:
        LOG_INFO("Provision Credentials failed");
        break;
    case ARDUINO_EVENT_PROV_CRED_SUCCESS:
        LOG_INFO("Provision Credentials success");
        break;
    default:
        break;
    }
}
#endif

uint8_t getWifiDisconnectReason()
{
    return wifiDisconnectReason;
}
#endif // HAS_WIFI
