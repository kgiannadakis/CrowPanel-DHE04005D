// ============================================================
// utils.cpp — Utility functions (sanitization, time, base64, etc.)
// ============================================================

#include "utils.h"
#include "app_globals.h"
#include <Wire.h>
#include <SHA256.h>
#include <RTClib.h>

#include "ui.h"
#include "ui_homescreen.h"

// ---- I2C helpers ----
bool i2c_ok(uint8_t a)  { Wire.beginTransmission(a); return !Wire.endTransmission(); }
void i2c_cmd(uint8_t c) { Wire.beginTransmission(0x30); Wire.write(c); Wire.endTransmission(); }

// ---- Screen sleep/wake ----
void screen_sleep() {
  if (!g_screen_awake) return;
  i2c_cmd(BL_OFF);
  g_screen_awake = false;
  g_ramp_current = 0;
}

void screen_wake_soft(uint8_t target) {
  if (g_screen_awake) return;
  uint8_t lvl = target;
  if (lvl < BL_MIN_VISIBLE) lvl = BL_MIN_VISIBLE;
  if (lvl > BL_MAX) lvl = BL_MAX;
  g_ramp_target  = lvl;
  g_ramp_current = BL_MIN_VISIBLE;
  i2c_cmd(g_ramp_current);
  g_ramp_next_ms = millis() + RAMP_STEP_MS;
  g_screen_awake = true;
}

void screen_wake() {
  screen_wake_soft(g_backlight_level);
}

void note_touch_activity() {
  g_last_touch_ms = millis();
  if (!g_screen_awake) screen_wake();
}

void wake_on_event() {
  if (!g_notifications_enabled) return;
  if (!g_screen_awake) screen_wake();
  g_last_touch_ms = millis();
}

// ---- Deferred message push ----
void deferred_msg_push(bool out, const char* txt, const char* sig, bool live_status, uint32_t msg_ts) {
  static const int DEFERRED_MSG_MAX = 32;
  if (g_deferred_msg_count >= DEFERRED_MSG_MAX) { g_deferred_msg_dropped++; return; }
  DeferredChatMsg& m = g_deferred_msgs[g_deferred_msg_count++];
  m.out = out;
  m.live_status = live_status;
  m.msg_ts = msg_ts;
  strncpy(m.txt, txt ? txt : "", sizeof(m.txt) - 1); m.txt[sizeof(m.txt)-1] = '\0';
  strncpy(m.sig, sig ? sig : "", sizeof(m.sig) - 1); m.sig[sizeof(m.sig)-1] = '\0';
}

// ---- RTC ----
void mesh_set_time_epoch(uint32_t epoch) {
  rtc_clock.setCurrentTime(epoch);
}

void update_timelabel() {
  if (!ui_timelabel) return;
  uint32_t utc = rtc_clock.getCurrentTime();
  if (utc < 1000000UL) return;
  uint32_t local = utc + (uint32_t)DISPLAY_UTC_OFFSET_S;
  uint32_t secs_today = local % 86400UL;
  char buf[8];
  snprintf(buf, sizeof(buf), "%02lu:%02lu",
           (unsigned long)(secs_today / 3600), (unsigned long)((secs_today % 3600) / 60));
  lv_label_set_text(ui_timelabel, buf);
}

String time_string_now() {
  uint32_t utc = rtc_clock.getCurrentTime();
  if (utc < 1000000UL) return "--:--";
  uint32_t local = utc + (uint32_t)DISPLAY_UTC_OFFSET_S;
  uint32_t secs_today = local % 86400UL;
  char b[8];
  snprintf(b, sizeof(b), "%02lu:%02lu",
           (unsigned long)(secs_today / 3600), (unsigned long)((secs_today % 3600) / 60));
  return String(b);
}

// ---- UTF-8 validation ----
int utf8_seq_len(uint8_t lead) {
  if (lead < 0x80) return 1;
  if ((lead & 0xE0) == 0xC0) return 2;
  if ((lead & 0xF0) == 0xE0) return 3;
  if ((lead & 0xF8) == 0xF0) return 4;
  return 0;
}

