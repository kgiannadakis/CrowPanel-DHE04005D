// ============================================================
// utils.cpp — Utility functions (sanitization, time, base64, etc.)
// ============================================================

#include "utils.h"
#include "app_globals.h"
#include "emoji_atlas.h"   // emoji_atlas_has() for sanitize_for_font_string
#include <Wire.h>
#include <SHA256.h>

#include "ui.h"
#include "ui_homescreen.h"
#include "stc8.h"

// ---- I2C helpers ----
// P4 port: v11 used a dumb I2C backlight chip at 0x30 addressed directly via
// Arduino Wire. On DHE04005D the backlight is controlled via the STC8
// coprocessor at 0x2F through a register scheme (see variants/…/stc8.cpp),
// and Arduino Wire is DISABLED (PIN_BOARD_SDA/SCL=-1) because the STC8
// driver owns I2C_NUM_0. Route i2c_cmd() through stc8_set_pwm_duty() —
// this also kills the periodic "could not acquire lock" / "NULL TX buffer"
// spam from the ramp ticker.
bool i2c_ok(uint8_t) { return true; }  // Wire is disabled; probe is a no-op
void i2c_cmd(uint8_t level) {
  // v11 levels are 0..BL_MAX(=0x10); STC8 expects duty in percent (0..100).
  // BL_OFF (=0x05) is v11's "sleep command" for its I2C chip, NOT a dim
  // level — treat anything ≤ BL_OFF as hardware-off so screen_sleep()
  // actually turns the backlight off on our variant.
  uint8_t duty;
  if (level <= BL_OFF) {
    duty = 0;
  } else {
    if (level > BL_MAX) level = BL_MAX;
    duty = (uint32_t)level * 100u / BL_MAX;
  }
  stc8_set_pwm_duty(STC8_PWM_LCD_BL_EN, duty);
}

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

