#if HAS_TFT && USE_MCUI

#include "McScreens.h"
#include "../McKeyboard.h"
#include "../McTheme.h"
#include "../McUI.h"
#include "../data/McNodeActions.h"

#include "configuration.h"
#include "crowpanel_backlight.h"
#include "main.h"
#include "memGet.h"
#include "concurrency/OSThread.h"
#include "mesh/NodeDB.h"
#include "mesh/Channels.h"
#include "mesh/CryptoEngine.h"
#include "mesh/Default.h"
#include "mesh/MeshService.h"
#include "mesh/RadioInterface.h"
#include "mesh/Router.h"
#include "mesh/generated/meshtastic/config.pb.h"
#include "mesh/generated/meshtastic/module_config.pb.h"
#include "mesh/TypeConversions.h"
#include "modules/PositionModule.h"
#include "gps/RTC.h"

#include "PowerStatus.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace mcui {

using namespace meshtastic;

static volatile bool     s_config_dirty           = false;
static volatile bool     s_config_save_only_dirty = false;
static volatile bool     s_orientation_dirty      = false;
static volatile bool     s_orientation_force_apply = false;
static volatile bool     s_factory_reset_req      = false;
static volatile bool     s_pending_landscape      = false;
static volatile uint32_t s_last_change_ms         = 0;
static constexpr uint32_t APPLY_DEBOUNCE_MS       = 5000;

static lv_obj_t *s_lbl_pending = nullptr;

static void cfg_show_pending_banner()
{
    if (s_lbl_pending) {
        lv_label_set_text(s_lbl_pending, "Changes pending — saving in 5 s...");
        lv_obj_remove_flag(s_lbl_pending, LV_OBJ_FLAG_HIDDEN);
    }
}

static void cfg_mark_dirty()
{
    s_config_dirty   = true;
    s_last_change_ms = millis();
    cfg_show_pending_banner();
}

static void cfg_mark_save_only()
{
    s_config_save_only_dirty = true;
    s_last_change_ms = millis();
    cfg_show_pending_banner();
}

static void cfg_mark_orientation(bool landscape)
{
    if (!s_orientation_dirty && landscape == landscape_active())
        return;
    s_pending_landscape = landscape;
    s_orientation_dirty = (landscape != landscape_active());
    s_last_change_ms = millis();
    if (s_orientation_dirty)
        cfg_show_pending_banner();
}

static bool cfg_has_pending()
{
    return s_config_dirty || s_config_save_only_dirty || s_orientation_dirty;
}

class McConfigApplyThread : public concurrency::OSThread
{
  public:
    McConfigApplyThread() : concurrency::OSThread("McCfg") {}

  protected:
    int32_t runOnce() override
    {
        if (s_factory_reset_req) {
            s_factory_reset_req = false;
            LOG_WARN("mcui: applying factory reset on main loop");
            if (nodeDB) nodeDB->factoryReset();
            rebootAtMsec = millis() + 1500;
            return 500;
        }
        bool force_apply = s_orientation_force_apply;
        if (!cfg_has_pending()) {
            s_orientation_force_apply = false;
            return 500;
        }
        uint32_t now = millis();
        if (!force_apply && now - s_last_change_ms < APPLY_DEBOUNCE_MS) return 500;

        bool reboot_required = s_config_dirty;
        bool save_only = s_config_save_only_dirty;
        bool orientation_change = s_orientation_dirty;
        bool target_landscape = s_pending_landscape;
        s_orientation_force_apply = false;
        s_config_dirty = false;
        s_config_save_only_dirty = false;
        s_orientation_dirty = false;
        LOG_INFO("mcui: applying pending config (debounce elapsed, cfg_reboot=%d orientation=%d)",
                 reboot_required ? 1 : 0, orientation_change ? 1 : 0);
        if ((reboot_required || save_only) && nodeDB)
            nodeDB->saveToDisk(SEGMENT_CONFIG);
        if (orientation_change && !orientation_save(target_landscape)) {
            LOG_WARN("mcui: orientation save failed, retrying later");
            s_pending_landscape = target_landscape;
            s_orientation_dirty = true;
            s_last_change_ms = millis();
            return 1000;
        }
        if (reboot_required) {
            if (service) service->reloadConfig(SEGMENT_CONFIG);
        }
        if (reboot_required || orientation_change) {
            rebootAtMsec = millis() + 1500;
        }
        return 500;
    }
};

static McConfigApplyThread *s_cfg_thread = nullptr;
static void ensure_cfg_thread()
{
    if (!s_cfg_thread) s_cfg_thread = new McConfigApplyThread();
}

struct PendingOwner {
    bool long_set;
    bool short_set;
    char long_name[40];
    char short_name[5];
};

static volatile bool s_owner_req        = false;
static volatile bool s_regen_keys_req   = false;
static PendingOwner  s_pending_owner    = {};
static SemaphoreHandle_t s_aux_lock     = nullptr;

struct PendingPosition {
    bool   set;
    double lat_deg;
    double lon_deg;
    bool   has_alt;
    int32_t alt_m;
};
static volatile bool s_position_req = false;
static PendingPosition s_pending_position = {};

struct ScannedNetwork {
    char     ssid[33];
    int8_t   rssi;
    bool     encrypted;
};
static constexpr int SCAN_MAX = 20;
enum ScanState : uint8_t { SCAN_IDLE = 0, SCAN_REQUESTED, SCAN_RUNNING, SCAN_DONE, SCAN_FAIL };
static volatile ScanState s_scan_state  = SCAN_IDLE;
static ScannedNetwork s_scan_results[SCAN_MAX];
static int s_scan_count = 0;

static void aux_lock_init()
{
    if (!s_aux_lock) s_aux_lock = xSemaphoreCreateMutex();
}

static void tz_apply_to_libc();

static void queue_owner_edit(const char *longn, const char *shortn)
{
    aux_lock_init();
    xSemaphoreTake(s_aux_lock, portMAX_DELAY);
    if (longn) {
        s_pending_owner.long_set = true;
        strncpy(s_pending_owner.long_name, longn, sizeof(s_pending_owner.long_name) - 1);
        s_pending_owner.long_name[sizeof(s_pending_owner.long_name) - 1] = '\0';
    }
    if (shortn) {
        s_pending_owner.short_set = true;
        strncpy(s_pending_owner.short_name, shortn, sizeof(s_pending_owner.short_name) - 1);
        s_pending_owner.short_name[sizeof(s_pending_owner.short_name) - 1] = '\0';
    }
    s_owner_req = true;
    xSemaphoreGive(s_aux_lock);
}

static void queue_regenerate_private_keys()
{
    aux_lock_init();
    xSemaphoreTake(s_aux_lock, portMAX_DELAY);
    s_regen_keys_req = true;
    xSemaphoreGive(s_aux_lock);
}

static void queue_position_apply(double lat_deg, double lon_deg,
                                 bool has_alt, int32_t alt_m)
{
    aux_lock_init();
    xSemaphoreTake(s_aux_lock, portMAX_DELAY);
    s_pending_position.set     = true;
    s_pending_position.lat_deg = lat_deg;
    s_pending_position.lon_deg = lon_deg;
    s_pending_position.has_alt = has_alt;
    s_pending_position.alt_m   = alt_m;
    s_position_req = true;
    xSemaphoreGive(s_aux_lock);
}
static void queue_position_clear()
{
    aux_lock_init();
    xSemaphoreTake(s_aux_lock, portMAX_DELAY);
    s_pending_position.set = false;
    s_position_req = true;
    xSemaphoreGive(s_aux_lock);
}

class McAuxThread : public concurrency::OSThread
{
  public:
    McAuxThread() : concurrency::OSThread("McAux") {}

  protected:
    int32_t runOnce() override
    {

        if (s_owner_req) {
            aux_lock_init();
            PendingOwner po;
            xSemaphoreTake(s_aux_lock, portMAX_DELAY);
            po = s_pending_owner;
            s_pending_owner = {};
            s_owner_req = false;
            xSemaphoreGive(s_aux_lock);

            bool changed = false;
            if (po.long_set && po.long_name[0]) {
                if (strcmp(owner.long_name, po.long_name) != 0) {
                    strncpy(owner.long_name, po.long_name, sizeof(owner.long_name) - 1);
                    owner.long_name[sizeof(owner.long_name) - 1] = '\0';
                    changed = true;
                }
            }
            if (po.short_set && po.short_name[0]) {
                if (strcmp(owner.short_name, po.short_name) != 0) {
                    strncpy(owner.short_name, po.short_name, sizeof(owner.short_name) - 1);
                    owner.short_name[sizeof(owner.short_name) - 1] = '\0';
                    changed = true;
                }
            }
            if (changed) {
                LOG_INFO("mcui: applying owner edit (long=%s short=%s)",
                         owner.long_name, owner.short_name);
                if (service) service->reloadOwner(true);
                if (nodeDB) nodeDB->saveToDisk(SEGMENT_DEVICESTATE | SEGMENT_NODEDATABASE);
            }
        }

        bool regen_keys = false;
        aux_lock_init();
        xSemaphoreTake(s_aux_lock, portMAX_DELAY);
        if (s_regen_keys_req) {
            s_regen_keys_req = false;
            regen_keys = true;
        }
        xSemaphoreGive(s_aux_lock);

        if (regen_keys) {
#if !(MESHTASTIC_EXCLUDE_PKI_KEYGEN || MESHTASTIC_EXCLUDE_PKI)
            if (owner.is_licensed) {
                LOG_WARN("mcui: private key regeneration skipped in licensed/ham mode");
            } else if (!crypto) {
                LOG_WARN("mcui: private key regeneration skipped, crypto unavailable");
            } else {
                LOG_WARN("mcui: regenerating private keys");
                if (!config.has_security) {
                    config.has_security = true;
                    config.security = meshtastic_Config_SecurityConfig_init_default;
                    config.security.serial_enabled = config.device.serial_enabled;
                    config.security.is_managed = config.device.is_managed;
                }
                crypto->generateKeyPair(config.security.public_key.bytes,
                                        config.security.private_key.bytes);
                config.security.public_key.size = 32;
                config.security.private_key.size = 32;
                owner.public_key.size = 32;
                memcpy(owner.public_key.bytes, config.security.public_key.bytes, 32);
                crypto->setDHPrivateKey(config.security.private_key.bytes);
                if (service) service->reloadOwner(true);
                if (nodeDB)
                    nodeDB->saveToDisk(SEGMENT_CONFIG | SEGMENT_DEVICESTATE | SEGMENT_NODEDATABASE);
            }
#else
            LOG_WARN("mcui: private key regeneration unavailable in this build");
#endif
        }

        bool position_req = false;
        PendingPosition pp = {};
        aux_lock_init();
        xSemaphoreTake(s_aux_lock, portMAX_DELAY);
        if (s_position_req) {
            s_position_req = false;
            position_req = true;
            pp = s_pending_position;
        }
        xSemaphoreGive(s_aux_lock);

        if (position_req) {
            if (pp.set) {

                meshtastic_Position p = meshtastic_Position_init_default;
                p.has_latitude_i  = true;
                p.latitude_i      = (int32_t)llround(pp.lat_deg * 1e7);
                p.has_longitude_i = true;
                p.longitude_i     = (int32_t)llround(pp.lon_deg * 1e7);
                if (pp.has_alt) {
                    p.has_altitude = true;
                    p.altitude     = pp.alt_m;
                }
                p.location_source = meshtastic_Position_LocSource_LOC_MANUAL;
                p.time            = (uint32_t)getValidTime(RTCQualityFromNet);
                p.timestamp       = p.time;

                if (nodeDB) {
                    nodeDB->setLocalPosition(p);
                    meshtastic_NodeInfoLite *me =
                        nodeDB->getMeshNode(nodeDB->getNodeNum());
                    if (me) {
                        me->has_position = true;
                        me->position = TypeConversions::ConvertToPositionLite(p);
                    }
                }
                config.position.fixed_position = true;
                if (nodeDB) nodeDB->saveToDisk(SEGMENT_CONFIG | SEGMENT_NODEDATABASE);

                LOG_INFO("mcui: applied manual position lat=%.6f lon=%.6f alt=%d "
                         "(fixed_position=true)",
                         pp.lat_deg, pp.lon_deg, pp.has_alt ? (int)pp.alt_m : 0);

                if (positionModule) positionModule->sendOurPosition();
            } else {

                if (nodeDB) {
                    nodeDB->clearLocalPosition();
                    meshtastic_NodeInfoLite *me =
                        nodeDB->getMeshNode(nodeDB->getNodeNum());
                    if (me) {
                        me->has_position = false;
                        me->position = meshtastic_PositionLite_init_default;
                    }
                }
                config.position.fixed_position = false;
                if (nodeDB) nodeDB->saveToDisk(SEGMENT_CONFIG | SEGMENT_NODEDATABASE);
                LOG_INFO("mcui: cleared manual position (fixed_position=false)");
            }
        }

        static uint32_t s_last_tz_check_ms = 0;
        uint32_t now_ms = millis();
        if (now_ms - s_last_tz_check_ms >= 60UL * 1000UL) {
            s_last_tz_check_ms = now_ms;
            tz_apply_to_libc();
        }

        if (s_scan_state == SCAN_REQUESTED) {
            s_scan_state = SCAN_RUNNING;
            LOG_INFO("mcui: starting WiFi scan");

            if (WiFi.getMode() == WIFI_MODE_NULL) {
                WiFi.mode(WIFI_STA);
            }
            int n = WiFi.scanNetworks( false,  false);
            if (n < 0) {
                LOG_WARN("mcui: WiFi scan failed (%d)", n);
                s_scan_count = 0;
                s_scan_state = SCAN_FAIL;
                return 500;
            }
            if (n > SCAN_MAX) n = SCAN_MAX;
            s_scan_count = 0;
            for (int i = 0; i < n; i++) {
                ScannedNetwork &r = s_scan_results[s_scan_count];
                String ssid = WiFi.SSID(i);
                if (ssid.length() == 0) continue;
                strncpy(r.ssid, ssid.c_str(), sizeof(r.ssid) - 1);
                r.ssid[sizeof(r.ssid) - 1] = '\0';
                r.rssi = (int8_t)WiFi.RSSI(i);
                r.encrypted = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
                s_scan_count++;
            }
            WiFi.scanDelete();
            LOG_INFO("mcui: WiFi scan found %d networks", s_scan_count);
            s_scan_state = SCAN_DONE;
        }
        return 300;
    }
};

static McAuxThread *s_aux_thread = nullptr;
static void ensure_aux_thread()
{
    if (!s_aux_thread) s_aux_thread = new McAuxThread();
}

struct EnumEntry {
    int         code;
    const char *label;
};

static const EnumEntry REGIONS[] = {
    {meshtastic_Config_LoRaConfig_RegionCode_UNSET,    "UNSET"},
    {meshtastic_Config_LoRaConfig_RegionCode_US,       "US"},
    {meshtastic_Config_LoRaConfig_RegionCode_EU_433,   "EU_433"},
    {meshtastic_Config_LoRaConfig_RegionCode_EU_868,   "EU_868"},
    {meshtastic_Config_LoRaConfig_RegionCode_CN,       "CN"},
    {meshtastic_Config_LoRaConfig_RegionCode_JP,       "JP"},
    {meshtastic_Config_LoRaConfig_RegionCode_ANZ,      "ANZ"},
    {meshtastic_Config_LoRaConfig_RegionCode_KR,       "KR"},
    {meshtastic_Config_LoRaConfig_RegionCode_TW,       "TW"},
    {meshtastic_Config_LoRaConfig_RegionCode_RU,       "RU"},
    {meshtastic_Config_LoRaConfig_RegionCode_IN,       "IN"},
    {meshtastic_Config_LoRaConfig_RegionCode_NZ_865,   "NZ_865"},
    {meshtastic_Config_LoRaConfig_RegionCode_TH,       "TH"},
    {meshtastic_Config_LoRaConfig_RegionCode_LORA_24,  "LORA_24"},
    {meshtastic_Config_LoRaConfig_RegionCode_UA_433,   "UA_433"},
    {meshtastic_Config_LoRaConfig_RegionCode_UA_868,   "UA_868"},
    {meshtastic_Config_LoRaConfig_RegionCode_MY_433,   "MY_433"},
    {meshtastic_Config_LoRaConfig_RegionCode_MY_919,   "MY_919"},
    {meshtastic_Config_LoRaConfig_RegionCode_SG_923,   "SG_923"},
};
static constexpr int REGION_COUNT = sizeof(REGIONS) / sizeof(REGIONS[0]);