bool utf8_valid_seq(const uint8_t* p, int len) {
  for (int i = 1; i < len; i++) {
    if ((p[i] & 0xC0) != 0x80) return false;
  }
  if (len == 2 && p[0] < 0xC2) return false;
  if (len == 3 && p[0] == 0xE0 && p[1] < 0xA0) return false;
  if (len == 4 && p[0] == 0xF0 && p[1] < 0x90) return false;
  return true;
}

static uint32_t utf8_decode_seq(const uint8_t* p, int len) {
  switch (len) {
    case 1: return p[0];
    case 2: return ((uint32_t)(p[0] & 0x1F) << 6) |
                   ((uint32_t)(p[1] & 0x3F));
    case 3: return ((uint32_t)(p[0] & 0x0F) << 12) |
                   ((uint32_t)(p[1] & 0x3F) << 6) |
                   ((uint32_t)(p[2] & 0x3F));
    case 4: return ((uint32_t)(p[0] & 0x07) << 18) |
                   ((uint32_t)(p[1] & 0x3F) << 12) |
                   ((uint32_t)(p[2] & 0x3F) << 6) |
                   ((uint32_t)(p[3] & 0x3F));
    default: return 0;
  }
}

static bool font_has_glyph(const lv_font_t* font, uint32_t codepoint) {
  if (!font || codepoint == 0) return false;
  lv_font_glyph_dsc_t dsc;
  return lv_font_get_glyph_dsc(font, &dsc, codepoint, 0);
}

// ---- Text sanitization ----
void sanitize_ascii_inplace(char* s) {
  if (!s) return;
  char* w = s;
  const uint8_t* r = (const uint8_t*)s;
  while (*r) {
    uint8_t c = *r;
    if (c == '\n' || c == '\t') { *w++ = (char)c; r++; continue; }
    if (c >= 32 && c < 0x80) { *w++ = (char)c; r++; continue; }
    int slen = utf8_seq_len(c);
    if (slen >= 2 && slen <= 4) {
      bool ok = true;
      for (int i = 0; i < slen; i++) { if (!r[i]) { ok = false; break; } }
      if (ok && utf8_valid_seq(r, slen)) {
        for (int i = 0; i < slen; i++) *w++ = (char)r[i];
        r += slen;
        continue;
      }
    }
    r++;
  }
  *w = 0;
}

String sanitize_ascii_string(const char* s) {
  if (!s) return "";
  String out;
  out.reserve(strlen(s));
  const uint8_t* r = (const uint8_t*)s;
  while (*r) {
    uint8_t c = *r;
    if (c == '\n' || c == '\t') { out += (char)c; r++; continue; }
    if (c >= 32 && c < 0x80) { out += (char)c; r++; continue; }
    int slen = utf8_seq_len(c);
    if (slen >= 2 && slen <= 4) {
      bool ok = true;
      for (int i = 0; i < slen; i++) { if (!r[i]) { ok = false; break; } }
      if (ok && utf8_valid_seq(r, slen)) {
        for (int i = 0; i < slen; i++) out += (char)r[i];
        r += slen;
        continue;
      }
    }
    r++;
  }
  return out;
}

String sanitize_for_font_string(const char* s, const lv_font_t* font) {
  if (!s) return "";
  if (!font) return sanitize_ascii_string(s);

  String out;
  out.reserve(strlen(s));
  const uint8_t* r = (const uint8_t*)s;
  while (*r) {
    uint8_t c = *r;
    if (c == '\n' || c == '\t') {
      out += (char)c;
      r++;
      continue;
    }
    if (c >= 32 && c < 0x80) {
      out += (char)c;
      r++;
      continue;
    }

    int slen = utf8_seq_len(c);
    if (slen < 2 || slen > 4) {
      r++;
      continue;
    }

    bool ok = true;
    for (int i = 0; i < slen; i++) {
      if (!r[i]) {
        ok = false;
        break;
      }
    }
    if (!ok || !utf8_valid_seq(r, slen)) {
      r++;
      continue;
    }

    uint32_t codepoint = utf8_decode_seq(r, slen);
    if (font_has_glyph(font, codepoint)) {
      for (int i = 0; i < slen; i++) out += (char)r[i];
    }
    r += slen;
  }
  return out;
}

