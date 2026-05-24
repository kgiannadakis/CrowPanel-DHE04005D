#if HAS_TFT && USE_MCUI

#include "McClock.h"
#include "configuration.h"
#include "crowpanel_backlight.h"
#include "ds3231.h"
#include "gps/RTC.h"

#include <Arduino.h>

#include <sys/time.h>
#include <time.h>

namespace mcui {

static bool s_rtc_ok     = false;
static bool s_time_valid = false;

void mcclock_init()
{
    static bool done = false;
    if (done) return;
    done = true;

    if (!ds3231_begin()) {
        LOG_INFO("mcclock: DS3231 not detected on 0x68");
        return;
    }
    s_rtc_ok = true;
    LOG_INFO("mcclock: DS3231 detected on 0x68");

    uint32_t utc = 0;
    if (!ds3231_read_unix(&utc) || utc < 1700000000U) {
        LOG_INFO("mcclock: DS3231 oscillator-halt or pre-2023 timestamp — waiting for NTP / GPS");
        return;
    }

    time_t t = (time_t)utc;
    struct tm utc_tm;
    gmtime_r(&t, &utc_tm);
    RTCSetResult r = perhapsSetRTC(RTCQualityDevice, utc_tm);
    if (r == RTCSetResultSuccess || r == RTCSetResultNotSet) {
        s_time_valid = true;
        LOG_INFO("mcclock: restored UTC %04u-%02u-%02u %02u:%02u:%02u from DS3231",
                 (unsigned)(utc_tm.tm_year + 1900), (unsigned)(utc_tm.tm_mon + 1),
                 (unsigned)utc_tm.tm_mday, (unsigned)utc_tm.tm_hour,
                 (unsigned)utc_tm.tm_min, (unsigned)utc_tm.tm_sec);
    } else {
        LOG_WARN("mcclock: perhapsSetRTC rejected DS3231 time (r=%d)", (int)r);
    }
}

void mcclock_save(uint32_t utc_epoch)
{
    if (!s_rtc_ok) {
        LOG_DEBUG("mcclock: save skipped — no RTC");
        return;
    }
    if (utc_epoch < 1700000000) {

        LOG_WARN("mcclock: refusing to save suspicious epoch %u", (unsigned)utc_epoch);
        return;
    }

    if (!ds3231_write_unix(utc_epoch)) {
        LOG_WARN("mcclock: ds3231_write_unix failed");
        return;
    }
    s_time_valid = true;
}

bool mcclock_has_rtc()        { return s_rtc_ok; }
bool mcclock_has_valid_time() { return s_time_valid; }

}

#endif