static const EnumEntry PRESETS[] = {
    {meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST,        "Long Fast"},
    {meshtastic_Config_LoRaConfig_ModemPreset_LONG_SLOW,        "Long Slow"},
    {meshtastic_Config_LoRaConfig_ModemPreset_VERY_LONG_SLOW,   "Very Long Slow"},
    {meshtastic_Config_LoRaConfig_ModemPreset_LONG_MODERATE,    "Long Moderate"},
    {meshtastic_Config_LoRaConfig_ModemPreset_MEDIUM_SLOW,      "Medium Slow"},
    {meshtastic_Config_LoRaConfig_ModemPreset_MEDIUM_FAST,      "Medium Fast"},
    {meshtastic_Config_LoRaConfig_ModemPreset_SHORT_SLOW,       "Short Slow"},
    {meshtastic_Config_LoRaConfig_ModemPreset_SHORT_FAST,       "Short Fast"},
    {meshtastic_Config_LoRaConfig_ModemPreset_SHORT_TURBO,      "Short Turbo"},
    {meshtastic_Config_LoRaConfig_ModemPreset_LONG_TURBO,       "Long Turbo"},
};
static constexpr int PRESET_COUNT = sizeof(PRESETS) / sizeof(PRESETS[0]);

static const EnumEntry ROLES[] = {
    {meshtastic_Config_DeviceConfig_Role_CLIENT,          "Client"},
    {meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE,     "Client Mute"},
    {meshtastic_Config_DeviceConfig_Role_CLIENT_HIDDEN,   "Client Hidden"},
    {meshtastic_Config_DeviceConfig_Role_ROUTER,          "Router"},
    {meshtastic_Config_DeviceConfig_Role_ROUTER_CLIENT,   "Router Client"},
    {meshtastic_Config_DeviceConfig_Role_REPEATER,        "Repeater"},
    {meshtastic_Config_DeviceConfig_Role_TRACKER,         "Tracker"},
    {meshtastic_Config_DeviceConfig_Role_SENSOR,          "Sensor"},
    {meshtastic_Config_DeviceConfig_Role_TAK,             "TAK"},
    {meshtastic_Config_DeviceConfig_Role_TAK_TRACKER,     "TAK Tracker"},
    {meshtastic_Config_DeviceConfig_Role_LOST_AND_FOUND,  "Lost & Found"},
};
static constexpr int ROLE_COUNT = sizeof(ROLES) / sizeof(ROLES[0]);

static const EnumEntry REBROADCAST_MODES[] = {
    {meshtastic_Config_DeviceConfig_RebroadcastMode_ALL,                "All"},
    {meshtastic_Config_DeviceConfig_RebroadcastMode_ALL_SKIP_DECODING,  "All (skip decoding)"},
    {meshtastic_Config_DeviceConfig_RebroadcastMode_LOCAL_ONLY,         "Local Only"},
    {meshtastic_Config_DeviceConfig_RebroadcastMode_KNOWN_ONLY,         "Known Only"},
    {meshtastic_Config_DeviceConfig_RebroadcastMode_NONE,               "None"},
    {meshtastic_Config_DeviceConfig_RebroadcastMode_CORE_PORTNUMS_ONLY, "Core Portnums"},
};
static constexpr int REBROADCAST_COUNT = sizeof(REBROADCAST_MODES) / sizeof(REBROADCAST_MODES[0]);

static const EnumEntry UNITS[] = {
    {meshtastic_Config_DisplayConfig_DisplayUnits_METRIC,   "Metric"},
    {meshtastic_Config_DisplayConfig_DisplayUnits_IMPERIAL, "Imperial"},
};
static constexpr int UNITS_COUNT = sizeof(UNITS) / sizeof(UNITS[0]);

enum McuiDSTRule : uint8_t {
    MCUI_DST_NONE = 0,
    MCUI_DST_EU,
    MCUI_DST_US,
    MCUI_DST_AU,
};

struct TzEntry {
    const char *label;
    int32_t     std_offset_s;
    McuiDSTRule     dst_rule;
};

static const TzEntry TZ_LIST[] = {
    {"UTC      (no offset)",                  0,      MCUI_DST_NONE},

    {"UTC-12   Baker Island",                 -43200, MCUI_DST_NONE},
    {"UTC-11   Samoa, Niue",                  -39600, MCUI_DST_NONE},
    {"UTC-10   Hawaii, Tahiti (no DST)",      -36000, MCUI_DST_NONE},
    {"UTC-9    Alaska (auto DST)",            -32400, MCUI_DST_US},
    {"UTC-8    Los Angeles (auto DST)",       -28800, MCUI_DST_US},
    {"UTC-7    Arizona (no DST)",             -25200, MCUI_DST_NONE},
    {"UTC-7    Denver (auto DST)",            -25200, MCUI_DST_US},
    {"UTC-6    Chicago / Mexico (auto DST)",  -21600, MCUI_DST_US},
    {"UTC-5    New York (auto DST)",          -18000, MCUI_DST_US},
    {"UTC-5    Bogota, Lima (no DST)",        -18000, MCUI_DST_NONE},
    {"UTC-4    Halifax (auto DST) / Caracas", -14400, MCUI_DST_US},
    {"UTC-3    Brasilia, Buenos Aires",       -10800, MCUI_DST_NONE},
    {"UTC-2    Mid-Atlantic, South Georgia",   -7200, MCUI_DST_NONE},
    {"UTC-1    Azores (auto DST)",             -3600, MCUI_DST_EU},

    {"UTC      London (auto DST)",                 0, MCUI_DST_EU},
    {"UTC      Reykjavik, Accra (no DST)",         0, MCUI_DST_NONE},
    {"UTC+1    Brussels, Paris, Berlin (auto DST)", 3600, MCUI_DST_EU},
    {"UTC+1    Lagos, Algiers (no DST)",        3600, MCUI_DST_NONE},
    {"UTC+2    Athens, Helsinki (auto DST)",    7200, MCUI_DST_EU},
    {"UTC+2    Cairo, Johannesburg (no DST)",   7200, MCUI_DST_NONE},
    {"UTC+3    Moscow, Istanbul, Riyadh",      10800, MCUI_DST_NONE},
    {"UTC+3:30 Tehran (no DST)",               12600, MCUI_DST_NONE},

    {"UTC+4    Dubai, Baku, Tbilisi",          14400, MCUI_DST_NONE},
    {"UTC+4:30 Kabul",                         16200, MCUI_DST_NONE},
    {"UTC+5    Karachi, Tashkent, Yekaterinburg", 18000, MCUI_DST_NONE},
    {"UTC+5:30 Mumbai, Delhi, Colombo",        19800, MCUI_DST_NONE},
    {"UTC+5:45 Kathmandu",                     20700, MCUI_DST_NONE},
    {"UTC+6    Dhaka, Almaty",                 21600, MCUI_DST_NONE},
    {"UTC+6:30 Yangon, Cocos Is.",             23400, MCUI_DST_NONE},
    {"UTC+7    Bangkok, Jakarta, Hanoi",       25200, MCUI_DST_NONE},
    {"UTC+8    Beijing, Singapore, HK, Perth", 28800, MCUI_DST_NONE},
    {"UTC+8:45 Eucla (Australia)",             31500, MCUI_DST_NONE},
    {"UTC+9    Tokyo, Seoul, Pyongyang",       32400, MCUI_DST_NONE},
    {"UTC+9:30 Darwin (no DST)",               34200, MCUI_DST_NONE},
    {"UTC+9:30 Adelaide (auto DST)",           34200, MCUI_DST_AU},
    {"UTC+10   Brisbane (no DST)",             36000, MCUI_DST_NONE},
    {"UTC+10   Sydney, Melbourne (auto DST)",  36000, MCUI_DST_AU},
    {"UTC+11   Solomon Is., Noumea",           39600, MCUI_DST_NONE},
    {"UTC+12   Fiji, Kamchatka, Tarawa",       43200, MCUI_DST_NONE},
    {"UTC+12   Auckland (auto DST)",           43200, MCUI_DST_AU},
    {"UTC+13   Tonga, Samoa, Phoenix Is.",     46800, MCUI_DST_NONE},
    {"UTC+14   Kiribati (Line Islands)",       50400, MCUI_DST_NONE},
};
static constexpr int TZ_COUNT = sizeof(TZ_LIST) / sizeof(TZ_LIST[0]);
static constexpr int TZ_DEFAULT_INDEX = 0;

static int tz_dow(int y, int m, int d)
{
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y--;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

static int tz_last_sunday(int year, int month)
{
    int dim;
    switch (month) {
        case 2:  dim = ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0) ? 29 : 28; break;
        case 4: case 6: case 9: case 11: dim = 30; break;
        default: dim = 31; break;
    }
    int wd = tz_dow(year, month, dim);
    return dim - wd;
}

static int tz_nth_sunday(int year, int month, int n)
{
    int wd1 = tz_dow(year, month, 1);
    int first_sun = (wd1 == 0) ? 1 : (8 - wd1);
    return first_sun + (n - 1) * 7;
}

static bool tz_is_dst_active(uint32_t utc_epoch, McuiDSTRule rule)
{
    if (rule == MCUI_DST_NONE) return false;
    time_t t = (time_t)utc_epoch;
    struct tm tm;
    gmtime_r(&t, &tm);
    int year = tm.tm_year + 1900;
    int mon  = tm.tm_mon + 1;
    int day  = tm.tm_mday;
    int hour_utc = tm.tm_hour;
    int md = mon * 100 + day;

    if (rule == MCUI_DST_EU) {

        int start = 3 * 100 + tz_last_sunday(year, 3);
        int end   = 10 * 100 + tz_last_sunday(year, 10);
        if (md > start && md < end) return true;
        if (md < start || md > end) return false;
        if (md == start) return hour_utc >= 1;
        if (md == end)   return hour_utc < 1;
        return false;
    }
    if (rule == MCUI_DST_US) {

        int start = 3 * 100 + tz_nth_sunday(year, 3, 2);
        int end   = 11 * 100 + tz_nth_sunday(year, 11, 1);
        if (md > start && md < end) return true;
        if (md < start || md > end) return false;
        if (md == start) return hour_utc >= 10;
        if (md == end)   return hour_utc < 6;
        return false;
    }
    if (rule == MCUI_DST_AU) {

        int oct_sun = tz_nth_sunday(year, 10, 1);
        int apr_sun = tz_nth_sunday(year, 4, 1);
        if (mon >= 10) {
            if (md > 10 * 100 + oct_sun) return true;
            if (md == 10 * 100 + oct_sun) return hour_utc >= 16;
            return false;
        }
        if (mon <= 4) {
            if (md < 4 * 100 + apr_sun) return true;
            if (md == 4 * 100 + apr_sun) return hour_utc < 16;
            return false;
        }
        return false;
    }
    return false;
}

static int32_t tz_effective_offset(int idx, uint32_t utc_epoch)
{
    if (idx < 0 || idx >= TZ_COUNT) idx = TZ_DEFAULT_INDEX;
    int32_t off = TZ_LIST[idx].std_offset_s;
    if (tz_is_dst_active(utc_epoch, TZ_LIST[idx].dst_rule)) off += 3600;
    return off;
}

static void tz_build_posix_fixed(int32_t off_s, char *out, size_t out_sz)
{

    int sign_chr_tag = (off_s >= 0) ? '+' : '-';
    int sign_chr_off = (off_s >= 0) ? '-' : '+';
    int32_t abs_s = (off_s >= 0) ? off_s : -off_s;
    int hours = (int)(abs_s / 3600);
    int mins  = (int)((abs_s % 3600) / 60);
    if (mins == 0) {
        snprintf(out, out_sz, "<%c%02d>%c%d",
                 sign_chr_tag, hours, sign_chr_off, hours);
    } else {
        snprintf(out, out_sz, "<%c%02d%02d>%c%d:%02d",
                 sign_chr_tag, hours, mins, sign_chr_off, hours, mins);
    }
}

static constexpr const char *TZ_NVS_NS  = "meshtastic";
static constexpr const char *TZ_NVS_KEY = "mcuiTzIdx";

static int s_tz_index = TZ_DEFAULT_INDEX;
static int32_t s_tz_offset_now = 0;
static uint32_t s_tz_last_apply_ms = 0;

static void tz_apply_to_libc()
{
    time_t now = time(nullptr);
    int32_t off = tz_effective_offset(s_tz_index, (uint32_t)now);
    if (off == s_tz_offset_now && s_tz_last_apply_ms != 0) return;
    char tz_str[24];
    tz_build_posix_fixed(off, tz_str, sizeof(tz_str));
    setenv("TZ", tz_str, 1);
    tzset();
    s_tz_offset_now = off;
    s_tz_last_apply_ms = millis();
    LOG_INFO("mcui: TZ idx=%d (%s) → libc=%s offset=%+d s",
             s_tz_index, TZ_LIST[s_tz_index].label, tz_str, (int)off);
}

static int tz_load_from_nvs()
{
    Preferences p;
    int idx = TZ_DEFAULT_INDEX;
    if (p.begin(TZ_NVS_NS,  true)) {
        idx = (int)p.getUChar(TZ_NVS_KEY, (uint8_t)TZ_DEFAULT_INDEX);
        p.end();
    }
    if (idx < 0 || idx >= TZ_COUNT) idx = TZ_DEFAULT_INDEX;
    return idx;
}

static void tz_save_to_nvs(int idx)
{
    Preferences p;
    if (p.begin(TZ_NVS_NS,  false)) {
        p.putUChar(TZ_NVS_KEY, (uint8_t)idx);
        p.end();
    }
}

extern "C" void mcui_tz_apply_at_boot()
{
    s_tz_index = tz_load_from_nvs();
    tz_apply_to_libc();
    ensure_aux_thread();
}

static lv_obj_t *s_row_noise = nullptr;
static constexpr const char *NOISE_NVS_KEY = "mcuiShowNoise";

static bool show_noise_floor_get()
{
    Preferences p;
    bool v = false;
    if (p.begin(TZ_NVS_NS,  true)) {
        v = p.getBool(NOISE_NVS_KEY, false);
        p.end();
    }
    return v;
}

static void show_noise_floor_set(bool enabled)
{
    Preferences p;
    if (p.begin(TZ_NVS_NS,  false)) {
        p.putBool(NOISE_NVS_KEY, enabled);
        p.end();
    }
}

static void show_noise_floor_changed_cb(lv_event_t *e)
{
    lv_obj_t *sw = (lv_obj_t *)lv_event_get_current_target(e);
    bool enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    show_noise_floor_set(enabled);
    if (s_row_noise) {
        if (enabled) lv_obj_remove_flag(s_row_noise, LV_OBJ_FLAG_HIDDEN);
        else         lv_obj_add_flag(s_row_noise, LV_OBJ_FLAG_HIDDEN);
    }
}

static const EnumEntry HOP_LIMITS[] = {
    {1, "1"},
    {2, "2"},
    {3, "3 (default)"},
    {4, "4"},
    {5, "5"},
    {6, "6"},
    {7, "7"},
};
static constexpr int HOP_LIMIT_COUNT = sizeof(HOP_LIMITS) / sizeof(HOP_LIMITS[0]);