// ---- SNR ----
int snr_to_bars(int8_t snr) {
  if (snr == -128) return 0;
  if (snr >= 8)    return 4;
  if (snr >= 3)    return 3;
  if (snr >= -2)   return 2;
  if (snr >= -8)   return 1;
  return 0;
}

void snr_contact_update(const uint8_t* pub_key, int8_t snr) {
  for (int i = 0; i < MAX_UNREAD_SLOTS; i++) {
    if (g_contact_snr[i].valid && memcmp(g_contact_snr[i].pub_key, pub_key, 32) == 0) {
      g_contact_snr[i].last_snr = snr;
      return;
    }
  }
  for (int i = 0; i < MAX_UNREAD_SLOTS; i++) {
    if (!g_contact_snr[i].valid) {
      memcpy(g_contact_snr[i].pub_key, pub_key, 32);
      g_contact_snr[i].last_snr = snr;
      g_contact_snr[i].valid = true;
      return;
    }
  }
}

int8_t snr_contact_get(const uint8_t* pub_key) {
  for (int i = 0; i < MAX_UNREAD_SLOTS; i++)
    if (g_contact_snr[i].valid && memcmp(g_contact_snr[i].pub_key, pub_key, 32) == 0)
      return g_contact_snr[i].last_snr;
  return -128;
}

// ---- Notification helpers ----
bool has_any_unread() {
  for (int i = 0; i < MAX_UNREAD_SLOTS; i++)
    if (g_contact_unread[i].valid && g_contact_unread[i].count > 0) return true;
  for (int i = 0; i < MAX_UNREAD_SLOTS; i++)
    if (g_channel_unread[i].valid && g_channel_unread[i].count > 0) return true;
  return false;
}

int notify_contact_find(const uint8_t* pub_key) {
  for (int i = 0; i < MAX_UNREAD_SLOTS; i++)
    if (g_contact_unread[i].valid && memcmp(g_contact_unread[i].pub_key, pub_key, 32) == 0)
      return i;
  return -1;
}
int notify_channel_find(int channel_idx) {
  for (int i = 0; i < MAX_UNREAD_SLOTS; i++)
    if (g_channel_unread[i].valid && g_channel_unread[i].channel_idx == channel_idx)
      return i;
  return -1;
}

void notify_contact_inc(const uint8_t* pub_key) {
  int i = notify_contact_find(pub_key);
  if (i < 0) {
    for (i = 0; i < MAX_UNREAD_SLOTS; i++) if (!g_contact_unread[i].valid) break;
    if (i == MAX_UNREAD_SLOTS) return;
    memcpy(g_contact_unread[i].pub_key, pub_key, 32);
    g_contact_unread[i].count = 0;
    g_contact_unread[i].valid = true;
  }
  if (g_contact_unread[i].count < 9999) g_contact_unread[i].count++;
  g_notifications_dirty = true;
}
void notify_contact_clear(const uint8_t* pub_key) {
  int i = notify_contact_find(pub_key);
  if (i < 0) return;
  g_contact_unread[i] = {};
  g_notifications_dirty = true;
}
uint16_t notify_contact_get(const uint8_t* pub_key) {
  int i = notify_contact_find(pub_key);
  return i >= 0 ? g_contact_unread[i].count : 0;
}

void notify_channel_inc(int channel_idx) {
  int i = notify_channel_find(channel_idx);
  if (i < 0) {
    for (i = 0; i < MAX_UNREAD_SLOTS; i++) if (!g_channel_unread[i].valid) break;
    if (i == MAX_UNREAD_SLOTS) return;
    g_channel_unread[i].channel_idx = channel_idx;
    g_channel_unread[i].count = 0;
    g_channel_unread[i].valid = true;
  }
  if (g_channel_unread[i].count < 9999) g_channel_unread[i].count++;
  g_notifications_dirty = true;
}
void notify_channel_clear(int channel_idx) {
  int i = notify_channel_find(channel_idx);
  if (i < 0) return;
  g_channel_unread[i] = {};
  g_notifications_dirty = true;
}
uint16_t notify_channel_get(int channel_idx) {
  int i = notify_channel_find(channel_idx);
  return i >= 0 ? g_channel_unread[i].count : 0;
}