static bool append_latin_translit(String& out, uint32_t cp) {
  switch (cp) {
    case 0x00E4: out += "ae"; return true; // ä
    case 0x00F6: out += "oe"; return true; // ö
    case 0x00FC: out += "ue"; return true; // ü
    case 0x00C4: out += "Ae"; return true; // Ä
    case 0x00D6: out += "Oe"; return true; // Ö
    case 0x00DC: out += "Ue"; return true; // Ü
    case 0x00DF: out += "ss"; return true; // ß
    default: return false;
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

// Codepoint in a "likely emoji" range — used by sanitize_for_font_string
// to decide whether to consult the emoji atlas for glyph availability.
// We only check the atlas for codepoints in these ranges; everything
// else (Latin, Greek, CJK, punctuation, arrows, etc.) gets through
// unconditionally and LVGL's normal font fallback handles it.
static inline bool codepoint_is_emoji_range(uint32_t cp) {
  if (cp == 0x200D)                  return true;   // ZWJ
  if (cp >= 0x2300 && cp <= 0x27BF)  return true;   // misc tech / geometric / misc symbols / dingbats
  if (cp >= 0x2B00 && cp <= 0x2BFF)  return true;   // misc symbols and arrows
  if (cp >= 0x1F000 && cp <= 0x1FFFF) return true;  // all supplemental planes (emoticons, transport, etc.)
  return false;
}

String sanitize_for_font_string(const char* s, const lv_font_t* font) {
  (void)font;

  // Step 1: strip invalid UTF-8 and control bytes. Identical to what
  // file-persistence paths do — sanitize_ascii_string is reused
  // verbatim so the display path and the on-disk representation share
  // their "valid-byte" definition.
  String cleaned = sanitize_ascii_string(s);

  // Step 2: strip display-problem codepoints that would render as
  // empty placeholder boxes.
  //   (a) Variation selectors (U+FE00-U+FE0F). These are invisible
  //       hints ("render the preceding char as emoji" for VS-16 /
  //       U+FE0F), never glyphs. Phones append U+FE0F to emojis like
  //       ❤ U+2764 for proper emoji presentation; on receive we have
  //       ❤ in the atlas but no glyph for FE0F, so FE0F used to show
  //       as a box next to every received heart.
  //   (b) Emoji codepoints in known-emoji ranges that we don't have
  //       in our atlas. Without this, a rare emoji from a newer phone
  //       renders as an empty box. With it, the emoji simply
  //       disappears and the surrounding text stays intact.
  //
  // Codepoints outside the emoji ranges pass through regardless — we
  // trust Montserrat / Greek / LVGL-symbol fonts to handle them.
  String out;
  out.reserve(cleaned.length());

  const uint8_t* r = (const uint8_t*)cleaned.c_str();
  size_t remaining = cleaned.length();
  while (remaining > 0) {
    uint8_t c = *r;

    // ASCII — keep, fast path
    if (c < 0x80) {
      out += (char)c;
      r++;
      remaining--;
      continue;
    }

    int slen = utf8_seq_len(c);
    if (slen < 2 || slen > 4 || (size_t)slen > remaining || !utf8_valid_seq(r, slen)) {
      // Shouldn't happen after sanitize_ascii_string, but be defensive
      r++;
      remaining--;
      continue;
    }

    // Decode the codepoint so we can test ranges / query the atlas
    uint32_t cp = 0;
    switch (slen) {
      case 2: cp = ((uint32_t)(r[0] & 0x1F) << 6) |
                   ((uint32_t)(r[1] & 0x3F)); break;
      case 3: cp = ((uint32_t)(r[0] & 0x0F) << 12) |
                   ((uint32_t)(r[1] & 0x3F) << 6) |
                   ((uint32_t)(r[2] & 0x3F)); break;
      case 4: cp = ((uint32_t)(r[0] & 0x07) << 18) |
                   ((uint32_t)(r[1] & 0x3F) << 12) |
                   ((uint32_t)(r[2] & 0x3F) << 6) |
                   ((uint32_t)(r[3] & 0x3F)); break;
    }

    // (a) Variation selectors — always strip
    if (cp >= 0xFE00 && cp <= 0xFE0F) {
      r += slen;
      remaining -= slen;
      continue;
    }

    // (b) Emoji range + not in atlas — strip
    if (codepoint_is_emoji_range(cp) && !emoji_atlas_has(cp)) {
      r += slen;
      remaining -= slen;
      continue;
    }

    // If caller provided a font and this non-ASCII codepoint is missing,
    // transliterate common latin-extended chars to ASCII equivalents;
    // otherwise drop to avoid repeated LVGL "glyph dsc not found" work.
    if (font && cp >= 0x80 && !font_has_glyph(font, cp)) {
      if (append_latin_translit(out, cp)) {
        r += slen;
        remaining -= slen;
        continue;
      }
      r += slen;
      remaining -= slen;
      continue;
    }

    // Keep
    for (int i = 0; i < slen; i++) out += (char)r[i];
    r += slen;
    remaining -= slen;
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
// Flip to 1 (or -DSERIALMON_MIRROR_UART=1 at build time) to mirror
// every serialmon_append() line to the UART during development.
// Default OFF: on-screen serial monitor shows the same text and the
// UART write consumes CPU + blocking tx time for every log event.
#ifndef SERIALMON_MIRROR_UART
#define SERIALMON_MIRROR_UART 0
#endif

static void serialmon_append_raw(const char* line, size_t llen) {
  if (!line) return;

#if SERIALMON_MIRROR_UART
  // Mirror to Arduino Serial for PC-side debugging. See note above.
  Serial.println(line);
#endif

  // Buffer into g_serial_buf unconditionally — the ui_serialmonitorwindow
  // widget is created later in setup() by ui_init(), but log lines from
  // sd_init / emoji_atlas_init etc. happen BEFORE that. The deferred
  // drain in main.cpp creates `serialLabel` the first time it sees dirty
  // data AND the window exists, then flushes the accumulated buffer.

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

  // Strip non-ASCII so the on-screen monitor (which uses plain
  // lv_font_plain_14 without an emoji fallback) doesn't log a
  // "glyph not found" warning per character per frame. Mesh advert
  // names with flag emojis (🇳🇱 = U+1F1F3 + U+1F1F1) were the
  // common trigger. Replacement '?' keeps the string length stable
  // and visually marks the dropped character.
  size_t budget = SERIAL_BUF_SIZE - g_serial_len - 2;
  size_t copy = (llen < budget) ? llen : budget;
  const uint8_t* src = (const uint8_t*)line;
  for (size_t i = 0; i < copy; i++) {
    uint8_t c = src[i];
    g_serial_buf[g_serial_len + i] =
        (c == '\n' || c == '\t' || (c >= 32 && c < 0x80)) ? (char)c : '?';
  }
  g_serial_len += copy;
  g_serial_buf[g_serial_len++] = '\n';
  g_serial_buf[g_serial_len]   = '\0';

  g_deferred_serialmon_dirty = true;
}

void serialmon_append(const char* line) {
  if (!line) return;
  // Recolor is OFF on serialLabel (see main.cpp lazy-create block),
  // so '#' needs no escaping and the label renders text verbatim.
  serialmon_append_raw(line, strlen(line));
}

void serialmon_append_color(uint32_t /*rgb*/, const char* line) {
  if (!line) return;
  // Recolor is disabled on the serial-monitor label (see main.cpp
  // lazy-create block — we turned it off because the per-frame scan of
  // every '#' in a 2 KB buffer was the dominant cost of scrolling the
  // monitor). With recolor off, the old `#RRGGBB …#` escape dance would
  // render literally as text (e.g., a repeater name "KaA_HV43#1_S.Deurne"
  // came out as "KaA_HV43####00FFC8 1_S.Deurne"). Just emit the plain
  // line — we lose per-line colour but keep correct text.
  serialmon_append_raw(line, strlen(line));
}
// Dead path below (kept commented out so the full recolor logic is
// discoverable if we ever re-enable recolor on the monitor label):
#if 0
static void serialmon_append_color_legacy(uint32_t rgb, const char* line) {
  if (!line) return;
  char hex[8];
  snprintf(hex, sizeof(hex), "%06lX", (unsigned long)(rgb & 0xFFFFFF));

  char tagged[512];
  size_t j = 0;
  tagged[j++] = '#';
  for (int k = 0; k < 6; k++) tagged[j++] = hex[k];
  tagged[j++] = ' ';

  for (size_t i = 0; line[i] && j < sizeof(tagged) - 16; i++) {
    if (line[i] == '#') {
      tagged[j++] = '#';
      tagged[j++] = '#';
      tagged[j++] = '#';
      tagged[j++] = '#';
      for (int k = 0; k < 6; k++) tagged[j++] = hex[k];
      tagged[j++] = ' ';
    } else {
      tagged[j++] = line[i];
    }
  }
  tagged[j++] = '#';
  tagged[j] = '\0';
  serialmon_append_raw(tagged, j);
}
#endif  // end of legacy recolor-escape implementation

// ---- Speaker / Buzzer (I2S) ----
// DHE04005D has an I2S audio header at LRCK=IO21, BCLK=IO22, SDOUT=IO23.
// v11's ledc-driven piezo is gone; we emit a short sine burst on I2S
// instead. Mono, 16-bit signed, 32 kHz — identical frame is fed to both
// channels since the I2S slot mode is stereo by default.
//
// IMPORTANT: the whole beep pipeline runs in a DEDICATED FreeRTOS task
// at priority 1 (below WiFi/LwIP at 18+ and LVGL at 2). Calling the
// blocking i2s_channel_write() on the Arduino loop() task was starving
// ESP-Hosted's SDIO RX processing for 100-180 ms per beep, which blew
// the SDIO RX buffer pool and panicked in sdio_rx_get_buffer. By
// putting the actual I2S work on its own task, beep_msg_in() just
// posts a command to a queue and returns in microseconds — the main
// loop keeps pulling WiFi packets while the beep plays.
#include <math.h>
#include <driver/i2s_std.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include "board_config.h"

static i2s_chan_handle_t s_spk_tx    = nullptr;
static bool              s_spk_ok    = false;
static QueueHandle_t     s_beep_q    = nullptr;
static TaskHandle_t      s_beep_task = nullptr;
static constexpr uint32_t SPK_SAMPLE_RATE_HZ = 32000;
static constexpr size_t   SPK_CHUNK_FRAMES   = 256;

// Shared I2S scratch buffer. All sine/sweep/noise/FM/square helpers
// run sequentially on the single beep task so they can reuse one
// 1-KB .bss region instead of each owning a private static buffer.
// Stereo interleaved int16 — [L R L R ...].
static int16_t           s_spk_buf[SPK_CHUNK_FRAMES * 2];

enum BeepCmd : uint8_t { BEEP_MSG_IN = 1, BEEP_MSG_OUT = 2, BEEP_ERROR = 3 };

// Amp control — matches Elecrow's set_Audio_ctrl(): SD pin is ACTIVE
// LOW (level=0 = amp ON, level=1 = amp shutdown). Keep amp off at
// idle so it doesn't idle-buzz on an empty I2S stream.
static inline void amp_enable(bool on) {
  stc8_gpio_set_level(STC8_GPIO_OUT_AUDIO_SD, on ? 0 : 1);
}

// Compute the current sample amplitude given the user's volume
// setting (0..100). Maximum base amplitude is capped at ~50% of int16
// so clipping never occurs even with modest gain downstream.
static inline int16_t volume_scaled_amplitude() {
  uint32_t vol = g_speaker_volume;
  if (vol > 100) vol = 100;
  return (int16_t)((int32_t)16000 * (int32_t)vol / 100);
}

// Streams a sine burst of `freq_hz` for `duration_ms`. If `envelope`
// is true, applies an exponential decay (full→10 % over the note
// duration) — gives the bell/ding feel. If false, constant amplitude
// like the old beep.
static void play_tone_blocking(uint16_t freq_hz, uint16_t duration_ms,
                               bool envelope) {
  if (!s_spk_ok || freq_hz == 0 || duration_ms == 0) return;

  const uint32_t total_samples = (uint32_t)SPK_SAMPLE_RATE_HZ * duration_ms / 1000u;
  if (total_samples == 0) return;
  const int16_t  base_amplitude = volume_scaled_amplitude();
  if (base_amplitude <= 0) return;  // muted — skip entirely


  // Exponential decay: amp = base * exp(-decay_rate * t).
  // For "decay to 10% by end of note" -> decay_rate = ln(10)/duration.
  // Pre-compute a per-sample multiplier so the inner loop stays in
  // integer arithmetic as much as possible.
  const float decay_per_sample = envelope
      ? expf(-2.302585f / (float)total_samples)  // ln(10) ≈ 2.3026
      : 1.0f;
  float amp = (float)base_amplitude;

  const float phase_step = 2.0f * (float)M_PI * (float)freq_hz / (float)SPK_SAMPLE_RATE_HZ;
  float phase = 0.0f;
  uint32_t emitted = 0;
  while (emitted < total_samples) {
    size_t frames = total_samples - emitted;
    if (frames > SPK_CHUNK_FRAMES) frames = SPK_CHUNK_FRAMES;

    for (size_t i = 0; i < frames; i++) {
      int16_t s = (int16_t)(amp * sinf(phase));
      s_spk_buf[i * 2 + 0] = s;
      s_spk_buf[i * 2 + 1] = s;
      phase += phase_step;
      if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
      amp *= decay_per_sample;
    }

    size_t written = 0;
    i2s_channel_write(s_spk_tx, s_spk_buf, frames * 2 * sizeof(int16_t),
                      &written, pdMS_TO_TICKS(200));
    emitted += frames;
  }
}

// Wrap each "phrase" (possibly multiple tones) in amp-on/amp-off so
// the speaker is only powered for the duration of the actual sound.
static void play_phrase_begin() {
  amp_enable(true);
  i2s_channel_enable(s_spk_tx);
}
static void play_phrase_end() {
  // Flush a chunk of silence so the DAC returns to midscale before we
  // disable the channel — prevents the "click" that class-D amps make
  // when I2S clocks stop mid-sample. Reuses the shared scratch buffer.
  memset(s_spk_buf, 0, sizeof(s_spk_buf));
  size_t written = 0;
  i2s_channel_write(s_spk_tx, s_spk_buf, sizeof(s_spk_buf), &written, pdMS_TO_TICKS(50));
  i2s_channel_disable(s_spk_tx);
  amp_enable(false);
}

// Musical frequencies (equal temperament, A4 = 440 Hz).
namespace {
  constexpr uint16_t NOTE_G5  = 784;
  constexpr uint16_t NOTE_A5  = 880;
  constexpr uint16_t NOTE_B5  = 988;
  constexpr uint16_t NOTE_C6  = 1047;
  constexpr uint16_t NOTE_D6  = 1175;
  constexpr uint16_t NOTE_E6  = 1319;
  constexpr uint16_t NOTE_G6  = 1568;
  constexpr uint16_t NOTE_C7  = 2093;
}

// Helper: sum-of-two-sines tone. Used for chord + marimba (sine +
// octave harmonic). Both tones share the same volume-scaled amplitude
// but each sine is halved so their sum doesn't clip.
static void play_two_tone(uint16_t f1, uint16_t f2, uint16_t duration_ms,
                          bool envelope) {
  if (!s_spk_ok || duration_ms == 0) return;
  const uint32_t total_samples = (uint32_t)SPK_SAMPLE_RATE_HZ * duration_ms / 1000u;
  if (total_samples == 0) return;
  int16_t amp_base = volume_scaled_amplitude();
  if (amp_base <= 0) return;


  const float decay_per_sample = envelope
      ? expf(-2.302585f / (float)total_samples) : 1.0f;
  float amp = (float)amp_base;

  const float step1 = 2.0f * (float)M_PI * (float)f1 / (float)SPK_SAMPLE_RATE_HZ;
  const float step2 = 2.0f * (float)M_PI * (float)f2 / (float)SPK_SAMPLE_RATE_HZ;
  float ph1 = 0.0f, ph2 = 0.0f;

  uint32_t emitted = 0;
  while (emitted < total_samples) {
    size_t frames = total_samples - emitted;
    if (frames > SPK_CHUNK_FRAMES) frames = SPK_CHUNK_FRAMES;
    for (size_t i = 0; i < frames; i++) {
      int16_t s = (int16_t)(amp * 0.5f * (sinf(ph1) + sinf(ph2)));
      s_spk_buf[i * 2 + 0] = s; s_spk_buf[i * 2 + 1] = s;
      ph1 += step1; ph2 += step2;
      amp *= decay_per_sample;
    }
    size_t written = 0;
    i2s_channel_write(s_spk_tx, s_spk_buf, frames * 2 * sizeof(int16_t),
                      &written, pdMS_TO_TICKS(200));
    emitted += frames;
  }
}

// Helper: linear frequency sweep from f_start to f_end over duration.
// Used for the swoosh / glissando sound.
static void play_sweep(uint16_t f_start, uint16_t f_end, uint16_t duration_ms) {
  if (!s_spk_ok || duration_ms == 0) return;
  const uint32_t total_samples = (uint32_t)SPK_SAMPLE_RATE_HZ * duration_ms / 1000u;
  if (total_samples == 0) return;
  int16_t amp_base = volume_scaled_amplitude();
  if (amp_base <= 0) return;

  // `step` is the per-sample phase increment — a linear ramp from
  // step_start to step_end over the full duration. Precompute once
  // and advance with addition instead of the previous per-sample
  // divide (`2π·freq/Fs`), which cost ~7k FP divides on a 220 ms
  // sweep at 32 kHz.
  const float two_pi_over_fs = 2.0f * (float)M_PI / (float)SPK_SAMPLE_RATE_HZ;
  const float step_start = two_pi_over_fs * (float)f_start;
  const float step_end   = two_pi_over_fs * (float)f_end;
  const float step_delta = (step_end - step_start) / (float)total_samples;
  float step  = step_start;
  float phase = 0.0f;
  uint32_t emitted = 0;
  while (emitted < total_samples) {
    size_t frames = total_samples - emitted;
    if (frames > SPK_CHUNK_FRAMES) frames = SPK_CHUNK_FRAMES;
    for (size_t i = 0; i < frames; i++) {
      int16_t s = (int16_t)((float)amp_base * sinf(phase));
      s_spk_buf[i * 2 + 0] = s; s_spk_buf[i * 2 + 1] = s;
      phase += step;
      step  += step_delta;
    }
    size_t written = 0;
    i2s_channel_write(s_spk_tx, s_spk_buf, frames * 2 * sizeof(int16_t),
                      &written, pdMS_TO_TICKS(200));
    emitted += frames;
  }
}

// Helper: FM synth. `carrier` is the audible pitch; `mod` modulates
// the carrier frequency at ±`depth` Hz. Produces a metallic/digital
// timbre.
static void play_fm(uint16_t carrier, uint16_t mod, uint16_t depth,
                    uint16_t duration_ms, bool envelope) {
  if (!s_spk_ok || duration_ms == 0) return;
  const uint32_t total_samples = (uint32_t)SPK_SAMPLE_RATE_HZ * duration_ms / 1000u;
  if (total_samples == 0) return;
  int16_t amp_base = volume_scaled_amplitude();
  if (amp_base <= 0) return;

  const float decay_per_sample = envelope
      ? expf(-2.302585f / (float)total_samples) : 1.0f;
  float amp = (float)amp_base;
  const float ts = 1.0f / (float)SPK_SAMPLE_RATE_HZ;
  const float two_pi_mod = 2.0f * (float)M_PI * (float)mod;
  float phase = 0.0f;
  uint32_t emitted = 0;
  while (emitted < total_samples) {
    size_t frames = total_samples - emitted;
    if (frames > SPK_CHUNK_FRAMES) frames = SPK_CHUNK_FRAMES;
    for (size_t i = 0; i < frames; i++) {
      float t = (float)(emitted + i) * ts;
      float inst_freq = (float)carrier + (float)depth * sinf(two_pi_mod * t);
      float step = 2.0f * (float)M_PI * inst_freq * ts;
      int16_t s = (int16_t)(amp * sinf(phase));
      s_spk_buf[i * 2 + 0] = s; s_spk_buf[i * 2 + 1] = s;
      phase += step;
      amp *= decay_per_sample;
    }
    size_t written = 0;
    i2s_channel_write(s_spk_tx, s_spk_buf, frames * 2 * sizeof(int16_t),
                      &written, pdMS_TO_TICKS(200));
    emitted += frames;
  }
}

// Helper: square-wave burst at `freq_hz`.
static void play_square(uint16_t freq_hz, uint16_t duration_ms) {
  if (!s_spk_ok || freq_hz == 0 || duration_ms == 0) return;
  const uint32_t total_samples = (uint32_t)SPK_SAMPLE_RATE_HZ * duration_ms / 1000u;
  if (total_samples == 0) return;
  int16_t amp = volume_scaled_amplitude();
  if (amp <= 0) return;

  const uint32_t half_period = SPK_SAMPLE_RATE_HZ / (freq_hz * 2u);
  uint32_t counter = 0;
  int16_t level = amp;
  uint32_t emitted = 0;
  while (emitted < total_samples) {
    size_t frames = total_samples - emitted;
    if (frames > SPK_CHUNK_FRAMES) frames = SPK_CHUNK_FRAMES;
    for (size_t i = 0; i < frames; i++) {
      s_spk_buf[i * 2 + 0] = level; s_spk_buf[i * 2 + 1] = level;
      if (++counter >= half_period) { counter = 0; level = -level; }
    }
    size_t written = 0;
    i2s_channel_write(s_spk_tx, s_spk_buf, frames * 2 * sizeof(int16_t),
                      &written, pdMS_TO_TICKS(200));
    emitted += frames;
  }
}

// Helper: short band-limited noise burst with decay.
static void play_noise(uint16_t duration_ms) {
  if (!s_spk_ok || duration_ms == 0) return;
  const uint32_t total_samples = (uint32_t)SPK_SAMPLE_RATE_HZ * duration_ms / 1000u;
  if (total_samples == 0) return;
  int16_t amp_base = volume_scaled_amplitude();
  if (amp_base <= 0) return;

  const float decay_per_sample = expf(-2.302585f / (float)total_samples);
  float amp = (float)amp_base;
  // Simple LCG for deterministic, cheap noise.
  static uint32_t rng = 0xA5A5A5A5u;
  uint32_t emitted = 0;
  while (emitted < total_samples) {
    size_t frames = total_samples - emitted;
    if (frames > SPK_CHUNK_FRAMES) frames = SPK_CHUNK_FRAMES;
    for (size_t i = 0; i < frames; i++) {
      rng = rng * 1664525u + 1013904223u;
      int16_t n = (int16_t)((int32_t)(rng >> 16) % 32767);
      int16_t s = (int16_t)(amp * ((float)n / 32767.0f));
      s_spk_buf[i * 2 + 0] = s; s_spk_buf[i * 2 + 1] = s;
      amp *= decay_per_sample;
    }
    size_t written = 0;
    i2s_channel_write(s_spk_tx, s_spk_buf, frames * 2 * sizeof(int16_t),
                      &written, pdMS_TO_TICKS(200));
    emitted += frames;
  }
}

// ── The sound bank ──────────────────────────────────────────
// Keep these in sync with SOUND_NAMES below and notification_sound_count().
enum NotifSound : uint8_t {
  SND_RISING_TRIAD = 0,
  SND_FALLING_TRIAD,
  SND_BELL_DING,
  SND_DOUBLE_DING,
  SND_SWOOSH,
  SND_FM_DIGITAL,
  SND_MAJOR_CHORD,
  SND_MARIMBA,
  SND_RETRO_ALERT,
  SND_WHISPER,
  // New (16-entry bank). Indices 0-9 above stay stable so an existing
  // g_notification_sound_idx in NVS still selects the same sound after
  // an upgrade.
  SND_HAPPY_ARP,     // bright C-major arpeggio, "cheerful"
  SND_TWINKLE,       // 5 sparkly high ticks, "attention"
  SND_MUSIC_BOX,     // slow FM, sweet nostalgic timbre
  SND_TADA,          // two-note fanfare + held major chord
  SND_MYSTERY,       // descending FM sweep, "hmm?"
  SND_CURIOUS,       // playful repeating pattern, "look!"
  SND_COUNT
};

static const char* const SOUND_NAMES[SND_COUNT] = {
  "Rising Chime",
  "Falling Chime",
  "Bell Ding",
  "Double Ding",
  "Swoosh",
  "Digital FM",
  "Major Chord",
  "Marimba",
  "Retro Alert",
  "Whisper Burst",
  "Happy Arpeggio",
  "Twinkle",
  "Music Box",
  "Ta-Da!",
  "Mystery",
  "Curious",
};

int notification_sound_count() { return SND_COUNT; }
const char* notification_sound_name(int idx) {
  if (idx < 0 || idx >= SND_COUNT) return "";
  return SOUND_NAMES[idx];
}
const char* notification_sound_names_newline_list() {
  // LVGL dropdown wants a single newline-separated string. Build once
  // and cache.
  static char cached[256];
  if (cached[0] != '\0') return cached;
  size_t w = 0;
  for (int i = 0; i < SND_COUNT; i++) {
    if (i > 0 && w + 1 < sizeof(cached)) cached[w++] = '\n';
    const char* n = SOUND_NAMES[i];
    size_t nl = strlen(n);
    if (w + nl + 1 >= sizeof(cached)) break;
    memcpy(cached + w, n, nl); w += nl;
  }
  cached[w] = '\0';
  return cached;
}

// Plays the given sound. Runs on the beep task (see beep_task below).
static void notification_sound_play_internal(uint8_t idx) {
  play_phrase_begin();
  switch (idx) {
    case SND_RISING_TRIAD:
      play_tone_blocking(NOTE_G5, 110, true);
      vTaskDelay(pdMS_TO_TICKS(15));
      play_tone_blocking(NOTE_B5, 110, true);
      vTaskDelay(pdMS_TO_TICKS(15));
      play_tone_blocking(NOTE_D6, 180, true);
      break;
    case SND_FALLING_TRIAD:
      play_tone_blocking(NOTE_D6, 110, true);
      vTaskDelay(pdMS_TO_TICKS(15));
      play_tone_blocking(NOTE_B5, 110, true);
      vTaskDelay(pdMS_TO_TICKS(15));
      play_tone_blocking(NOTE_G5, 220, true);
      break;
    case SND_BELL_DING:
      play_tone_blocking(NOTE_E6, 450, true);
      break;
    case SND_DOUBLE_DING:
      play_tone_blocking(NOTE_E6, 120, true);
      vTaskDelay(pdMS_TO_TICKS(80));
      play_tone_blocking(NOTE_E6, 180, true);
      break;
    case SND_SWOOSH:
      play_sweep(600, 1800, 220);
      break;
    case SND_FM_DIGITAL:
      // Carrier ~1250 Hz, modulator 55 Hz at ±180 Hz depth: "Nokia-ish"
      // metallic tone. 300 ms with decay.
      play_fm(1250, 55, 180, 300, true);
      break;
    case SND_MAJOR_CHORD:
      // G5 + B5 held together for 300 ms with decay — clean major third.
      play_two_tone(NOTE_G5, NOTE_B5, 300, true);
      break;
    case SND_MARIMBA:
      // Fundamental + one octave harmonic, short decay — percussive.
      play_two_tone(NOTE_C6, NOTE_C7, 260, true);
      break;
    case SND_RETRO_ALERT:
      play_square(1000, 90);
      vTaskDelay(pdMS_TO_TICKS(60));
      play_square(1000, 90);
      break;
    case SND_WHISPER:
      play_noise(220);
      break;
    case SND_HAPPY_ARP:
      // Fast ascending C-major arpeggio with octave — unambiguously
      // cheerful, ~450 ms total.
      play_tone_blocking(NOTE_C6, 70, true);
      vTaskDelay(pdMS_TO_TICKS(10));
      play_tone_blocking(NOTE_E6, 70, true);
      vTaskDelay(pdMS_TO_TICKS(10));
      play_tone_blocking(NOTE_G6, 70, true);
      vTaskDelay(pdMS_TO_TICKS(10));
      play_tone_blocking(NOTE_C7, 160, true);
      break;
    case SND_TWINKLE:
      // 5 short high ticks alternating C7 and G6 — sparkly, grabs
      // attention without being harsh.
      play_tone_blocking(NOTE_C7, 50, true);
      vTaskDelay(pdMS_TO_TICKS(30));
      play_tone_blocking(NOTE_G6, 50, true);
      vTaskDelay(pdMS_TO_TICKS(30));
      play_tone_blocking(NOTE_C7, 50, true);
      vTaskDelay(pdMS_TO_TICKS(30));
      play_tone_blocking(NOTE_E6, 50, true);
      vTaskDelay(pdMS_TO_TICKS(30));
      play_tone_blocking(NOTE_C7, 100, true);
      break;
    case SND_MUSIC_BOX:
      // FM with a slow, sweet modulator: carrier near C6, mod 7 Hz at
      // ±45 Hz depth. Produces a tremolo-ish timbre reminiscent of a
      // child's music box. 450 ms with decay.
      play_fm(NOTE_C6, 7, 45, 450, true);
      break;
    case SND_TADA:
      // Classic fanfare — G6 rising to C7 (perfect fourth), small gap,
      // then a held C-major two-tone (E6 + G6) for the "reveal".
      play_tone_blocking(NOTE_G6, 100, true);
      vTaskDelay(pdMS_TO_TICKS(20));
      play_tone_blocking(NOTE_C7, 120, true);
      vTaskDelay(pdMS_TO_TICKS(60));
      play_two_tone(NOTE_E6, NOTE_G6, 260, true);
      break;
    case SND_MYSTERY:
      // FM sweep 1600→700 Hz with a modulator makes a "what?"-flavored
      // descending pitch with a slight warble. ~350 ms.
      play_sweep(1600, 700, 220);
      vTaskDelay(pdMS_TO_TICKS(20));
      play_fm(700, 6, 120, 180, true);
      break;
    case SND_CURIOUS:
      // Playful "pick-me-up" pattern: two-note call, answer, resolve.
      // E6, G6 (short) — C7 (short) — G6 (medium).
      play_tone_blocking(NOTE_E6, 70, true);
      vTaskDelay(pdMS_TO_TICKS(10));
      play_tone_blocking(NOTE_G6, 70, true);
      vTaskDelay(pdMS_TO_TICKS(50));
      play_tone_blocking(NOTE_C7, 70, true);
      vTaskDelay(pdMS_TO_TICKS(20));
      play_tone_blocking(NOTE_G6, 160, true);
      break;
    default:
      // Unknown idx → fall back to the default.
      play_tone_blocking(NOTE_E6, 150, true);
      break;
  }
  play_phrase_end();
}

// Queue word encoding: low 4 bits = opcode, high 4 bits = arg.
// Keeps queue element size at 1 byte. Arg is a sound index (0..15)
// for BEEP_TEST_SOUND; unused for the other opcodes.
// NOTE: with 4 bits for arg, the sound bank is capped at 16 entries.
// Adding a 17th sound requires bumping the queue element size to
// uint16_t and updating pack_cmd / cmd_arg. Caught at compile time:
static_assert(SND_COUNT <= 16, "SND_COUNT exceeds 4-bit beep-queue arg width; bump packing");
enum BeepCmdExt : uint8_t { BEEP_PREVIEW = 4, BEEP_TEST_SOUND = 5 };
static inline uint8_t pack_cmd(uint8_t opcode, uint8_t arg) {
  return (uint8_t)((arg & 0x0F) << 4 | (opcode & 0x0F));
}
static inline uint8_t cmd_op(uint8_t c)  { return c & 0x0F; }
static inline uint8_t cmd_arg(uint8_t c) { return (c >> 4) & 0x0F; }

static void beep_task(void*) {
  uint8_t cmd;
  for (;;) {
    if (xQueueReceive(s_beep_q, &cmd, portMAX_DELAY) != pdTRUE) continue;
    if (!g_speaker_enabled) continue;
    const uint8_t op  = cmd_op(cmd);
    const uint8_t arg = cmd_arg(cmd);
    switch (op) {
      case BEEP_MSG_IN:
        notification_sound_play_internal(g_notification_sound_idx);
        break;
      case BEEP_MSG_OUT:
        play_phrase_begin();
        play_tone_blocking(NOTE_C6, 70, true);
        play_phrase_end();
        break;
      case BEEP_ERROR:
        play_phrase_begin();
        play_tone_blocking(400, 180, false);
        play_phrase_end();
        break;
      case BEEP_PREVIEW:
        play_phrase_begin();
        play_tone_blocking(NOTE_E6, 120, true);
        play_phrase_end();
        break;
      case BEEP_TEST_SOUND:
        notification_sound_play_internal(arg);
        break;
    }
  }
}

void speaker_init() {
  if (s_spk_ok) return;

  // Elecrow Lesson11/Lesson12 reference for this exact board uses
  // I2S_NUM_1 — NOT I2S_NUM_0. I2S_NUM_0 appeared to init cleanly but
  // the GDMA / pin-matrix routing didn't reach the external amp, which
  // is why speaker_init logged "i2s=ok stc8_err=0" but nothing came
  // out of the speaker.
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num  = 6;
  chan_cfg.dma_frame_num = 256;
  chan_cfg.auto_clear    = true;
  esp_err_t err = i2s_new_channel(&chan_cfg, &s_spk_tx, nullptr);
  if (err != ESP_OK) {
    serialmon_append("speaker_init: i2s_new_channel failed");
    return;
  }

  i2s_std_config_t std_cfg = {};
  std_cfg.clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SPK_SAMPLE_RATE_HZ);
  std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                       I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  std_cfg.gpio_cfg.bclk = (gpio_num_t)SPK_I2S_BCLK_GPIO;
  std_cfg.gpio_cfg.ws   = (gpio_num_t)SPK_I2S_LRCK_GPIO;
  std_cfg.gpio_cfg.dout = (gpio_num_t)SPK_I2S_SDOUT_GPIO;
  std_cfg.gpio_cfg.din  = I2S_GPIO_UNUSED;

  err = i2s_channel_init_std_mode(s_spk_tx, &std_cfg);
  if (err != ESP_OK) {
    serialmon_append("speaker_init: i2s_channel_init_std_mode failed");
    i2s_del_channel(s_spk_tx);
    s_spk_tx = nullptr;
    return;
  }

  s_spk_ok = true;
  Serial.println("speaker_init: i2s_std channel ready (I2S_NUM_1)");

  // Command queue + worker task. Priority 1 keeps it below WiFi/LwIP
  // (18+) and LVGL (2) so the beep's blocking I2S writes never stall
  // those. 4-KB stack covers sinf() + i2s_channel_write's internal
  // buffers; the sine-generation scratch is in .bss not on this stack.
  s_beep_q = xQueueCreate(4, sizeof(uint8_t));
  if (!s_beep_q) return;
  xTaskCreate(beep_task, "beep", 4096, nullptr, 1, &s_beep_task);
}

// Public API — these enqueue a packed command and return in
// microseconds. Safe to call from any task; the actual I2S transfer
// happens on the beep task.
static inline void post_beep(uint8_t cmd) {
  if (!s_beep_q) return;
  // Non-blocking: if the queue is full we drop this one rather than
  // stall the caller.
  xQueueSend(s_beep_q, &cmd, 0);
}

void beep_msg_in()         { post_beep(pack_cmd(BEEP_MSG_IN,  0)); }
void beep_msg_out()        { post_beep(pack_cmd(BEEP_MSG_OUT, 0)); }
void beep_error()          { post_beep(pack_cmd(BEEP_ERROR,   0)); }
void speaker_play_preview(){ post_beep(pack_cmd(BEEP_PREVIEW, 0)); }

// Preview a specific sound index without committing it as the
// notification sound. Used by the Settings dropdown Test button.
void notification_sound_test(int idx) {
  if (idx < 0 || idx >= notification_sound_count()) return;
  // We only have 4 bits for the arg, which is enough for up to 16
  // sounds. If the bank grows past that we'd need to widen the queue
  // entry.
  post_beep(pack_cmd(BEEP_TEST_SOUND, (uint8_t)idx));
}