static const EnumEntry TX_POWERS[] = {
    {0,  "Region default"},
    {5,  "5 dBm"},
    {10, "10 dBm"},
    {14, "14 dBm"},
    {17, "17 dBm"},
    {20, "20 dBm (default)"},
    {22, "22 dBm"},
};
static constexpr int TX_POWER_COUNT = sizeof(TX_POWERS) / sizeof(TX_POWERS[0]);

struct IntervalEntry {
    uint32_t    secs;
    const char *label;
};
static const IntervalEntry INTERVAL_SHORT[] = {
    {60,     "1 min"},
    {300,    "5 min"},
    {900,    "15 min"},
    {1800,   "30 min"},
    {3600,   "1 hour"},
    {10800,  "3 hours"},
    {21600,  "6 hours"},
    {43200,  "12 hours"},
    {86400,  "24 hours"},
};
static constexpr int INTERVAL_SHORT_COUNT = sizeof(INTERVAL_SHORT) / sizeof(INTERVAL_SHORT[0]);

static const IntervalEntry SCREEN_TIMEOUT[] = {
    {0,          "Default"},
    {15,         "15 s"},
    {30,         "30 s"},
    {60,         "1 min"},
    {300,        "5 min"},
    {900,        "15 min"},
    {3600,       "1 hour"},
    {0xFFFFFFFF, "Always"},
};
static constexpr int SCREEN_TIMEOUT_COUNT = sizeof(SCREEN_TIMEOUT) / sizeof(SCREEN_TIMEOUT[0]);

static const char *enum_label(const EnumEntry *table, int n, int code)
{
    for (int i = 0; i < n; i++) if (table[i].code == code) return table[i].label;
    return "?";
}
static int enum_index(const EnumEntry *table, int n, int code)
{
    for (int i = 0; i < n; i++) if (table[i].code == code) return i;
    return 0;
}
static int interval_index(const IntervalEntry *table, int n, uint32_t secs)
{

    for (int i = 0; i < n; i++) if (table[i].secs == secs) return i;

    for (int i = 0; i < n; i++) if (table[i].secs >= secs) return i;
    return n - 1;
}

static void build_dropdown_options(char *out, size_t out_sz,
                                   const char *const *labels, int n)
{
    out[0] = '\0';
    size_t used = 0;
    for (int i = 0; i < n; i++) {
        size_t len = strlen(labels[i]);
        if (used + len + 2 > out_sz) break;
        if (i) out[used++] = '\n';
        memcpy(out + used, labels[i], len);
        used += len;
        out[used] = '\0';
    }
}

static lv_obj_t *s_page = nullptr;
static lv_obj_t *s_list = nullptr;

static lv_obj_t *s_lbl_firmware    = nullptr;
static lv_obj_t *s_lbl_owner       = nullptr;
static lv_obj_t *s_lbl_nodeid      = nullptr;
static lv_obj_t *s_lbl_region_now  = nullptr;
static lv_obj_t *s_lbl_preset_now  = nullptr;
static lv_obj_t *s_lbl_uptime      = nullptr;
static lv_obj_t *s_lbl_heap        = nullptr;
static lv_obj_t *s_lbl_battery     = nullptr;
static lv_obj_t *s_lbl_noise       = nullptr;

static lv_obj_t *s_lbl_wifi_ssid   = nullptr;
static lv_obj_t *s_lbl_orientation = nullptr;
static lv_obj_t *s_btn_orientation = nullptr;
static lv_obj_t *s_btn_orientation_label = nullptr;
static lv_obj_t *s_orientation_overlay = nullptr;
static lv_obj_t *s_orientation_overlay_label = nullptr;

static lv_obj_t *s_lbl_owner_long  = nullptr;
static lv_obj_t *s_lbl_owner_short = nullptr;

static lv_obj_t *s_lbl_pos_lat  = nullptr;
static lv_obj_t *s_lbl_pos_lon  = nullptr;
static lv_obj_t *s_lbl_pos_alt  = nullptr;
static lv_obj_t *s_lbl_pos_status = nullptr;

static char s_pos_lat_pending[24] = "";
static char s_pos_lon_pending[24] = "";
static char s_pos_alt_pending[24] = "";

static uint32_t s_last_refresh_ms = 0;
static uint32_t s_orientation_reboot_at_ms = 0;
static uint32_t s_orientation_last_countdown = UINT32_MAX;

static void refresh_values();
static void wifi_enabled_changed_cb(lv_event_t *e);

static void orientation_overlay_close()
{
    if (s_orientation_overlay) {
        lv_obj_delete(s_orientation_overlay);
        s_orientation_overlay = nullptr;
    }
    s_orientation_overlay_label = nullptr;
    s_orientation_reboot_at_ms = 0;
    s_orientation_last_countdown = UINT32_MAX;
}

static void orientation_overlay_update()
{
    if (!s_orientation_overlay || !s_orientation_overlay_label || !s_orientation_reboot_at_ms)
        return;

    uint32_t now = millis();
    uint32_t secs = 0;
    if (now < s_orientation_reboot_at_ms)
        secs = (s_orientation_reboot_at_ms - now + 999) / 1000;

    if (secs != s_orientation_last_countdown) {
        char buf[32];
        if (secs > 0)
            snprintf(buf, sizeof(buf), "Reboot in %u...", (unsigned)secs);
        else
            snprintf(buf, sizeof(buf), "Rebooting...");
        lv_label_set_text(s_orientation_overlay_label, buf);
        s_orientation_last_countdown = secs;
    }

    if (now >= s_orientation_reboot_at_ms) {
        s_orientation_force_apply = true;
        orientation_overlay_close();
    }
}

static void orientation_overlay_open()
{
    orientation_overlay_close();

    lv_obj_t *scr = lv_screen_active();
    s_orientation_overlay = lv_obj_create(scr);
    lv_obj_remove_style_all(s_orientation_overlay);
    lv_obj_set_size(s_orientation_overlay, SCR_W, SCR_H);
    lv_obj_set_pos(s_orientation_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_orientation_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_orientation_overlay, LV_OPA_70, 0);
    lv_obj_remove_flag(s_orientation_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_orientation_overlay);

    lv_obj_t *card = lv_obj_create(s_orientation_overlay);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, SCR_W > 460 ? 420 : SCR_W - 40, 150);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(TH_SURFACE), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 0, 0);
    lv_obj_set_style_pad_all(card, 18, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Changing orientation");
    lv_obj_set_style_text_color(title, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    s_orientation_overlay_label = lv_label_create(card);
    lv_label_set_text(s_orientation_overlay_label, "Reboot in 3...");
    lv_obj_set_style_text_color(s_orientation_overlay_label, lv_color_hex(TH_TEXT2), 0);
    lv_obj_set_style_text_font(s_orientation_overlay_label, &lv_font_montserrat_16, 0);
    lv_obj_align(s_orientation_overlay_label, LV_ALIGN_CENTER, 0, 20);

    s_orientation_reboot_at_ms = millis() + 3000;
    s_orientation_last_countdown = UINT32_MAX;
    orientation_overlay_update();
}

static void add_section_header(const char *text)
{
    lv_obj_t *h = lv_label_create(s_list);
    lv_label_set_text(h, text);
    lv_obj_set_style_text_color(h, lv_color_hex(TH_ACCENT_LIGHT), 0);
    lv_obj_set_style_text_font(h, &lv_font_montserrat_16, 0);
    lv_obj_set_style_pad_top(h, 10, 0);
    lv_obj_set_style_pad_left(h, 6, 0);
}

static lv_obj_t *add_card()
{
    lv_obj_t *card = lv_obj_create(s_list);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_hex(TH_SURFACE), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 0, 0);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_set_style_pad_row(card, 6, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    return card;
}

static lv_obj_t *add_info_row(lv_obj_t *card, const char *label, const char *initial)
{
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 24);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, lv_color_hex(TH_TEXT2), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *val = lv_label_create(row);
    lv_label_set_text(val, initial);
    lv_obj_set_style_text_color(val, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(val, &lv_font_montserrat_16, 0);
    lv_obj_align(val, LV_ALIGN_RIGHT_MID, 0, 0);
    return val;
}

static lv_obj_t *add_switch_row(lv_obj_t *card, const char *label, bool initial,
                                lv_event_cb_t cb)
{
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 44);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, lv_color_hex(TH_TEXT2), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 56, 28);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
    if (initial) lv_obj_add_state(sw, LV_STATE_CHECKED);
    if (cb) lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, nullptr);
    return sw;
}

static lv_obj_t *add_enum_dropdown_row(lv_obj_t *card, const char *label,
                                       const EnumEntry *table, int n,
                                       int current_code, lv_event_cb_t cb)
{
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 52);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, lv_color_hex(TH_TEXT2), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *dd = lv_dropdown_create(row);
    const char *labels[32];
    int m = (n > 32) ? 32 : n;
    for (int i = 0; i < m; i++) labels[i] = table[i].label;
    char opts[512];
    build_dropdown_options(opts, sizeof(opts), labels, m);
    lv_dropdown_set_options(dd, opts);
    lv_obj_set_width(dd, 190);
    lv_obj_align(dd, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_dropdown_set_selected(dd, (uint16_t)enum_index(table, n, current_code));
    if (cb) lv_obj_add_event_cb(dd, cb, LV_EVENT_VALUE_CHANGED, nullptr);
    return dd;
}

static lv_obj_t *add_interval_dropdown_row(lv_obj_t *card, const char *label,
                                           const IntervalEntry *table, int n,
                                           uint32_t current_secs, lv_event_cb_t cb)
{
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 52);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, lv_color_hex(TH_TEXT2), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *dd = lv_dropdown_create(row);
    const char *labels[16];
    int m = (n > 16) ? 16 : n;
    for (int i = 0; i < m; i++) labels[i] = table[i].label;
    char opts[256];
    build_dropdown_options(opts, sizeof(opts), labels, m);
    lv_dropdown_set_options(dd, opts);
    lv_obj_set_width(dd, 190);
    lv_obj_align(dd, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_dropdown_set_selected(dd, (uint16_t)interval_index(table, n, current_secs));
    if (cb) lv_obj_add_event_cb(dd, cb, LV_EVENT_VALUE_CHANGED, nullptr);
    return dd;
}

static void add_card_hint(lv_obj_t *card, const char *text)
{
    lv_obj_t *h = lv_label_create(card);
    lv_label_set_text(h, text);
    lv_obj_set_style_text_color(h, lv_color_hex(TH_TEXT3), 0);
    lv_obj_set_style_text_font(h, &lv_font_montserrat_16, 0);
    lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(h, lv_pct(100));
}

static int modal_card_height(int desired, int vertical_margin = 40)
{

    int max_h = SCR_H - vertical_margin - keyboard_height();
    return (desired < max_h) ? desired : max_h;
}

static int modal_card_y(int card_h, int top_margin = 20)
{
    int y = (SCR_H - card_h) / 2;
    return (y < top_margin) ? top_margin : y;
}

typedef void (*TextEditCb)(const char *new_text);

static lv_obj_t *s_tem_overlay = nullptr;
static lv_obj_t *s_tem_ta      = nullptr;
static TextEditCb s_tem_cb     = nullptr;

static void tem_close()
{
    keyboard_hide();
    if (s_tem_overlay) {
        lv_obj_delete(s_tem_overlay);
        s_tem_overlay = nullptr;
        s_tem_ta      = nullptr;
    }
    s_tem_cb = nullptr;
}

static void tem_ok_cb(lv_event_t *)
{
    if (s_tem_ta && s_tem_cb) {
        const char *t = lv_textarea_get_text(s_tem_ta);
        TextEditCb cb = s_tem_cb;

        s_tem_cb = nullptr;
        char copy[129];
        strncpy(copy, t ? t : "", sizeof(copy) - 1);
        copy[sizeof(copy) - 1] = '\0';
        tem_close();
        cb(copy);
        return;
    }
    tem_close();
}

static void tem_cancel_cb(lv_event_t *) { tem_close(); }