// ---- Base64 ----
static const char* B64_ALPH = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

String b64_encode_bytes(const uint8_t* data, size_t len) {
  String out;
  out.reserve(((len + 2) / 3) * 4);
  for (size_t i = 0; i < len; i += 3) {
    uint32_t v = (uint32_t)data[i] << 16;
    if (i + 1 < len) v |= (uint32_t)data[i + 1] << 8;
    if (i + 2 < len) v |= (uint32_t)data[i + 2];
    out += B64_ALPH[(v >> 18) & 0x3F];
    out += B64_ALPH[(v >> 12) & 0x3F];
    out += (i + 1 < len) ? B64_ALPH[(v >> 6) & 0x3F] : '=';
    out += (i + 2 < len) ? B64_ALPH[(v >> 0) & 0x3F] : '=';
  }
  return out;
}

String packet_signal_str(const mesh::Packet* pkt) {
  if (!pkt) return "";
  char buf[32];
  int hops = (int)pkt->getPathHashCount();
  snprintf(buf, sizeof(buf), "%d hop%s, SNR:%d",
           hops, hops == 1 ? "" : "s", (int)pkt->_snr);
  return String(buf);
}

// ---- Hashtag helpers ----
String normalize_hashtag(const char* in) {
  if (!in) return "";
  String s(in);
  s.trim();
  if (!s.length()) return "";
  s.replace(" ", "");
  if (s[0] != '#') s = String("#") + s;
  s.toLowerCase();
  if (s.length() > 31) s.remove(31);
  return s;
}

void hashtag_to_secret16(const String& tag, uint8_t out16[16]) {
  uint8_t full[32];
  SHA256 h;
  h.reset();
  h.update((const uint8_t*)tag.c_str(), tag.length());
  h.finalize(full, 32);
  memcpy(out16, full, 16);
}

String secret16_to_base64(const uint8_t sec16[16]) {
  return b64_encode_bytes(sec16, 16);
}

// ---- Misc ----
uint32_t parse_u32(const char* t, uint32_t fallback) {
  if (!t) return fallback;
  while (*t == ' ') t++;
  if (!*t) return fallback;
  char* endp = nullptr;
  unsigned long v = strtoul(t, &endp, 10);
  if (endp == t) return fallback;
  return (uint32_t)v;
}

String fmt_duration(uint32_t secs) {
  uint32_t d = secs / 86400; secs %= 86400;
  uint32_t h = secs / 3600;  secs %= 3600;
  uint32_t m = secs / 60;    uint32_t s = secs % 60;
  char buf[64];
  if (d > 0) snprintf(buf, sizeof(buf), "%lud %02luh %02lum %02lus", (unsigned long)d, (unsigned long)h, (unsigned long)m, (unsigned long)s);
  else        snprintf(buf, sizeof(buf), "%02luh %02lum %02lus", (unsigned long)h, (unsigned long)m, (unsigned long)s);
  return String(buf);
}

// ---- Serial monitor ----
// Internal: append raw text to serial buffer (no # escaping)
static void serialmon_append_raw(const char* line, size_t llen) {
  if (!ui_serialmonitorwindow || !line) return;

  if (!serialLabel) {
    serialLabel = lv_label_create(ui_serialmonitorwindow);
    lv_obj_set_width(serialLabel, lv_pct(100));
    lv_label_set_long_mode(serialLabel, LV_LABEL_LONG_WRAP);
    lv_label_set_text(serialLabel, "");
    lv_label_set_recolor(serialLabel, true);
    lv_obj_set_style_text_font(serialLabel, &lv_font_montserrat_16, 0);
    g_serial_buf[0] = '\0';
    g_serial_len = 0;
  }

  if (g_serial_len + llen + 64 > SERIAL_BUF_SIZE) {
    size_t keep = SERIAL_BUF_TRIM;
    if (keep < g_serial_len) {
      memmove(g_serial_buf, g_serial_buf + (g_serial_len - keep), keep);
      g_serial_len = keep;
      g_serial_buf[g_serial_len] = '\0';
    } else {
      g_serial_len = 0;
      g_serial_buf[0] = '\0';
    }
  }

  const char* sep = "------------------------------\n";
  size_t seplen = strlen(sep);
  if (g_serial_len + seplen + 1 < SERIAL_BUF_SIZE) {
    memcpy(g_serial_buf + g_serial_len, sep, seplen);
    g_serial_len += seplen;
    g_serial_buf[g_serial_len] = '\0';
  }

  size_t copy = llen;
  if (g_serial_len + copy + 2 > SERIAL_BUF_SIZE)
    copy = SERIAL_BUF_SIZE - g_serial_len - 2;
  memcpy(g_serial_buf + g_serial_len, line, copy);
  g_serial_len += copy;
  g_serial_buf[g_serial_len++] = '\n';
  g_serial_buf[g_serial_len]   = '\0';

  g_deferred_serialmon_dirty = true;
}