static void text_edit_modal(const char *title, const char *current,
                            int max_len, bool password, TextEditCb cb)
{
    if (s_tem_overlay) tem_close();
    s_tem_cb = cb;

    lv_obj_t *scr = lv_screen_active();
    s_tem_overlay = lv_obj_create(scr);
    lv_obj_remove_style_all(s_tem_overlay);
    lv_obj_set_size(s_tem_overlay, SCR_W, SCR_H);
    lv_obj_set_pos(s_tem_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_tem_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_tem_overlay, LV_OPA_60, 0);
    lv_obj_remove_flag(s_tem_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_tem_overlay);

    lv_obj_t *card = lv_obj_create(s_tem_overlay);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, SCR_W - 40, 220);
    lv_obj_set_pos(card, 20, SCR_H - keyboard_height() - 240);
    lv_obj_set_style_bg_color(card, lv_color_hex(TH_SURFACE), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 0, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(card);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_color(t, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_16, 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);

    s_tem_ta = lv_textarea_create(card);
    lv_obj_set_size(s_tem_ta, lv_pct(100), 52);
    lv_obj_align(s_tem_ta, LV_ALIGN_TOP_LEFT, 0, 34);
    lv_textarea_set_one_line(s_tem_ta, true);
    lv_textarea_set_text(s_tem_ta, current ? current : "");
    lv_textarea_set_max_length(s_tem_ta, max_len);
    lv_obj_set_style_bg_color(s_tem_ta, lv_color_hex(TH_INPUT), 0);
    lv_obj_set_style_text_color(s_tem_ta, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_border_width(s_tem_ta, 0, 0);
    lv_obj_set_style_radius(s_tem_ta, 0, 0);
    lv_obj_set_style_anim_duration(s_tem_ta, 0, LV_PART_CURSOR);
    if (password) lv_textarea_set_password_mode(s_tem_ta, true);

    lv_obj_t *cancel = lv_button_create(card);
    lv_obj_set_size(cancel, 130, 42);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(TH_INPUT), 0);
    lv_obj_set_style_radius(cancel, 0, 0);
    lv_obj_add_event_cb(cancel, tem_cancel_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *cl = lv_label_create(cancel);
    lv_label_set_text(cl, "Cancel");
    lv_obj_set_style_text_color(cl, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(cl, &lv_font_montserrat_16, 0);
    lv_obj_center(cl);

    lv_obj_t *ok = lv_button_create(card);
    lv_obj_set_size(ok, 130, 42);
    lv_obj_align(ok, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(ok, lv_color_hex(TH_ACCENT), 0);
    lv_obj_set_style_radius(ok, 0, 0);
    lv_obj_add_event_cb(ok, tem_ok_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *ol = lv_label_create(ok);
    lv_label_set_text(ol, "OK");
    lv_obj_set_style_text_color(ol, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(ol, &lv_font_montserrat_16, 0);
    lv_obj_center(ol);

    keyboard_attach(s_tem_ta);
    keyboard_show();
    lv_textarea_set_cursor_pos(s_tem_ta, LV_TEXTAREA_CURSOR_LAST);
}

static lv_obj_t *s_onboarding_overlay = nullptr;
static lv_obj_t *s_onboarding_long    = nullptr;
static lv_obj_t *s_onboarding_short   = nullptr;
static lv_obj_t *s_onboarding_region  = nullptr;
static lv_obj_t *s_onboarding_status  = nullptr;
static bool      s_onboarding_shown   = false;

static bool onboarding_needed()
{
    return config.lora.region == meshtastic_Config_LoRaConfig_RegionCode_UNSET ||
           owner.long_name[0] == '\0' ||
           owner.short_name[0] == '\0';
}

static void onboarding_close()
{
    keyboard_hide();
    if (s_onboarding_overlay) {
        lv_obj_delete(s_onboarding_overlay);
        s_onboarding_overlay = nullptr;
        s_onboarding_long    = nullptr;
        s_onboarding_short   = nullptr;
        s_onboarding_region  = nullptr;
        s_onboarding_status  = nullptr;
    }
}

static void onboarding_later_cb(lv_event_t *)
{
    s_onboarding_shown = true;
    onboarding_close();
}

static void onboarding_focus_cb(lv_event_t *e)
{
    lv_obj_t *ta = (lv_obj_t *)lv_event_get_target(e);
    keyboard_attach(ta);
    keyboard_show();
}

static void onboarding_dismiss_kb_cb(lv_event_t *e)
{
    lv_obj_t *tgt = (lv_obj_t *)lv_event_get_target(e);

    if (tgt && lv_obj_check_type(tgt, &lv_textarea_class)) return;
    keyboard_hide();
}

static lv_obj_t *onboarding_label(lv_obj_t *parent, const char *text, int y)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(TH_TEXT2), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, y);
    return label;
}

static lv_obj_t *onboarding_textarea(lv_obj_t *parent, const char *text,
                                     int max_len, int y)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_set_size(ta, lv_pct(100), 50);
    lv_obj_align(ta, LV_ALIGN_TOP_LEFT, 0, y);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_text(ta, text ? text : "");
    lv_textarea_set_max_length(ta, max_len);
    lv_obj_set_style_bg_color(ta, lv_color_hex(TH_INPUT), 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_border_width(ta, 0, 0);
    lv_obj_set_style_radius(ta, 0, 0);
    lv_obj_set_style_anim_duration(ta, 0, LV_PART_CURSOR);
    lv_obj_add_event_cb(ta, onboarding_focus_cb, LV_EVENT_FOCUSED, nullptr);
    return ta;
}

static void onboarding_save_cb(lv_event_t *e)
{
    const char *longn = s_onboarding_long ? lv_textarea_get_text(s_onboarding_long) : "";
    const char *shortn = s_onboarding_short ? lv_textarea_get_text(s_onboarding_short) : "";
    uint16_t idx = s_onboarding_region ? lv_dropdown_get_selected(s_onboarding_region) : 0;
    if (!longn || !*longn || !shortn || !*shortn) {
        if (s_onboarding_status)
            lv_label_set_text(s_onboarding_status, "Please enter both names.");
        return;
    }
    if (idx >= REGION_COUNT ||
        REGIONS[idx].code == meshtastic_Config_LoRaConfig_RegionCode_UNSET) {
        if (s_onboarding_status)
            lv_label_set_text(s_onboarding_status, "Please select your radio region.");
        return;
    }

    ensure_aux_thread();
    queue_owner_edit(longn, shortn);
    config.lora.region = (meshtastic_Config_LoRaConfig_RegionCode)REGIONS[idx].code;
    cfg_mark_dirty();

    lv_obj_t *btn = (lv_obj_t *)lv_event_get_current_target(e);
    if (btn) lv_obj_add_state(btn, LV_STATE_DISABLED);
    if (s_onboarding_status)
        lv_label_set_text(s_onboarding_status, "Saving setup... rebooting soon.");
    s_onboarding_shown = true;
}

void settings_maybe_show_onboarding()
{
    if (s_onboarding_shown || s_onboarding_overlay || !onboarding_needed())
        return;

    s_onboarding_shown = true;
    lv_obj_t *scr = lv_screen_active();
    s_onboarding_overlay = lv_obj_create(scr);
    lv_obj_remove_style_all(s_onboarding_overlay);
    lv_obj_set_size(s_onboarding_overlay, SCR_W, SCR_H);
    lv_obj_set_pos(s_onboarding_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_onboarding_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_onboarding_overlay, LV_OPA_70, 0);
    lv_obj_remove_flag(s_onboarding_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_onboarding_overlay);

    lv_obj_add_flag(s_onboarding_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_onboarding_overlay, onboarding_dismiss_kb_cb,
                        LV_EVENT_CLICKED, nullptr);

    int onboarding_card_h = modal_card_height(430);
    lv_obj_t *card = lv_obj_create(s_onboarding_overlay);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, SCR_W - 40, onboarding_card_h);
    lv_obj_set_pos(card, 20, modal_card_y(onboarding_card_h));
    lv_obj_set_style_bg_color(card, lv_color_hex(TH_SURFACE), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 0, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_set_scroll_dir(card, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "First setup");
    lv_obj_set_style_text_color(title, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *hint = lv_label_create(card);
    lv_label_set_text(hint, "Choose names and a legal LoRa region before using the mesh.");
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, lv_pct(100));
    lv_obj_set_style_text_color(hint, lv_color_hex(TH_TEXT3), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_16, 0);
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 0, 32);

    onboarding_label(card, "Long name", 84);
    s_onboarding_long = onboarding_textarea(card, owner.long_name, 39, 110);

    onboarding_label(card, "Short name", 172);
    s_onboarding_short = onboarding_textarea(card, owner.short_name, 4, 198);

    onboarding_label(card, "Region", 260);
    s_onboarding_region = lv_dropdown_create(card);
    const char *labels[32];
    int m = (REGION_COUNT > 32) ? 32 : REGION_COUNT;
    for (int i = 0; i < m; i++) labels[i] = REGIONS[i].label;
    char opts[512];
    build_dropdown_options(opts, sizeof(opts), labels, m);
    lv_dropdown_set_options(s_onboarding_region, opts);
    lv_dropdown_set_selected(s_onboarding_region,
                             (uint16_t)enum_index(REGIONS, REGION_COUNT,
                                                  config.lora.region));
    lv_obj_set_size(s_onboarding_region, lv_pct(100), 50);
    lv_obj_align(s_onboarding_region, LV_ALIGN_TOP_LEFT, 0, 286);
    lv_obj_set_style_bg_color(s_onboarding_region, lv_color_hex(TH_INPUT), 0);
    lv_obj_set_style_text_color(s_onboarding_region, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_border_width(s_onboarding_region, 0, 0);
    lv_obj_set_style_radius(s_onboarding_region, 0, 0);

    s_onboarding_status = lv_label_create(card);
    lv_label_set_text(s_onboarding_status, "");
    lv_label_set_long_mode(s_onboarding_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_onboarding_status, lv_pct(100));
    lv_obj_set_style_text_color(s_onboarding_status, lv_color_hex(0xE0A030), 0);
    lv_obj_set_style_text_font(s_onboarding_status, &lv_font_montserrat_16, 0);
    lv_obj_align(s_onboarding_status, LV_ALIGN_TOP_LEFT, 0, 348);

    const int onboarding_btn_y = 380;
    lv_obj_t *later = lv_button_create(card);
    lv_obj_set_size(later, 130, 42);
    lv_obj_align(later, LV_ALIGN_TOP_LEFT, 0, onboarding_btn_y);
    lv_obj_set_style_bg_color(later, lv_color_hex(TH_INPUT), 0);
    lv_obj_set_style_radius(later, 0, 0);
    lv_obj_add_event_cb(later, onboarding_later_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *ll = lv_label_create(later);
    lv_label_set_text(ll, "Later");
    lv_obj_set_style_text_color(ll, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(ll, &lv_font_montserrat_16, 0);
    lv_obj_center(ll);

    lv_obj_t *save = lv_button_create(card);
    lv_obj_set_size(save, 160, 42);
    lv_obj_align(save, LV_ALIGN_TOP_RIGHT, 0, onboarding_btn_y);
    lv_obj_set_style_bg_color(save, lv_color_hex(TH_ACCENT), 0);
    lv_obj_set_style_radius(save, 0, 0);
    lv_obj_add_event_cb(save, onboarding_save_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *sl = lv_label_create(save);
    lv_label_set_text(sl, "Save & reboot");
    lv_obj_set_style_text_color(sl, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(sl, &lv_font_montserrat_16, 0);
    lv_obj_center(sl);

    keyboard_attach(s_onboarding_long);
    keyboard_show();
    lv_textarea_set_cursor_pos(s_onboarding_long, LV_TEXTAREA_CURSOR_LAST);
}

static lv_obj_t *s_wifi_overlay = nullptr;
static lv_obj_t *s_wifi_list    = nullptr;
static lv_obj_t *s_wifi_status  = nullptr;
static lv_obj_t *s_wifi_enabled_sw = nullptr;
static lv_obj_t *s_wifi_screen_ssid = nullptr;
static lv_obj_t *s_wifi_reboot_overlay = nullptr;
static uint32_t  s_wifi_poll_ms = 0;

static char      s_wifi_pending_ssid[33] = {0};
static bool      s_wifi_pending_encrypted = false;

static void wifi_screen_close()
{
    keyboard_hide();
    if (s_wifi_overlay) {
        lv_obj_delete(s_wifi_overlay);
        s_wifi_overlay = nullptr;
        s_wifi_list    = nullptr;
        s_wifi_status  = nullptr;
    }
    s_wifi_enabled_sw = nullptr;
    s_wifi_screen_ssid = nullptr;
    s_scan_state = SCAN_IDLE;
}

static void wifi_screen_back_cb(lv_event_t *) { wifi_screen_close(); }

static void wifi_show_rebooting_overlay()
{
    if (s_wifi_reboot_overlay) {
        lv_obj_delete(s_wifi_reboot_overlay);
        s_wifi_reboot_overlay = nullptr;
    }

    lv_obj_t *scr = lv_screen_active();
    s_wifi_reboot_overlay = lv_obj_create(scr);
    lv_obj_remove_style_all(s_wifi_reboot_overlay);
    lv_obj_set_size(s_wifi_reboot_overlay, SCR_W, SCR_H);
    lv_obj_set_pos(s_wifi_reboot_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_wifi_reboot_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_wifi_reboot_overlay, LV_OPA_70, 0);
    lv_obj_remove_flag(s_wifi_reboot_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_wifi_reboot_overlay);

    lv_obj_t *card = lv_obj_create(s_wifi_reboot_overlay);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, SCR_W > 420 ? 320 : SCR_W - 40, 110);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(TH_SURFACE), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 0, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(card);
    lv_label_set_text(lbl, "Rebooting...");
    lv_obj_set_style_text_color(lbl, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    lv_obj_center(lbl);
}

static void wifi_apply(const char *ssid, const char *psk)
{
    strncpy(config.network.wifi_ssid, ssid,
            sizeof(config.network.wifi_ssid) - 1);
    config.network.wifi_ssid[sizeof(config.network.wifi_ssid) - 1] = '\0';
    if (psk) {
        strncpy(config.network.wifi_psk, psk,
                sizeof(config.network.wifi_psk) - 1);
        config.network.wifi_psk[sizeof(config.network.wifi_psk) - 1] = '\0';
    } else {
        config.network.wifi_psk[0] = '\0';
    }
    config.network.wifi_enabled = true;
    LOG_INFO("mcui: WiFi set to SSID=%s (reboot pending)", ssid);
    cfg_mark_dirty();
}

static void wifi_password_entered(const char *pw)
{
    wifi_apply(s_wifi_pending_ssid, pw);
    refresh_values();

    // Persist WiFi credentials immediately so reboot can happen right away.
    if (nodeDB) nodeDB->saveToDisk(SEGMENT_CONFIG);
    if (service) service->reloadConfig(SEGMENT_CONFIG);

    wifi_show_rebooting_overlay();
    rebootAtMsec = millis() + 1200;
}

static void wifi_pick_btn_clicked(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_scan_count) return;

    const ScannedNetwork &n = s_scan_results[idx];
    strncpy(s_wifi_pending_ssid, n.ssid, sizeof(s_wifi_pending_ssid) - 1);
    s_wifi_pending_ssid[sizeof(s_wifi_pending_ssid) - 1] = '\0';
    s_wifi_pending_encrypted = n.encrypted;

    if (n.encrypted) {
        char title[64];
        snprintf(title, sizeof(title), "Password for %s", n.ssid);

        text_edit_modal(title, "", 64,  true, wifi_password_entered);
    } else {
        wifi_apply(s_wifi_pending_ssid, "");
        refresh_values();
    }
}

static void wifi_populate_list()
{
    if (!s_wifi_list) return;

    lv_obj_clean(s_wifi_list);

    if (s_scan_count == 0) {
        lv_obj_t *l = lv_label_create(s_wifi_list);
        lv_label_set_text(l, "No networks found.");
        lv_obj_set_style_text_color(l, lv_color_hex(TH_TEXT3), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
        return;
    }

    for (int i = 0; i < s_scan_count; i++) {
        const ScannedNetwork &n = s_scan_results[i];
        lv_obj_t *row = lv_button_create(s_wifi_list);
        lv_obj_set_size(row, lv_pct(100), 46);
        lv_obj_set_style_bg_color(row, lv_color_hex(TH_INPUT), 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_pad_all(row, 8, 0);

        lv_obj_t *lbl = lv_label_create(row);
        char buf[64];
        snprintf(buf, sizeof(buf), "%s%s  %d dBm",
                 n.ssid, n.encrypted ? " " LV_SYMBOL_SETTINGS : "",
                 (int)n.rssi);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_color(lbl, lv_color_hex(TH_TEXT), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_add_event_cb(row, wifi_pick_btn_clicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
    }
}

static void wifi_modal_tick()
{
    if (!s_wifi_overlay) return;
    uint32_t now = millis();
    if (now - s_wifi_poll_ms < 200) return;
    s_wifi_poll_ms = now;

    if (s_scan_state == SCAN_RUNNING || s_scan_state == SCAN_REQUESTED) {
        if (s_wifi_status)
            lv_label_set_text(s_wifi_status, "Scanning...");
    } else if (s_scan_state == SCAN_FAIL) {
        if (s_wifi_status)
            lv_label_set_text(s_wifi_status, "Scan failed.");
        s_scan_state = SCAN_IDLE;
    } else if (s_scan_state == SCAN_DONE) {
        if (s_wifi_status) {
            char buf[48];
            snprintf(buf, sizeof(buf), "Found %d networks.", s_scan_count);
            lv_label_set_text(s_wifi_status, buf);
        }
        wifi_populate_list();
        s_scan_state = SCAN_IDLE;
    }
}

static void wifi_request_scan()
{
    ensure_aux_thread();
    s_scan_count = 0;
    s_scan_state = SCAN_REQUESTED;
    s_wifi_poll_ms = 0;
    if (s_wifi_status)
        lv_label_set_text(s_wifi_status, "Scanning...");
    if (s_wifi_list)
        lv_obj_clean(s_wifi_list);
}

static void wifi_scan_start_cb(lv_event_t *)
{
    wifi_request_scan();
}

static void wifi_delete_clicked_cb(lv_event_t *)
{
    bool changed = false;
    if (config.network.wifi_enabled) {
        config.network.wifi_enabled = false;
        changed = true;
    }
    if (config.network.wifi_ssid[0] != '\0') {
        config.network.wifi_ssid[0] = '\0';
        changed = true;
    }
    if (config.network.wifi_psk[0] != '\0') {
        config.network.wifi_psk[0] = '\0';
        changed = true;
    }

    if (s_wifi_enabled_sw)
        lv_obj_remove_state(s_wifi_enabled_sw, LV_STATE_CHECKED);

    if (changed) {
        LOG_INFO("mcui: WiFi credentials cleared (reboot pending)");
        cfg_mark_dirty();
    }

    if (s_wifi_status)
        lv_label_set_text(s_wifi_status, "Saved: WiFi credentials cleared.");
    refresh_values();
}

static void wifi_open_clicked_cb(lv_event_t *)
{
    if (s_wifi_overlay) return;

    lv_obj_t *scr = lv_screen_active();
    s_wifi_overlay = lv_obj_create(scr);
    lv_obj_remove_style_all(s_wifi_overlay);
    lv_obj_set_size(s_wifi_overlay, SCR_W, SCR_H);
    lv_obj_set_pos(s_wifi_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_wifi_overlay, lv_color_hex(TH_BG), 0);
    lv_obj_set_style_bg_opa(s_wifi_overlay, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_wifi_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_wifi_overlay);

    lv_obj_t *header = lv_obj_create(s_wifi_overlay);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, SCR_W, 56);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(TH_SURFACE), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(header, 8, 0);
    lv_obj_set_style_pad_right(header, 8, 0);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = lv_button_create(header);
    lv_obj_set_size(back, 86, 40);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(TH_INPUT), 0);
    lv_obj_set_style_radius(back, 0, 0);
    lv_obj_add_event_cb(back, wifi_screen_back_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *back_lbl = lv_label_create(back);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_16, 0);
    lv_obj_center(back_lbl);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "WiFi");
    lv_obj_set_style_text_color(title, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *body = lv_obj_create(s_wifi_overlay);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, SCR_W - 16, SCR_H - 64);
    lv_obj_set_pos(body, 8, 60);
    lv_obj_set_style_bg_color(body, lv_color_hex(TH_SURFACE), 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(body, 0, 0);
    lv_obj_set_style_pad_all(body, 12, 0);
    lv_obj_set_style_pad_row(body, 8, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);

    s_wifi_screen_ssid = add_info_row(body, "SSID", "-");
    s_wifi_enabled_sw = add_switch_row(body, "WiFi enabled", config.network.wifi_enabled,
                                       wifi_enabled_changed_cb);

    lv_obj_t *scan = lv_button_create(body);
    lv_obj_set_size(scan, lv_pct(100), 46);
    lv_obj_set_style_bg_color(scan, lv_color_hex(TH_ACCENT), 0);
    lv_obj_set_style_radius(scan, 0, 0);
    lv_obj_add_event_cb(scan, wifi_scan_start_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *scan_lbl = lv_label_create(scan);
    lv_label_set_text(scan_lbl, LV_SYMBOL_WIFI "  Scan networks");
    lv_obj_set_style_text_color(scan_lbl, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(scan_lbl, &lv_font_montserrat_16, 0);
    lv_obj_center(scan_lbl);

    lv_obj_t *del = lv_button_create(body);
    lv_obj_set_size(del, lv_pct(100), 46);
    lv_obj_set_style_bg_color(del, lv_color_hex(0xB83232), 0);
    lv_obj_set_style_radius(del, 0, 0);
    lv_obj_add_event_cb(del, wifi_delete_clicked_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *del_lbl = lv_label_create(del);
    lv_label_set_text(del_lbl, "Delete WiFi credentials");
    lv_obj_set_style_text_color(del_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(del_lbl, &lv_font_montserrat_16, 0);
    lv_obj_center(del_lbl);

    s_wifi_status = lv_label_create(body);
    lv_label_set_text(s_wifi_status, "Tap \"Scan networks\" to search.");
    lv_obj_set_style_text_color(s_wifi_status, lv_color_hex(TH_TEXT3), 0);
    lv_obj_set_style_text_font(s_wifi_status, &lv_font_montserrat_16, 0);
    lv_label_set_long_mode(s_wifi_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_wifi_status, lv_pct(100));

    s_wifi_list = lv_obj_create(body);
    lv_obj_remove_style_all(s_wifi_list);
    int wifi_list_h = SCR_H - 380;
    if (wifi_list_h < 160) wifi_list_h = 160;
    lv_obj_set_size(s_wifi_list, lv_pct(100), wifi_list_h);
    lv_obj_set_style_bg_opa(s_wifi_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_row(s_wifi_list, 4, 0);
    lv_obj_set_flex_flow(s_wifi_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_wifi_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_wifi_list, LV_SCROLLBAR_MODE_AUTO);
    refresh_values();
}

static lv_obj_t *s_mqtt_overlay = nullptr;
static lv_obj_t *s_mqtt_body = nullptr;
static lv_obj_t *s_mqtt_enabled_sw = nullptr;
static lv_obj_t *s_mqtt_primary_uplink_sw = nullptr;
static lv_obj_t *s_mqtt_primary_downlink_sw = nullptr;
static lv_obj_t *s_mqtt_addr_ta = nullptr;
static lv_obj_t *s_mqtt_user_ta = nullptr;
static lv_obj_t *s_mqtt_pass_ta = nullptr;
static lv_obj_t *s_mqtt_root_ta = nullptr;
static lv_obj_t *s_mqtt_encrypt_sw = nullptr;
static lv_obj_t *s_mqtt_json_sw = nullptr;
static lv_obj_t *s_mqtt_tls_sw = nullptr;

static void mqtt_screen_close()
{
    keyboard_hide();
    if (s_mqtt_overlay) {
        lv_obj_delete(s_mqtt_overlay);
        s_mqtt_overlay = nullptr;
    }
    s_mqtt_body = nullptr;
    s_mqtt_enabled_sw = nullptr;
    s_mqtt_primary_uplink_sw = nullptr;
    s_mqtt_primary_downlink_sw = nullptr;
    s_mqtt_addr_ta = nullptr;
    s_mqtt_user_ta = nullptr;
    s_mqtt_pass_ta = nullptr;
    s_mqtt_root_ta = nullptr;
    s_mqtt_encrypt_sw = nullptr;
    s_mqtt_json_sw = nullptr;
    s_mqtt_tls_sw = nullptr;
}

static void mqtt_screen_back_cb(lv_event_t *) { mqtt_screen_close(); }

static bool mqtt_is_descendant_of(lv_obj_t *obj, lv_obj_t *ancestor)
{
    if (!obj || !ancestor) return false;
    while (obj) {
        if (obj == ancestor) return true;
        obj = lv_obj_get_parent(obj);
    }
    return false;
}

static void mqtt_dismiss_keyboard_cb(lv_event_t *e)
{
    if (!keyboard_is_visible()) return;

    lv_obj_t *target = (lv_obj_t *)lv_event_get_target(e);
    if (target && lv_obj_check_type(target, &lv_textarea_class))
        return;

    lv_obj_t *kb = keyboard_get();
    if (kb && mqtt_is_descendant_of(target, kb))
        return;

    keyboard_hide();
    if (s_mqtt_body) {
        lv_obj_set_style_pad_bottom(s_mqtt_body, 12, 0);
        lv_obj_update_layout(s_mqtt_body);
    }
}

static void mqtt_bind_dismiss_recursive(lv_obj_t *obj)
{
    if (!obj) return;
    if (!lv_obj_check_type(obj, &lv_textarea_class))
        lv_obj_add_event_cb(obj, mqtt_dismiss_keyboard_cb, LV_EVENT_CLICKED, nullptr);

    uint32_t child_count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < child_count; ++i) {
        lv_obj_t *child = lv_obj_get_child(obj, (int32_t)i);
        mqtt_bind_dismiss_recursive(child);
    }
}

static void mqtt_scroll_field_visible(lv_obj_t *ta)
{
    if (!ta || !s_mqtt_body) return;

    lv_obj_update_layout(s_mqtt_body);
    lv_obj_scroll_to_view_recursive(ta, LV_ANIM_OFF);
    lv_obj_scroll_to_view(ta, LV_ANIM_OFF);

    lv_area_t a;
    lv_obj_get_coords(ta, &a);
    const int kb_top = SCR_H - keyboard_height();
    const int desired_margin = 20;
    const int overlap = (a.y2 + desired_margin) - kb_top;
    if (overlap > 0) {
        lv_obj_scroll_by(s_mqtt_body, 0, overlap + 8, LV_ANIM_OFF);
    }
}

static void mqtt_field_focus_cb(lv_event_t *e)
{
    lv_obj_t *ta = (lv_obj_t *)lv_event_get_target(e);
    keyboard_attach(ta);
    keyboard_show();
    if (s_mqtt_body) {
        lv_obj_set_style_pad_bottom(s_mqtt_body, keyboard_height() + 16, 0);
    }
    mqtt_scroll_field_visible(ta);
}

static void mqtt_copy_text(char *dst, size_t dst_sz, lv_obj_t *ta)
{
    const char *src = ta ? lv_textarea_get_text(ta) : "";
    if (!src) src = "";
    strncpy(dst, src, dst_sz - 1);
    dst[dst_sz - 1] = '\0';
}

static lv_obj_t *mqtt_add_text_field(lv_obj_t *parent, const char *label,
                                     const char *initial, int max_len, bool password)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 76);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, lv_color_hex(TH_TEXT2), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *ta = lv_textarea_create(row);
    lv_obj_set_size(ta, lv_pct(100), 44);
    lv_obj_align(ta, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_text(ta, initial ? initial : "");
    lv_textarea_set_max_length(ta, max_len);
    lv_obj_set_style_bg_color(ta, lv_color_hex(TH_INPUT), 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_border_width(ta, 0, 0);
    lv_obj_set_style_radius(ta, 0, 0);
    lv_obj_set_style_anim_duration(ta, 0, LV_PART_CURSOR);
    if (password) lv_textarea_set_password_mode(ta, true);
    lv_obj_add_event_cb(ta, mqtt_field_focus_cb, LV_EVENT_FOCUSED, nullptr);
    return ta;
}

static void mqtt_switch_row_clicked_cb(lv_event_t *e)
{
    if (lv_event_get_target(e) != lv_event_get_current_target(e))
        return;

    lv_obj_t *sw = (lv_obj_t *)lv_event_get_user_data(e);
    if (!sw) return;
    if (lv_obj_has_state(sw, LV_STATE_DISABLED)) return;

    if (lv_obj_has_state(sw, LV_STATE_CHECKED))
        lv_obj_remove_state(sw, LV_STATE_CHECKED);
    else
        lv_obj_add_state(sw, LV_STATE_CHECKED);

    lv_obj_send_event(sw, LV_EVENT_VALUE_CHANGED, nullptr);
}

static void mqtt_make_switch_row_clickable(lv_obj_t *sw)
{
    if (!sw) return;
    lv_obj_t *row = lv_obj_get_parent(sw);
    if (!row) return;
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(row, lv_color_hex(TH_INPUT), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_set_style_radius(row, 0, LV_STATE_PRESSED);
    lv_obj_add_event_cb(row, mqtt_switch_row_clicked_cb, LV_EVENT_CLICKED, sw);
}

static void mqtt_screen_save_cb(lv_event_t *)
{
    moduleConfig.has_mqtt = true;
    moduleConfig.mqtt.enabled = s_mqtt_enabled_sw && lv_obj_has_state(s_mqtt_enabled_sw, LV_STATE_CHECKED);
    moduleConfig.mqtt.encryption_enabled = s_mqtt_encrypt_sw && lv_obj_has_state(s_mqtt_encrypt_sw, LV_STATE_CHECKED);
    moduleConfig.mqtt.json_enabled = s_mqtt_json_sw && lv_obj_has_state(s_mqtt_json_sw, LV_STATE_CHECKED);
    moduleConfig.mqtt.tls_enabled = s_mqtt_tls_sw && lv_obj_has_state(s_mqtt_tls_sw, LV_STATE_CHECKED);

    mqtt_copy_text(moduleConfig.mqtt.address, sizeof(moduleConfig.mqtt.address), s_mqtt_addr_ta);
    mqtt_copy_text(moduleConfig.mqtt.username, sizeof(moduleConfig.mqtt.username), s_mqtt_user_ta);
    mqtt_copy_text(moduleConfig.mqtt.password, sizeof(moduleConfig.mqtt.password), s_mqtt_pass_ta);
    // No forced default: an empty root topic stays empty so the device does
    // not auto-subscribe to the public regional firehose (msh/EU_868/...),
    // whose volume floods this board's (post-framebuffer corrupted) PSRAM
    // heap. The user types their own broker namespace.
    mqtt_copy_text(moduleConfig.mqtt.root, sizeof(moduleConfig.mqtt.root), s_mqtt_root_ta);

    // CrowPanel P4 should use direct MQTT, not client proxy.
    moduleConfig.mqtt.proxy_to_client_enabled = false;

    bool pri_up = s_mqtt_primary_uplink_sw && lv_obj_has_state(s_mqtt_primary_uplink_sw, LV_STATE_CHECKED);
    bool pri_dn = s_mqtt_primary_downlink_sw && lv_obj_has_state(s_mqtt_primary_downlink_sw, LV_STATE_CHECKED);
    bool primary_changed = false;

    meshtastic_Channel ch0 = channels.getByIndex(0);
    if (ch0.role != meshtastic_Channel_Role_DISABLED) {
        if (!ch0.has_settings) {
            ch0.has_settings = true;
            ch0.settings = meshtastic_ChannelSettings_init_default;
        }
        if (ch0.settings.uplink_enabled != pri_up || ch0.settings.downlink_enabled != pri_dn) {
            ch0.settings.uplink_enabled = pri_up;
            ch0.settings.downlink_enabled = pri_dn;
            channels.setChannel(ch0);
            channels.onConfigChanged();
            primary_changed = true;
        }
    }

    int save_what = SEGMENT_MODULECONFIG;
    if (primary_changed) save_what |= SEGMENT_CHANNELS;

    if (nodeDB) nodeDB->saveToDisk(save_what);
    if (service) service->reloadConfig(save_what);

    LOG_INFO("mcui: MQTT settings saved (enabled=%d, primary up=%d, primary down=%d, root=%s)",
             moduleConfig.mqtt.enabled ? 1 : 0, pri_up ? 1 : 0, pri_dn ? 1 : 0, moduleConfig.mqtt.root);
    mqtt_screen_close();
}

static void mqtt_open_clicked_cb(lv_event_t *)
{
    if (s_mqtt_overlay) return;

    // Prefill root topic with saved value, or the standard default.
    const char *root_initial = (moduleConfig.mqtt.root[0] != '\0') ? moduleConfig.mqtt.root : default_mqtt_root;

    meshtastic_Channel ch0 = channels.getByIndex(0);
    bool primary_uplink = ch0.has_settings && ch0.settings.uplink_enabled;
    bool primary_downlink = ch0.has_settings && ch0.settings.downlink_enabled;

    lv_obj_t *scr = lv_screen_active();
    s_mqtt_overlay = lv_obj_create(scr);
    lv_obj_remove_style_all(s_mqtt_overlay);
    lv_obj_set_size(s_mqtt_overlay, SCR_W, SCR_H);
    lv_obj_set_pos(s_mqtt_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_mqtt_overlay, lv_color_hex(TH_BG), 0);
    lv_obj_set_style_bg_opa(s_mqtt_overlay, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_mqtt_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_mqtt_overlay);

    lv_obj_t *header = lv_obj_create(s_mqtt_overlay);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, SCR_W, 56);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(TH_SURFACE), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(header, 8, 0);
    lv_obj_set_style_pad_right(header, 8, 0);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = lv_button_create(header);
    lv_obj_set_size(back, 86, 40);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(TH_INPUT), 0);
    lv_obj_set_style_radius(back, 0, 0);
    lv_obj_add_event_cb(back, mqtt_screen_back_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *back_lbl = lv_label_create(back);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_16, 0);
    lv_obj_center(back_lbl);

    lv_obj_t *save = lv_button_create(header);
    lv_obj_set_size(save, 86, 40);
    lv_obj_align(save, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(save, lv_color_hex(TH_ACCENT), 0);
    lv_obj_set_style_radius(save, 0, 0);
    lv_obj_add_event_cb(save, mqtt_screen_save_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *save_lbl = lv_label_create(save);
    lv_label_set_text(save_lbl, "Save");
    lv_obj_set_style_text_color(save_lbl, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(save_lbl, &lv_font_montserrat_16, 0);
    lv_obj_center(save_lbl);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "MQTT");
    lv_obj_set_style_text_color(title, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    s_mqtt_body = lv_obj_create(s_mqtt_overlay);
    lv_obj_remove_style_all(s_mqtt_body);
    lv_obj_set_size(s_mqtt_body, SCR_W - 16, SCR_H - 64);
    lv_obj_set_pos(s_mqtt_body, 8, 60);
    lv_obj_set_style_bg_color(s_mqtt_body, lv_color_hex(TH_SURFACE), 0);
    lv_obj_set_style_bg_opa(s_mqtt_body, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_mqtt_body, 0, 0);
    lv_obj_set_style_pad_all(s_mqtt_body, 12, 0);
    lv_obj_set_style_pad_row(s_mqtt_body, 8, 0);
    lv_obj_set_flex_flow(s_mqtt_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_mqtt_body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_mqtt_body, LV_SCROLLBAR_MODE_AUTO);

    s_mqtt_enabled_sw = add_switch_row(s_mqtt_body, "MQTT enabled", moduleConfig.mqtt.enabled, nullptr);
    s_mqtt_primary_uplink_sw = add_switch_row(s_mqtt_body, "Primary channel uplink", primary_uplink, nullptr);
    s_mqtt_primary_downlink_sw = add_switch_row(s_mqtt_body, "Primary channel downlink", primary_downlink, nullptr);
    mqtt_make_switch_row_clickable(s_mqtt_enabled_sw);
    mqtt_make_switch_row_clickable(s_mqtt_primary_uplink_sw);
    mqtt_make_switch_row_clickable(s_mqtt_primary_downlink_sw);

    s_mqtt_addr_ta = mqtt_add_text_field(s_mqtt_body, "MQTT address",
                                         moduleConfig.mqtt.address, sizeof(moduleConfig.mqtt.address) - 1, false);
    s_mqtt_user_ta = mqtt_add_text_field(s_mqtt_body, "Username",
                                         moduleConfig.mqtt.username, sizeof(moduleConfig.mqtt.username) - 1, false);
    s_mqtt_pass_ta = mqtt_add_text_field(s_mqtt_body, "Password",
                                         moduleConfig.mqtt.password, sizeof(moduleConfig.mqtt.password) - 1, true);
    s_mqtt_root_ta = mqtt_add_text_field(s_mqtt_body, "Root topic",
                                         root_initial, sizeof(moduleConfig.mqtt.root) - 1, false);

    s_mqtt_encrypt_sw = add_switch_row(s_mqtt_body, "Encryption enabled", moduleConfig.mqtt.encryption_enabled, nullptr);
    s_mqtt_json_sw = add_switch_row(s_mqtt_body, "JSON output enabled", moduleConfig.mqtt.json_enabled, nullptr);
    s_mqtt_tls_sw = add_switch_row(s_mqtt_body, "TLS enabled", moduleConfig.mqtt.tls_enabled, nullptr);
    mqtt_make_switch_row_clickable(s_mqtt_encrypt_sw);
    mqtt_make_switch_row_clickable(s_mqtt_json_sw);
    mqtt_make_switch_row_clickable(s_mqtt_tls_sw);

    lv_obj_t *proxy_note = lv_label_create(s_mqtt_body);
    lv_label_set_text(proxy_note, "Client proxy is disabled on CrowPanel (direct MQTT).");
    lv_obj_set_style_text_color(proxy_note, lv_color_hex(TH_TEXT3), 0);
    lv_obj_set_style_text_font(proxy_note, &lv_font_montserrat_16, 0);
    lv_label_set_long_mode(proxy_note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(proxy_note, lv_pct(100));

    mqtt_bind_dismiss_recursive(s_mqtt_overlay);
}

static void region_changed_cb(lv_event_t *e)
{
    uint16_t idx = lv_dropdown_get_selected((lv_obj_t *)lv_event_get_target(e));
    if (idx >= REGION_COUNT) return;
    auto sel = (meshtastic_Config_LoRaConfig_RegionCode)REGIONS[idx].code;
    if (sel == config.lora.region) return;
    config.lora.region = sel;
    cfg_mark_dirty();
}
static void preset_changed_cb(lv_event_t *e)
{
    uint16_t idx = lv_dropdown_get_selected((lv_obj_t *)lv_event_get_target(e));
    if (idx >= PRESET_COUNT) return;
    auto sel = (meshtastic_Config_LoRaConfig_ModemPreset)PRESETS[idx].code;
    if (sel == config.lora.modem_preset && config.lora.use_preset) return;
    config.lora.use_preset   = true;
    config.lora.modem_preset = sel;
    cfg_mark_dirty();
}
static void hop_limit_changed_cb(lv_event_t *e)
{
    uint16_t idx = lv_dropdown_get_selected((lv_obj_t *)lv_event_get_target(e));
    if (idx >= HOP_LIMIT_COUNT) return;
    uint32_t v = (uint32_t)HOP_LIMITS[idx].code;
    if (v == config.lora.hop_limit) return;
    config.lora.hop_limit = v;
    cfg_mark_dirty();
}
static void tx_power_changed_cb(lv_event_t *e)
{
    uint16_t idx = lv_dropdown_get_selected((lv_obj_t *)lv_event_get_target(e));
    if (idx >= TX_POWER_COUNT) return;
    int8_t v = (int8_t)TX_POWERS[idx].code;
    if (v == config.lora.tx_power) return;
    config.lora.tx_power = v;
    cfg_mark_dirty();
}
static void tx_enabled_changed_cb(lv_event_t *e)
{
    lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
    config.lora.tx_enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    cfg_mark_dirty();
}

static void rx_boost_changed_cb(lv_event_t *e)
{
    lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
    config.lora.sx126x_rx_boosted_gain = lv_obj_has_state(sw, LV_STATE_CHECKED);
    cfg_mark_dirty();
}

static void override_duty_changed_cb(lv_event_t *e)
{
    lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
    config.lora.override_duty_cycle = lv_obj_has_state(sw, LV_STATE_CHECKED);
    cfg_mark_dirty();
}

static void role_changed_cb(lv_event_t *e)
{
    uint16_t idx = lv_dropdown_get_selected((lv_obj_t *)lv_event_get_target(e));
    if (idx >= ROLE_COUNT) return;
    auto sel = (meshtastic_Config_DeviceConfig_Role)ROLES[idx].code;
    if (sel == config.device.role) return;
    config.device.role = sel;
    cfg_mark_dirty();
}
static void rebroadcast_changed_cb(lv_event_t *e)
{
    uint16_t idx = lv_dropdown_get_selected((lv_obj_t *)lv_event_get_target(e));
    if (idx >= REBROADCAST_COUNT) return;
    auto sel = (meshtastic_Config_DeviceConfig_RebroadcastMode)REBROADCAST_MODES[idx].code;
    if (sel == config.device.rebroadcast_mode) return;
    config.device.rebroadcast_mode = sel;
    cfg_mark_dirty();
}
static void node_info_interval_changed_cb(lv_event_t *e)
{
    uint16_t idx = lv_dropdown_get_selected((lv_obj_t *)lv_event_get_target(e));
    if (idx >= INTERVAL_SHORT_COUNT) return;
    uint32_t s = INTERVAL_SHORT[idx].secs;
    if (s == config.device.node_info_broadcast_secs) return;
    config.device.node_info_broadcast_secs = s;
    cfg_mark_dirty();
}

static void screen_timeout_changed_cb(lv_event_t *e)
{
    uint16_t idx = lv_dropdown_get_selected((lv_obj_t *)lv_event_get_target(e));
    if (idx >= SCREEN_TIMEOUT_COUNT) return;
    uint32_t s = SCREEN_TIMEOUT[idx].secs;
    if (s == config.display.screen_on_secs) return;
    config.display.screen_on_secs = s;

    backlight_set_timeout_secs(s);
    cfg_mark_save_only();
}
static void units_changed_cb(lv_event_t *e)
{
    uint16_t idx = lv_dropdown_get_selected((lv_obj_t *)lv_event_get_target(e));
    if (idx >= UNITS_COUNT) return;
    auto sel = (meshtastic_Config_DisplayConfig_DisplayUnits)UNITS[idx].code;
    if (sel == config.display.units) return;
    config.display.units = sel;
    cfg_mark_dirty();
}

static void timezone_changed_cb(lv_event_t *e)
{
    uint16_t idx = lv_dropdown_get_selected((lv_obj_t *)lv_event_get_target(e));
    if (idx >= (uint16_t)TZ_COUNT) return;
    if ((int)idx == s_tz_index) return;
    s_tz_index = (int)idx;
    tz_save_to_nvs(s_tz_index);
    tz_apply_to_libc();

    time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    LOG_INFO("mcui: timezone picked → %s | local=%02d:%02d:%02d",
             TZ_LIST[idx].label, lt.tm_hour, lt.tm_min, lt.tm_sec);
}

static bool orientation_target_landscape()
{
    return s_orientation_dirty ? s_pending_landscape : landscape_active();
}

static void orientation_toggle_clicked_cb(lv_event_t *)
{
    bool target_landscape = !orientation_target_landscape();
    if (s_orientation_dirty && s_pending_landscape == target_landscape && s_orientation_overlay)
        return;

    cfg_mark_orientation(target_landscape);
    if (s_orientation_dirty)
        orientation_overlay_open();
    refresh_values();
}

static void wifi_enabled_changed_cb(lv_event_t *e)
{
    lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
    bool enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (enabled && config.network.wifi_ssid[0] == '\0') {

        lv_obj_remove_state(sw, LV_STATE_CHECKED);
        wifi_request_scan();
        return;
    }
    if (config.network.wifi_enabled == enabled) return;
    config.network.wifi_enabled = enabled;
    cfg_mark_dirty();
}

static void owner_long_entered(const char *text)
{
    if (!text || !*text) return;
    ensure_aux_thread();
    queue_owner_edit(text, nullptr);
}
static void owner_short_entered(const char *text)
{
    if (!text || !*text) return;
    ensure_aux_thread();
    queue_owner_edit(nullptr, text);
}
static void owner_long_tap_cb(lv_event_t *)
{
    text_edit_modal("Edit long name", owner.long_name, 39,
                     false, owner_long_entered);
}
static void owner_short_tap_cb(lv_event_t *)
{
    text_edit_modal("Edit short name (max 4)", owner.short_name, 4,
                     false, owner_short_entered);
}

static void format_coord(char *out, size_t out_sz, int32_t coord_i)
{
    double v = (double)coord_i / 1e7;
    snprintf(out, out_sz, "%.6f", v);
}

static void refresh_position_labels()
{
    if (!nodeDB) return;
    const meshtastic_PositionLite &pos =
        nodeDB->getMeshNode(nodeDB->getNodeNum())->position;
    bool have_saved = (config.position.fixed_position &&
                       (pos.latitude_i != 0 || pos.longitude_i != 0));

    char buf[32];
    if (s_lbl_pos_lat) {
        if (s_pos_lat_pending[0]) {
            lv_label_set_text(s_lbl_pos_lat, s_pos_lat_pending);
        } else if (have_saved) {
            format_coord(buf, sizeof(buf), pos.latitude_i);
            lv_label_set_text(s_lbl_pos_lat, buf);
        } else {
            lv_label_set_text(s_lbl_pos_lat, "(tap to set)");
        }
    }
    if (s_lbl_pos_lon) {
        if (s_pos_lon_pending[0]) {
            lv_label_set_text(s_lbl_pos_lon, s_pos_lon_pending);
        } else if (have_saved) {
            format_coord(buf, sizeof(buf), pos.longitude_i);
            lv_label_set_text(s_lbl_pos_lon, buf);
        } else {
            lv_label_set_text(s_lbl_pos_lon, "(tap to set)");
        }
    }
    if (s_lbl_pos_alt) {
        if (s_pos_alt_pending[0]) {
            lv_label_set_text(s_lbl_pos_alt, s_pos_alt_pending);
        } else if (have_saved && pos.altitude != 0) {
            snprintf(buf, sizeof(buf), "%d m", (int)pos.altitude);
            lv_label_set_text(s_lbl_pos_alt, buf);
        } else {
            lv_label_set_text(s_lbl_pos_alt, "(optional)");
        }
    }
}

static bool parse_decimal_in_range(const char *s, double lo, double hi, double *out)
{
    if (!s || !*s) return false;
    char *end = nullptr;
    double v = strtod(s, &end);
    if (end == s) return false;
    if (!isfinite(v)) return false;
    if (v < lo || v > hi) return false;

    while (*end) {
        if (*end != ' ' && *end != '\t' && *end != '\r' && *end != '\n')
            return false;
        end++;
    }
    *out = v;
    return true;
}

static void position_lat_entered(const char *text)
{
    if (!text) return;
    strncpy(s_pos_lat_pending, text, sizeof(s_pos_lat_pending) - 1);
    s_pos_lat_pending[sizeof(s_pos_lat_pending) - 1] = '\0';
    refresh_position_labels();
}
static void position_lon_entered(const char *text)
{
    if (!text) return;
    strncpy(s_pos_lon_pending, text, sizeof(s_pos_lon_pending) - 1);
    s_pos_lon_pending[sizeof(s_pos_lon_pending) - 1] = '\0';
    refresh_position_labels();
}
static void position_alt_entered(const char *text)
{
    if (!text) return;
    strncpy(s_pos_alt_pending, text, sizeof(s_pos_alt_pending) - 1);
    s_pos_alt_pending[sizeof(s_pos_alt_pending) - 1] = '\0';
    refresh_position_labels();
}

static void position_lat_tap_cb(lv_event_t *)
{
    text_edit_modal("Latitude (decimal degrees, e.g. 37.983810)",
                    s_pos_lat_pending, 23,
                     false, position_lat_entered);
}
static void position_lon_tap_cb(lv_event_t *)
{
    text_edit_modal("Longitude (decimal degrees, e.g. 23.727539)",
                    s_pos_lon_pending, 23,
                     false, position_lon_entered);
}
static void position_alt_tap_cb(lv_event_t *)
{
    text_edit_modal("Altitude (metres, optional)",
                    s_pos_alt_pending, 23,
                     false, position_alt_entered);
}

static void position_status(const char *msg, bool ok)
{
    if (!s_lbl_pos_status) return;
    lv_label_set_text(s_lbl_pos_status, msg);
    lv_obj_set_style_text_color(s_lbl_pos_status,
                                lv_color_hex(ok ? 0x45D483 : 0xE05050), 0);
    lv_obj_remove_flag(s_lbl_pos_status, LV_OBJ_FLAG_HIDDEN);
}

static void position_save_clicked_cb(lv_event_t *)
{

    const meshtastic_PositionLite *saved = nullptr;
    if (nodeDB) saved = &nodeDB->getMeshNode(nodeDB->getNodeNum())->position;

    double lat = 0, lon = 0;
    bool have_lat = false, have_lon = false;
    if (s_pos_lat_pending[0]) {
        if (!parse_decimal_in_range(s_pos_lat_pending, -90.0, 90.0, &lat)) {
            position_status("Latitude must be -90..+90 (e.g. 37.983810)", false);
            return;
        }
        have_lat = true;
    } else if (saved && config.position.fixed_position && saved->latitude_i != 0) {
        lat = (double)saved->latitude_i / 1e7;
        have_lat = true;
    }
    if (s_pos_lon_pending[0]) {
        if (!parse_decimal_in_range(s_pos_lon_pending, -180.0, 180.0, &lon)) {
            position_status("Longitude must be -180..+180 (e.g. 23.727539)", false);
            return;
        }
        have_lon = true;
    } else if (saved && config.position.fixed_position && saved->longitude_i != 0) {
        lon = (double)saved->longitude_i / 1e7;
        have_lon = true;
    }

    if (!have_lat || !have_lon) {
        position_status("Enter both latitude and longitude", false);
        return;
    }

    bool have_alt = false;
    int32_t alt_m = 0;
    if (s_pos_alt_pending[0]) {
        double a = 0;
        if (!parse_decimal_in_range(s_pos_alt_pending, -500.0, 9000.0, &a)) {
            position_status("Altitude must be -500..+9000 m (or leave blank)", false);
            return;
        }
        alt_m = (int32_t)llround(a);
        have_alt = true;
    } else if (saved && config.position.fixed_position && saved->altitude != 0) {
        alt_m = saved->altitude;
        have_alt = true;
    }

    ensure_aux_thread();
    queue_position_apply(lat, lon, have_alt, alt_m);

    s_pos_lat_pending[0] = '\0';
    s_pos_lon_pending[0] = '\0';
    s_pos_alt_pending[0] = '\0';
    char msg[80];
    if (have_alt)
        snprintf(msg, sizeof(msg), "Saved: %.6f, %.6f @ %d m — broadcasting",
                 lat, lon, (int)alt_m);
    else
        snprintf(msg, sizeof(msg), "Saved: %.6f, %.6f — broadcasting",
                 lat, lon);
    position_status(msg, true);
    refresh_position_labels();
}

static void position_clear_clicked_cb(lv_event_t *)
{
    s_pos_lat_pending[0] = '\0';
    s_pos_lon_pending[0] = '\0';
    s_pos_alt_pending[0] = '\0';
    ensure_aux_thread();
    queue_position_clear();
    position_status("Position cleared", true);
    refresh_position_labels();
}

static void reboot_clicked_cb(lv_event_t *)
{
    LOG_INFO("mcui: user-requested reboot in 1 s");
    rebootAtMsec = millis() + 1000;
}
static void shutdown_clicked_cb(lv_event_t *)
{
    LOG_INFO("mcui: user-requested shutdown in 1 s");
    shutdownAtMsec = millis() + 1000;
}

static void clear_nodes_confirm_cb(lv_event_t *e)
{
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_current_target(e);
    lv_obj_t *mbox = lv_obj_get_parent(lv_obj_get_parent(btn));
    bool do_clear = (btn == (lv_obj_t *)lv_event_get_user_data(e));
    if (do_clear) {
        LOG_WARN("mcui: clear all nodes queued");
        node_actions_clear_all();
    }
    if (mbox)
        lv_obj_delete(mbox);
}

static void clear_nodes_clicked_cb(lv_event_t *)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_t *dim = lv_obj_create(scr);
    lv_obj_remove_style_all(dim);
    lv_obj_set_size(dim, SCR_W, SCR_H);
    lv_obj_set_pos(dim, 0, 0);
    lv_obj_set_style_bg_color(dim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(dim, LV_OPA_60, 0);
    lv_obj_remove_flag(dim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(dim);

    lv_obj_t *card = lv_obj_create(dim);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 420, 220);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(TH_SURFACE), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 0, 0);
    lv_obj_set_style_pad_all(card, 18, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Delete all nodes?");
    lv_obj_set_style_text_color(title, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *body = lv_label_create(card);
    lv_label_set_text(body, "This clears the node database, including favourites.\nYour owner/config/channels stay intact.");
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, lv_pct(100));
    lv_obj_set_style_text_color(body, lv_color_hex(TH_TEXT2), 0);
    lv_obj_set_style_text_font(body, &lv_font_montserrat_16, 0);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, 34);

    lv_obj_t *cancel = lv_button_create(card);
    lv_obj_set_size(cancel, 150, 42);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(TH_INPUT), 0);
    lv_obj_set_style_radius(cancel, 0, 0);
    lv_obj_t *cl = lv_label_create(cancel);
    lv_label_set_text(cl, "Cancel");
    lv_obj_set_style_text_color(cl, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(cl, &lv_font_montserrat_16, 0);
    lv_obj_center(cl);

    lv_obj_t *erase = lv_button_create(card);
    lv_obj_set_size(erase, 150, 42);
    lv_obj_align(erase, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(erase, lv_color_hex(0xB83232), 0);
    lv_obj_set_style_radius(erase, 0, 0);
    lv_obj_t *el = lv_label_create(erase);
    lv_label_set_text(el, "Delete all");
    lv_obj_set_style_text_color(el, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(el, &lv_font_montserrat_16, 0);
    lv_obj_center(el);

    lv_obj_add_event_cb(cancel, clear_nodes_confirm_cb, LV_EVENT_CLICKED, erase);
    lv_obj_add_event_cb(erase, clear_nodes_confirm_cb, LV_EVENT_CLICKED, erase);
}

static void regenerate_keys_confirm_cb(lv_event_t *e)
{
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_current_target(e);
    lv_obj_t *mbox = lv_obj_get_parent(lv_obj_get_parent(btn));

    bool do_regen = (btn == (lv_obj_t *)lv_event_get_user_data(e));
    if (do_regen) {
        LOG_WARN("mcui: private key regeneration queued");
        ensure_aux_thread();
        queue_regenerate_private_keys();
    }
    if (mbox)
        lv_obj_delete(mbox);
}

static void regenerate_keys_clicked_cb(lv_event_t *)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_t *dim = lv_obj_create(scr);
    lv_obj_remove_style_all(dim);
    lv_obj_set_size(dim, SCR_W, SCR_H);
    lv_obj_set_pos(dim, 0, 0);
    lv_obj_set_style_bg_color(dim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(dim, LV_OPA_60, 0);
    lv_obj_remove_flag(dim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(dim);

    lv_obj_t *card = lv_obj_create(dim);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 440, 240);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(TH_SURFACE), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 0, 0);
    lv_obj_set_style_pad_all(card, 18, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Regenerate private keys?");
    lv_obj_set_style_text_color(title, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *body = lv_label_create(card);
    lv_label_set_text(body,
        "This creates a new public/private key pair.\n"
        "Existing encrypted direct-message trust may need to be re-established.");
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, lv_pct(100));
    lv_obj_set_style_text_color(body, lv_color_hex(TH_TEXT2), 0);
    lv_obj_set_style_text_font(body, &lv_font_montserrat_16, 0);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, 34);

    lv_obj_t *cancel = lv_button_create(card);
    lv_obj_set_size(cancel, 150, 42);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(TH_INPUT), 0);
    lv_obj_set_style_radius(cancel, 0, 0);
    lv_obj_t *cl = lv_label_create(cancel);
    lv_label_set_text(cl, "Cancel");
    lv_obj_set_style_text_color(cl, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(cl, &lv_font_montserrat_16, 0);
    lv_obj_center(cl);

    lv_obj_t *regen = lv_button_create(card);
    lv_obj_set_size(regen, 170, 42);
    lv_obj_align(regen, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(regen, lv_color_hex(0xB87532), 0);
    lv_obj_set_style_radius(regen, 0, 0);
    lv_obj_t *rl = lv_label_create(regen);
    lv_label_set_text(rl, "Regenerate");
    lv_obj_set_style_text_color(rl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(rl, &lv_font_montserrat_16, 0);
    lv_obj_center(rl);

    lv_obj_add_event_cb(cancel, regenerate_keys_confirm_cb, LV_EVENT_CLICKED, regen);
    lv_obj_add_event_cb(regen, regenerate_keys_confirm_cb, LV_EVENT_CLICKED, regen);
}

static void factory_reset_confirm_cb(lv_event_t *e)
{
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t *mbox = lv_obj_get_parent(lv_obj_get_parent(btn));

    bool do_erase = (btn == (lv_obj_t *)lv_event_get_user_data(e));
    if (do_erase) {
        LOG_WARN("mcui: factory reset queued — will run on main loop");
        ensure_cfg_thread();
        s_factory_reset_req = true;
    }
    if (mbox) lv_obj_delete(mbox);
}

static void factory_reset_clicked_cb(lv_event_t *)
{

    lv_obj_t *scr = lv_screen_active();
    lv_obj_t *dim = lv_obj_create(scr);
    lv_obj_remove_style_all(dim);
    lv_obj_set_size(dim, SCR_W, SCR_H);
    lv_obj_set_pos(dim, 0, 0);
    lv_obj_set_style_bg_color(dim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(dim, LV_OPA_60, 0);
    lv_obj_remove_flag(dim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(dim);

    lv_obj_t *card = lv_obj_create(dim);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 380, 220);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(TH_SURFACE), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 0, 0);
    lv_obj_set_style_pad_all(card, 18, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Factory reset?");
    lv_obj_set_style_text_color(title, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *body = lv_label_create(card);
    lv_label_set_text(body,
        "This erases all config, channels, owner info and the node DB.\n"
        "The device will reboot.");
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, lv_pct(100));
    lv_obj_set_style_text_color(body, lv_color_hex(TH_TEXT2), 0);
    lv_obj_set_style_text_font(body, &lv_font_montserrat_16, 0);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, 32);

    lv_obj_t *cancel = lv_button_create(card);
    lv_obj_set_size(cancel, 140, 42);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(TH_INPUT), 0);
    lv_obj_set_style_radius(cancel, 0, 0);
    lv_obj_t *cl = lv_label_create(cancel);
    lv_label_set_text(cl, "Cancel");
    lv_obj_set_style_text_color(cl, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(cl, &lv_font_montserrat_16, 0);
    lv_obj_center(cl);

    lv_obj_t *erase = lv_button_create(card);
    lv_obj_set_size(erase, 140, 42);
    lv_obj_align(erase, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(erase, lv_color_hex(0xB83232), 0);
    lv_obj_set_style_radius(erase, 0, 0);
    lv_obj_t *el = lv_label_create(erase);
    lv_label_set_text(el, "Erase");
    lv_obj_set_style_text_color(el, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(el, &lv_font_montserrat_16, 0);
    lv_obj_center(el);

    lv_obj_add_event_cb(cancel, factory_reset_confirm_cb, LV_EVENT_CLICKED, erase);
    lv_obj_add_event_cb(erase,  factory_reset_confirm_cb, LV_EVENT_CLICKED, erase);
}

static void refresh_values()
{
    if (s_lbl_firmware) lv_label_set_text(s_lbl_firmware, optstr(APP_VERSION));

    char buf[96];

    if (s_lbl_owner) {
        const char *ln = owner.long_name[0]  ? owner.long_name  : "(unset)";
        const char *sn = owner.short_name[0] ? owner.short_name : "--";
        snprintf(buf, sizeof(buf), "%s (%s)", ln, sn);
        lv_label_set_text(s_lbl_owner, buf);
    }
    if (s_lbl_nodeid && nodeDB) {
        snprintf(buf, sizeof(buf), "!%08x", (unsigned)nodeDB->getNodeNum());
        lv_label_set_text(s_lbl_nodeid, buf);
    }
    if (s_lbl_region_now)
        lv_label_set_text(s_lbl_region_now,
                          enum_label(REGIONS, REGION_COUNT, config.lora.region));
    if (s_lbl_preset_now)
        lv_label_set_text(s_lbl_preset_now,
                          enum_label(PRESETS, PRESET_COUNT, config.lora.modem_preset));
    if (s_lbl_uptime) {
        uint32_t s = (uint32_t)(millis() / 1000);
        uint32_t h = s / 3600, m = (s % 3600) / 60, sec = s % 60;
        snprintf(buf, sizeof(buf), "%uh %02um %02us",
                 (unsigned)h, (unsigned)m, (unsigned)sec);
        lv_label_set_text(s_lbl_uptime, buf);
    }
    if (s_lbl_heap) {
        uint32_t free_heap = memGet.getFreeHeap();
        snprintf(buf, sizeof(buf), "%u KB", (unsigned)(free_heap / 1024));
        lv_label_set_text(s_lbl_heap, buf);
    }
    if (s_lbl_battery) {
        if (powerStatus) {
            uint8_t pct = powerStatus->getBatteryChargePercent();
            if (pct > 100) snprintf(buf, sizeof(buf), "ext");
            else           snprintf(buf, sizeof(buf), "%u%%", (unsigned)pct);
        } else {
            snprintf(buf, sizeof(buf), "—");
        }
        lv_label_set_text(s_lbl_battery, buf);
    }
    if (s_lbl_noise) {

        float nf = 0.0f;
        if (router && router->getInterface()) nf = router->getInterface()->getNoiseFloor();
        if (nf < -1.0f) {

            snprintf(buf, sizeof(buf), "%d dBm", (int)lroundf(nf));
        } else {
            snprintf(buf, sizeof(buf), "—");
        }
        lv_label_set_text(s_lbl_noise, buf);
    }
    const char *wifi_ssid = config.network.wifi_ssid[0] ? config.network.wifi_ssid : "(unset)";
    bool wifi_connected = config.network.wifi_enabled && WiFi.status() == WL_CONNECTED;
    if (s_lbl_wifi_ssid) {
        lv_label_set_text(s_lbl_wifi_ssid, wifi_ssid);
        lv_obj_set_style_text_color(s_lbl_wifi_ssid,
                                    lv_color_hex(wifi_connected ? 0x45D483 : TH_TEXT), 0);
    }
    if (s_wifi_screen_ssid) {
        lv_label_set_text(s_wifi_screen_ssid, wifi_ssid);
        lv_obj_set_style_text_color(s_wifi_screen_ssid,
                                    lv_color_hex(wifi_connected ? 0x45D483 : TH_TEXT), 0);
    }
    if (s_lbl_orientation) {
        lv_label_set_text(s_lbl_orientation,
                          s_orientation_dirty
                              ? (s_pending_landscape ? "Landscape (pending reboot)"
                                                     : "Portrait (pending reboot)")
                              : (landscape_active() ? "Landscape" : "Portrait"));
    }
    if (s_btn_orientation_label) {
        lv_label_set_text(s_btn_orientation_label,
                          orientation_target_landscape()
                              ? "Switch to portrait mode"
                              : "Switch to landscape mode");
    }
    if (s_lbl_owner_long)
        lv_label_set_text(s_lbl_owner_long,
                          owner.long_name[0] ? owner.long_name : "(tap to set)");
    if (s_lbl_owner_short)
        lv_label_set_text(s_lbl_owner_short,
                          owner.short_name[0] ? owner.short_name : "(tap to set)");

    refresh_position_labels();

    wifi_modal_tick();

    if (s_lbl_pending) {
        if (cfg_has_pending()) {
            uint32_t elapsed = millis() - s_last_change_ms;
            uint32_t remaining =
                (elapsed >= APPLY_DEBOUNCE_MS) ? 0 : (APPLY_DEBOUNCE_MS - elapsed);
            snprintf(buf, sizeof(buf),
                     "Changes pending — saving in %u s...",
                     (unsigned)((remaining + 999) / 1000));
            lv_label_set_text(s_lbl_pending, buf);
            lv_obj_remove_flag(s_lbl_pending, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_lbl_pending, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void rebuild_settings()
{

    add_section_header("User");
    lv_obj_t *owner_card = add_card();
    {

        lv_obj_t *row_long = lv_obj_create(owner_card);
        lv_obj_remove_style_all(row_long);
        lv_obj_set_width(row_long, lv_pct(100));
        lv_obj_set_height(row_long, 32);
        lv_obj_add_flag(row_long, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(row_long, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(row_long, lv_color_hex(TH_INPUT), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row_long, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_radius(row_long, 0, 0);
        lv_obj_add_event_cb(row_long, owner_long_tap_cb, LV_EVENT_CLICKED, nullptr);

        lv_obj_t *l1 = lv_label_create(row_long);
        lv_label_set_text(l1, "Long name");
        lv_obj_set_style_text_color(l1, lv_color_hex(TH_TEXT2), 0);
        lv_obj_set_style_text_font(l1, &lv_font_montserrat_16, 0);
        lv_obj_align(l1, LV_ALIGN_LEFT_MID, 0, 0);

        s_lbl_owner_long = lv_label_create(row_long);
        lv_label_set_text(s_lbl_owner_long, owner.long_name[0] ? owner.long_name : "(tap to set)");
        lv_obj_set_style_text_color(s_lbl_owner_long, lv_color_hex(TH_TEXT), 0);
        lv_obj_set_style_text_font(s_lbl_owner_long, &lv_font_montserrat_16, 0);
        lv_obj_align(s_lbl_owner_long, LV_ALIGN_RIGHT_MID, -4, 0);
    }
    {
        lv_obj_t *row_short = lv_obj_create(owner_card);
        lv_obj_remove_style_all(row_short);
        lv_obj_set_width(row_short, lv_pct(100));
        lv_obj_set_height(row_short, 32);
        lv_obj_add_flag(row_short, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(row_short, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(row_short, lv_color_hex(TH_INPUT), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row_short, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_radius(row_short, 0, 0);
        lv_obj_add_event_cb(row_short, owner_short_tap_cb, LV_EVENT_CLICKED, nullptr);

        lv_obj_t *l2 = lv_label_create(row_short);
        lv_label_set_text(l2, "Short name");
        lv_obj_set_style_text_color(l2, lv_color_hex(TH_TEXT2), 0);
        lv_obj_set_style_text_font(l2, &lv_font_montserrat_16, 0);
        lv_obj_align(l2, LV_ALIGN_LEFT_MID, 0, 0);

        s_lbl_owner_short = lv_label_create(row_short);
        lv_label_set_text(s_lbl_owner_short, owner.short_name[0] ? owner.short_name : "(tap to set)");
        lv_obj_set_style_text_color(s_lbl_owner_short, lv_color_hex(TH_TEXT), 0);
        lv_obj_set_style_text_font(s_lbl_owner_short, &lv_font_montserrat_16, 0);
        lv_obj_align(s_lbl_owner_short, LV_ALIGN_RIGHT_MID, -4, 0);
    }
    add_card_hint(owner_card, "Tap a row to edit. Saves and broadcasts immediately.");

    s_lbl_pending = lv_label_create(s_list);
    lv_label_set_text(s_lbl_pending, "");
    lv_obj_set_style_text_color(s_lbl_pending, lv_color_hex(0xE0A030), 0);
    lv_obj_set_style_text_font(s_lbl_pending, &lv_font_montserrat_16, 0);
    lv_obj_set_style_pad_top(s_lbl_pending, 6, 0);
    lv_obj_set_style_pad_left(s_lbl_pending, 6, 0);
    lv_obj_add_flag(s_lbl_pending, LV_OBJ_FLAG_HIDDEN);

    add_section_header("LoRa");
    lv_obj_t *lora = add_card();
    add_enum_dropdown_row(lora, "Region",
                          REGIONS, REGION_COUNT, config.lora.region,
                          region_changed_cb);
    add_enum_dropdown_row(lora, "Preset",
                          PRESETS, PRESET_COUNT, config.lora.modem_preset,
                          preset_changed_cb);
    {
        int hop = (int)config.lora.hop_limit;
        if (hop < 1 || hop > 7) hop = 3;
        add_enum_dropdown_row(lora, "Hop limit",
                              HOP_LIMITS, HOP_LIMIT_COUNT, hop,
                              hop_limit_changed_cb);
    }
    add_enum_dropdown_row(lora, "TX power",
                          TX_POWERS, TX_POWER_COUNT,
                          (int)config.lora.tx_power,
                          tx_power_changed_cb);
    add_switch_row(lora, "TX enabled", config.lora.tx_enabled, tx_enabled_changed_cb);
    add_switch_row(lora, "RX boosted gain",
                   config.lora.sx126x_rx_boosted_gain, rx_boost_changed_cb);
    add_switch_row(lora, "Override duty cycle",
                   config.lora.override_duty_cycle, override_duty_changed_cb);
    add_card_hint(lora, "Changing region, preset, or RX boosted gain reboots the device.");

    add_section_header("Device");
    lv_obj_t *device = add_card();
    add_enum_dropdown_row(device, "Role",
                          ROLES, ROLE_COUNT, config.device.role,
                          role_changed_cb);
    add_enum_dropdown_row(device, "Rebroadcast",
                          REBROADCAST_MODES, REBROADCAST_COUNT,
                          config.device.rebroadcast_mode,
                          rebroadcast_changed_cb);
    add_interval_dropdown_row(device, "NodeInfo interval",
                              INTERVAL_SHORT, INTERVAL_SHORT_COUNT,
                              config.device.node_info_broadcast_secs,
                              node_info_interval_changed_cb);

    add_section_header("Display");
    lv_obj_t *display = add_card();
    add_interval_dropdown_row(display, "Screen sleep",
                              SCREEN_TIMEOUT, SCREEN_TIMEOUT_COUNT,
                              config.display.screen_on_secs,
                              screen_timeout_changed_cb);
    add_enum_dropdown_row(display, "Units",
                          UNITS, UNITS_COUNT, config.display.units,
                          units_changed_cb);

    {
        lv_obj_t *row = lv_obj_create(display);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, 52);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, "Timezone");
        lv_obj_set_style_text_color(lbl, lv_color_hex(TH_TEXT2), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *dd = lv_dropdown_create(row);
        const char *labels[TZ_COUNT];
        for (int i = 0; i < TZ_COUNT; i++) labels[i] = TZ_LIST[i].label;

        char opts[2048];
        build_dropdown_options(opts, sizeof(opts), labels, TZ_COUNT);
        lv_dropdown_set_options(dd, opts);
        lv_obj_set_width(dd, 270);
        lv_obj_align(dd, LV_ALIGN_RIGHT_MID, 0, 0);

        lv_dropdown_set_selected(dd, (uint16_t)s_tz_index);
        lv_obj_add_event_cb(dd, timezone_changed_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    }

    s_lbl_orientation = add_info_row(display, "Orientation",
                                     landscape_active() ? "Landscape" : "Portrait");
    s_btn_orientation = lv_button_create(display);
    lv_obj_set_size(s_btn_orientation, lv_pct(100), 46);
    lv_obj_set_style_bg_color(s_btn_orientation, lv_color_hex(TH_ACCENT), 0);
    lv_obj_set_style_radius(s_btn_orientation, 0, 0);
    lv_obj_add_event_cb(s_btn_orientation, orientation_toggle_clicked_cb, LV_EVENT_CLICKED, nullptr);
    s_btn_orientation_label = lv_label_create(s_btn_orientation);
    lv_label_set_text(s_btn_orientation_label,
                      landscape_active() ? "Switch to portrait mode"
                                         : "Switch to landscape mode");
    lv_obj_set_style_text_color(s_btn_orientation_label, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(s_btn_orientation_label, &lv_font_montserrat_16, 0);
    lv_obj_center(s_btn_orientation_label);
    add_card_hint(display, "Orientation changes after save and reboot.");

    add_section_header("Network");
    lv_obj_t *net = add_card();
    s_lbl_wifi_ssid = add_info_row(net, "SSID", "-");
    {

        lv_obj_t *btn = lv_button_create(net);
        lv_obj_set_size(btn, lv_pct(100), 46);
        lv_obj_set_style_bg_color(btn, lv_color_hex(TH_ACCENT), 0);
        lv_obj_set_style_radius(btn, 0, 0);
        lv_obj_add_event_cb(btn, wifi_open_clicked_cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *bl = lv_label_create(btn);
        lv_label_set_text(bl, LV_SYMBOL_WIFI "  WiFi");
        lv_obj_set_style_text_color(bl, lv_color_hex(TH_TEXT), 0);
        lv_obj_set_style_text_font(bl, &lv_font_montserrat_16, 0);
        lv_obj_center(bl);
    }
    {
        lv_obj_t *btn = lv_button_create(net);
        lv_obj_set_size(btn, lv_pct(100), 46);
        lv_obj_set_style_bg_color(btn, lv_color_hex(TH_ACCENT), 0);
        lv_obj_set_style_radius(btn, 0, 0);
        lv_obj_add_event_cb(btn, mqtt_open_clicked_cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *bl = lv_label_create(btn);
        lv_label_set_text(bl, "MQTT");
        lv_obj_set_style_text_color(bl, lv_color_hex(TH_TEXT), 0);
        lv_obj_set_style_text_font(bl, &lv_font_montserrat_16, 0);
        lv_obj_center(bl);
    }
    add_card_hint(net, "Open WiFi to enable, delete credentials, or scan and connect.");

    add_section_header("Position");
    lv_obj_t *pos_card = add_card();
    {

        add_switch_row(pos_card, "Advertise position",
                       position_advert_enabled(),
                       [](lv_event_t *e) {
                           lv_obj_t *sw = (lv_obj_t *)lv_event_get_current_target(e);
                           bool enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
                           if (!position_advert_save(enabled)) {

                               if (enabled) lv_obj_remove_state(sw, LV_STATE_CHECKED);
                               else         lv_obj_add_state(sw, LV_STATE_CHECKED);
                           }
                       });

        auto make_pos_row = [&](const char *label, lv_obj_t **out_value_lbl,
                                lv_event_cb_t tap_cb) {
            lv_obj_t *row = lv_obj_create(pos_card);
            lv_obj_remove_style_all(row);
            lv_obj_set_width(row, lv_pct(100));
            lv_obj_set_height(row, 32);
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_style_bg_color(row, lv_color_hex(TH_INPUT), LV_STATE_PRESSED);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
            lv_obj_set_style_radius(row, 0, 0);
            lv_obj_add_event_cb(row, tap_cb, LV_EVENT_CLICKED, nullptr);

            lv_obj_t *l = lv_label_create(row);
            lv_label_set_text(l, label);
            lv_obj_set_style_text_color(l, lv_color_hex(TH_TEXT2), 0);
            lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
            lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);

            *out_value_lbl = lv_label_create(row);
            lv_label_set_text(*out_value_lbl, "(tap to set)");
            lv_obj_set_style_text_color(*out_value_lbl, lv_color_hex(TH_TEXT), 0);
            lv_obj_set_style_text_font(*out_value_lbl, &lv_font_montserrat_16, 0);
            lv_obj_align(*out_value_lbl, LV_ALIGN_RIGHT_MID, -4, 0);
        };
        make_pos_row("Latitude",  &s_lbl_pos_lat, position_lat_tap_cb);
        make_pos_row("Longitude", &s_lbl_pos_lon, position_lon_tap_cb);
        make_pos_row("Altitude",  &s_lbl_pos_alt, position_alt_tap_cb);

        s_lbl_pos_status = lv_label_create(pos_card);
        lv_label_set_text(s_lbl_pos_status, "");
        lv_obj_set_style_text_color(s_lbl_pos_status, lv_color_hex(TH_TEXT3), 0);
        lv_obj_set_style_text_font(s_lbl_pos_status, &lv_font_montserrat_16, 0);
        lv_label_set_long_mode(s_lbl_pos_status, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(s_lbl_pos_status, lv_pct(100));
        lv_obj_add_flag(s_lbl_pos_status, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t *btn_save = lv_button_create(pos_card);
        lv_obj_set_size(btn_save, lv_pct(100), 46);
        lv_obj_set_style_bg_color(btn_save, lv_color_hex(TH_ACCENT), 0);
        lv_obj_set_style_radius(btn_save, 0, 0);
        lv_obj_add_event_cb(btn_save, position_save_clicked_cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *bs = lv_label_create(btn_save);
        lv_label_set_text(bs, LV_SYMBOL_OK "  Save & broadcast");
        lv_obj_set_style_text_color(bs, lv_color_hex(TH_TEXT), 0);
        lv_obj_set_style_text_font(bs, &lv_font_montserrat_16, 0);
        lv_obj_center(bs);

        lv_obj_t *btn_clear = lv_button_create(pos_card);
        lv_obj_set_size(btn_clear, lv_pct(100), 46);
        lv_obj_set_style_bg_color(btn_clear, lv_color_hex(TH_INPUT), 0);
        lv_obj_set_style_radius(btn_clear, 0, 0);
        lv_obj_add_event_cb(btn_clear, position_clear_clicked_cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *bc = lv_label_create(btn_clear);
        lv_label_set_text(bc, LV_SYMBOL_TRASH "  Clear position");
        lv_obj_set_style_text_color(bc, lv_color_hex(TH_TEXT), 0);
        lv_obj_set_style_text_font(bc, &lv_font_montserrat_16, 0);
        lv_obj_center(bc);
    }
    add_card_hint(pos_card,
                  "Type coordinates from any GPS app (Google Maps: long-press → "
                  "tap the dropped pin to copy lat,lon). Save sets a fixed "
                  "position and broadcasts it immediately. Turn off "
                  "\"Advertise position\" to keep the fix local without "
                  "broadcasting it.");

    add_section_header("Actions");
    lv_obj_t *actions = add_card();

    auto make_action_btn = [&](const char *label, uint32_t bg_color,
                               lv_event_cb_t cb) {
        lv_obj_t *btn = lv_button_create(actions);
        lv_obj_set_size(btn, lv_pct(100), 48);
        lv_obj_set_style_bg_color(btn, lv_color_hex(bg_color), 0);
        lv_obj_set_style_radius(btn, 0, 0);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *bl = lv_label_create(btn);
        lv_label_set_text(bl, label);
        lv_obj_set_style_text_color(bl, lv_color_hex(TH_TEXT), 0);
        lv_obj_set_style_text_font(bl, &lv_font_montserrat_16, 0);
        lv_obj_center(bl);
        return btn;
    };
    make_action_btn("Reboot",       TH_BUBBLE_OUT, reboot_clicked_cb);
    make_action_btn("Shutdown",     TH_INPUT,      shutdown_clicked_cb);
    make_action_btn("Delete all nodes", 0xB87532,  clear_nodes_clicked_cb);
    make_action_btn("Regenerate Private Keys", 0xB87532, regenerate_keys_clicked_cb);
    make_action_btn("Factory reset", 0xB83232,     factory_reset_clicked_cb);

    add_section_header("About");
    lv_obj_t *about = add_card();
    s_lbl_firmware   = add_info_row(about, "Firmware",  optstr(APP_VERSION));
    s_lbl_owner      = add_info_row(about, "User",      "-");
    s_lbl_nodeid     = add_info_row(about, "Node ID",   "-");
    s_lbl_region_now = add_info_row(about, "Region",    "-");
    s_lbl_preset_now = add_info_row(about, "Preset",    "-");
    s_lbl_uptime     = add_info_row(about, "Uptime",    "-");
    s_lbl_heap       = add_info_row(about, "Free heap", "-");
    s_lbl_battery    = add_info_row(about, "Battery",   "-");

    add_switch_row(about, "Show noise floor", show_noise_floor_get(),
                   show_noise_floor_changed_cb);
    s_lbl_noise = add_info_row(about, "Noise floor", "-");
    s_row_noise = lv_obj_get_parent(s_lbl_noise);
    if (s_row_noise && !show_noise_floor_get()) {
        lv_obj_add_flag(s_row_noise, LV_OBJ_FLAG_HIDDEN);
    }
}

lv_obj_t *settings_screen_create(lv_obj_t *parent)
{
    ensure_cfg_thread();

    s_page = lv_obj_create(parent);
    lv_obj_remove_style_all(s_page);
    lv_obj_set_size(s_page, SCR_W, PAGE_H);
    lv_obj_set_pos(s_page, 0, 0);
    lv_obj_set_style_bg_color(s_page, lv_color_hex(TH_BG), 0);
    lv_obj_set_style_bg_opa(s_page, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_page, LV_OBJ_FLAG_SCROLLABLE);

    s_list = lv_obj_create(s_page);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, SCR_W, PAGE_H);
    lv_obj_set_pos(s_list, 0, 0);
    lv_obj_set_style_bg_color(s_list, lv_color_hex(TH_BG), 0);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_list, 8, 0);
    lv_obj_set_style_pad_row(s_list, 6, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_AUTO);

    rebuild_settings();
    refresh_values();
    return s_page;
}

void settings_screen_tick()
{
    if (!s_page) return;
    orientation_overlay_update();
    uint32_t now = millis();
    if (now - s_last_refresh_ms < 1000) return;
    s_last_refresh_ms = now;
    refresh_values();
}

}

#endif