void serialmon_append(const char* line) {
  if (!line) return;
  // Escape any # as ## so LVGL recolor doesn't misinterpret plain text
  size_t len = strlen(line);
  // Fast path: no # in the text
  bool has_hash = false;
  for (size_t i = 0; i < len; i++) {
    if (line[i] == '#') { has_hash = true; break; }
  }
  if (!has_hash) {
    serialmon_append_raw(line, len);
    return;
  }
  // Slow path: escape # as ##
  char escaped[640];
  size_t j = 0;
  for (size_t i = 0; i < len && j < sizeof(escaped) - 2; i++) {
    if (line[i] == '#') {
      escaped[j++] = '#';
      escaped[j++] = '#';
    } else {
      escaped[j++] = line[i];
    }
  }
  escaped[j] = '\0';
  serialmon_append_raw(escaped, j);
}

void serialmon_append_color(uint32_t rgb, const char* line) {
  if (!line) return;
  // LVGL recolor state machine: #RRGGBB<transition_char>text#
  // Inside a color block, a # ENDS the block. So to show a literal #
  // we must: close color (#), emit escaped ## (literal #), reopen color (#RRGGBB ).
  // Sequence at each #: ####RRGGBB  (close + escape## + reopen + hex + transition)
  char hex[8];
  snprintf(hex, sizeof(hex), "%06lX", (unsigned long)(rgb & 0xFFFFFF));

  char tagged[512];
  size_t j = 0;
  // Open color tag: #RRGGBB<space>  (space is consumed transition char)
  tagged[j++] = '#';
  for (int k = 0; k < 6; k++) tagged[j++] = hex[k];
  tagged[j++] = ' ';  // transition char (consumed by parser)

  for (size_t i = 0; line[i] && j < sizeof(tagged) - 16; i++) {
    if (line[i] == '#') {
      tagged[j++] = '#';  // close current color (IN→WAIT, consumed)
      tagged[j++] = '#';  // start escape (WAIT→PAR, consumed)
      tagged[j++] = '#';  // complete escape (PAR→WAIT, rendered as literal #)
      tagged[j++] = '#';  // start new color (WAIT→PAR, consumed)
      for (int k = 0; k < 6; k++) tagged[j++] = hex[k]; // hex (consumed)
      tagged[j++] = ' ';  // transition char (consumed)
    } else {
      tagged[j++] = line[i];
    }
  }
  tagged[j++] = '#';  // close final color block
  tagged[j] = '\0';
  serialmon_append_raw(tagged, j);
}

// ---- Speaker / Buzzer ----
void speaker_init() {
  ledcSetup(SPK_LEDC_CHANNEL, 2000, 8);
  ledcAttachPin(SPK_PIN, SPK_LEDC_CHANNEL);
  ledcWrite(SPK_LEDC_CHANNEL, 0);
}

void beep_short(uint16_t freq_hz, uint16_t duration_ms) {
  if (!g_speaker_enabled) return;
  ledcWriteTone(SPK_LEDC_CHANNEL, freq_hz);
  delay(duration_ms);
  ledcWriteTone(SPK_LEDC_CHANNEL, 0);
}

void beep_msg_in()   { beep_short(1800, 60); delay(40); beep_short(2400, 80); }
void beep_msg_out()  { beep_short(1200, 50); }
void beep_error()    { beep_short(400, 200); }
