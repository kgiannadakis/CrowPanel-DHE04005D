// ============================================================
// persistence.cpp — SPIFFS chat files, NVS prefs, contact records
// ============================================================

#include "persistence.h"
#include "utils.h"
#include <vector>
#if defined(ESP32)
#include <esp_task_wdt.h>
#endif
#include "chat_ui.h"
#include "settings_cb.h"
#include "translate.h"

#include "ui_homescreen.h"

// ---- "NEW" divider tracking ----
ChatReadPos g_read_pos[MAX_READ_POS];

int read_pos_find(const char* key) {
  for (int i = 0; i < MAX_READ_POS; i++)
    if (g_read_pos[i].valid && strcmp(g_read_pos[i].key, key) == 0) return i;
  return -1;
}
int read_pos_get(const char* key) {
  int i = read_pos_find(key);
  return (i >= 0) ? g_read_pos[i].msg_count : -1;
}
void read_pos_set(const char* key, int count) {
  int i = read_pos_find(key);
  if (i < 0) {
    for (i = 0; i < MAX_READ_POS; i++) if (!g_read_pos[i].valid) break;
    if (i == MAX_READ_POS) return;
    strncpy(g_read_pos[i].key, key, sizeof(g_read_pos[i].key) - 1);
    g_read_pos[i].key[sizeof(g_read_pos[i].key) - 1] = '\0';
    g_read_pos[i].valid = true;
  }
  g_read_pos[i].msg_count = count;
}

// ---- Chat file persistence ----
const char* CHANNELS_FILE = "/channels_v2";

String key_for_contact(const mesh::Identity& id) {
  char hex[13];
  snprintf(hex, sizeof(hex), "%02X%02X%02X%02X%02X%02X",
           id.pub_key[0], id.pub_key[1], id.pub_key[2],
           id.pub_key[3], id.pub_key[4], id.pub_key[5]);
  return String("ct_") + hex;
}
String key_for_channel(int idx) { return String("ch_") + String(idx); }
String chat_path_for(const String& key) { return String("/chat_") + key; }

void append_chat_to_file(const String& key, bool out, const char* msg, uint32_t msg_ts, const char* signal_info) {
#if defined(ESP32)
  if ((SPIFFS.totalBytes() - SPIFFS.usedBytes()) < 4096) {
    serialmon_append("SPIFFS full - chat write skipped");
    return;
  }
  String path = chat_path_for(key);
  File f = SPIFFS.open(path, FILE_APPEND);
  if (!f) f = SPIFFS.open(path, FILE_WRITE);
  if (!f) return;
  uint32_t ts = msg_ts ? msg_ts : rtc_clock.getCurrentTime();
  if (out) {
    f.print("TX|");
    f.print(ts);
    f.print("|P|");
  } else {
    String safe_signal = signal_info ? sanitize_ascii_string(signal_info) : "";
    safe_signal.replace('|', ',');
    f.print("RX|");
    f.print(ts);
    f.print("|");
    if (safe_signal.length()) f.print(safe_signal);
    f.print("|");
  }
  f.println(msg);
  f.close();
#else
  (void)key; (void)out; (void)msg; (void)msg_ts; (void)signal_info;
#endif
}

void append_translation_to_last_rx(const String& key, const char* translation) {
#if defined(ESP32)
  if (!translation || !translation[0]) return;
  String path = chat_path_for(key);
  if (!SPIFFS.exists(path)) return;

  // Read all lines, append translation to the last RX line
  File f = SPIFFS.open(path, FILE_READ);
  if (!f) return;

  std::vector<String> lines;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length()) lines.push_back(line);
  }
  f.close();

  // Find last RX line and append translation
  for (int i = (int)lines.size() - 1; i >= 0; i--) {
    if (lines[i].startsWith("RX|") && lines[i].indexOf("{{TR}}") < 0) {
      lines[i] += "{{TR}}";
      lines[i] += translation;
      break;
    }
  }

  // Rewrite file
  esp_task_wdt_reset();
  f = SPIFFS.open(path, FILE_WRITE);
  if (!f) return;
  for (size_t i = 0; i < lines.size(); i++) {
    f.println(lines[i]);
  }
  f.close();
#else
  (void)key; (void)translation;
#endif
}

void update_last_tx_status_in_file(const String& key, char status, int16_t repeat_count) {
#if defined(ESP32)
  // Build status string: 'R' with count → "R3", others → single char
  String status_str;
  if (status == 'R' && repeat_count >= 0) {
    status_str = String("R") + String((int)repeat_count);
  } else {
    status_str = String((char)status);
  }

  String path = chat_path_for(key);
  if (!SPIFFS.exists(path)) return;

  File f = SPIFFS.open(path, FILE_READ);
  if (!f) return;
  int last_tx_offset = -1;
  int offset = 0;
  while (f.available()) {
    int line_start = offset;
    String line = f.readStringUntil('\n');
    offset = (int)f.position();
    line.trim();
    if (line.startsWith("TX|")) last_tx_offset = line_start;
  }
  f.close();
  if (last_tx_offset < 0) return;

  File src = SPIFFS.open(path, FILE_READ);
  if (!src) return;
  String tmp = path + ".tmp";
  File dst = SPIFFS.open(tmp, FILE_WRITE);
  if (!dst) { src.close(); return; }

  offset = 0;
  while (src.available()) {
    int line_start = offset;
    String line = src.readStringUntil('\n');
    offset = (int)src.position();
    line.trim();
    if (!line.length()) continue;

    if (line_start == last_tx_offset && line.startsWith("TX|")) {
      int p1 = line.indexOf('|');
      int p2 = (p1 >= 0) ? line.indexOf('|', p1 + 1) : -1;
      int p3 = (p2 >= 0) ? line.indexOf('|', p2 + 1) : -1;
      if (p2 >= 0 && p3 >= 0 && p3 > p2) {
        line = line.substring(0, p2 + 1) + status_str + line.substring(p3);
      }
    }
    dst.println(line);
  }
  src.close(); dst.close();
  SPIFFS.remove(path);
  SPIFFS.rename(tmp, path);
#else
  (void)key; (void)status; (void)repeat_count;
#endif
}

void update_tx_status_by_msg_ts(const String& key, uint32_t msg_ts, char status) {
#if defined(ESP32)
  if (!msg_ts) return;
  String ts_str = String(msg_ts);
  String path = chat_path_for(key);
  if (!SPIFFS.exists(path)) return;

  File f = SPIFFS.open(path, FILE_READ);
  if (!f) return;
  int target_offset = -1;
  int offset = 0;
  while (f.available()) {
    int line_start = offset;
    String line = f.readStringUntil('\n');
    offset = (int)f.position();
    line.trim();
    if (line.startsWith("TX|")) {
      // Extract timestamp: TX|<ts>|status|msg
      int p1 = line.indexOf('|');
      int p2 = (p1 >= 0) ? line.indexOf('|', p1 + 1) : -1;
      if (p2 > p1) {
        String file_ts = line.substring(p1 + 1, p2);
        if (file_ts == ts_str) { target_offset = line_start; break; }
      }
    }
  }
  f.close();
  if (target_offset < 0) return;

  File src = SPIFFS.open(path, FILE_READ);
  if (!src) return;
  String tmp = path + ".tmp";
  File dst = SPIFFS.open(tmp, FILE_WRITE);
  if (!dst) { src.close(); return; }

  offset = 0;
  while (src.available()) {
    int line_start = offset;
    String line = src.readStringUntil('\n');
    offset = (int)src.position();
    line.trim();
    if (!line.length()) continue;

    if (line_start == target_offset && line.startsWith("TX|")) {
      int p1 = line.indexOf('|');
      int p2 = (p1 >= 0) ? line.indexOf('|', p1 + 1) : -1;
      int p3 = (p2 >= 0) ? line.indexOf('|', p2 + 1) : -1;
      if (p2 >= 0 && p3 >= 0 && p3 > p2) {
        line = line.substring(0, p2 + 1) + String(status) + line.substring(p3);
      }
    }
    dst.println(line);
  }
  src.close(); dst.close();
  SPIFFS.remove(path);
  SPIFFS.rename(tmp, path);
#else
  (void)key; (void)msg_ts; (void)status;
#endif
}

static const int MAX_DISPLAY_MESSAGES = 30;

void load_chat_from_file(const String& key) {
  chat_clear();
#if defined(ESP32)
  String path = chat_path_for(key);
  if (!SPIFFS.exists(path)) return;
  File f = SPIFFS.open(path, FILE_READ);
  if (!f) return;

  int total = 0;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.startsWith("TX|") || line.startsWith("RX|")) total++;
  }
  f.seek(0);

  int last_read = read_pos_get(key.c_str());
  int skip = (total > MAX_DISPLAY_MESSAGES) ? total - MAX_DISPLAY_MESSAGES : 0;
  int divider_at = (last_read >= 0 && last_read < total) ? (last_read - skip) : -1;

  int idx = 0;
  int display_idx = 0;
  bool divider_inserted = false;
  g_loading_history = true;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (!line.length()) continue;
    bool out = line.startsWith("TX|");
    if (!out && !line.startsWith("RX|")) continue;

    if (idx++ < skip) continue;

    if (!divider_inserted && divider_at >= 0 && display_idx >= divider_at) {
      divider_inserted = true;
      if (ui_chatpanel) {
        lv_obj_t* div = lv_obj_create(ui_chatpanel);
        lv_obj_set_width(div, lv_pct(100));
        lv_obj_set_height(div, 28);
        lv_obj_set_style_bg_opa(div, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(div, 0, 0);
        lv_obj_set_style_pad_all(div, 0, 0);
        lv_obj_clear_flag(div, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t* lineL = lv_obj_create(div);
        lv_obj_set_size(lineL, lv_pct(30), 1);
        lv_obj_set_style_bg_color(lineL, lv_color_hex(g_theme->new_divider), 0);
        lv_obj_set_style_bg_opa(lineL, LV_OPA_60, 0);
        lv_obj_set_style_border_opa(lineL, LV_OPA_TRANSP, 0);
        lv_obj_align(lineL, LV_ALIGN_LEFT_MID, 4, 0);
        lv_obj_clear_flag(lineL, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t* dlbl = lv_label_create(div);
        lv_label_set_text(dlbl, "NEW MESSAGES");
        lv_obj_set_style_text_color(dlbl, lv_color_hex(g_theme->new_divider), 0);
        lv_obj_set_style_text_font(dlbl, &lv_font_montserrat_10, 0);
        lv_obj_center(dlbl);
        lv_obj_t* lineR = lv_obj_create(div);
        lv_obj_set_size(lineR, lv_pct(30), 1);
        lv_obj_set_style_bg_color(lineR, lv_color_hex(g_theme->new_divider), 0);
        lv_obj_set_style_bg_opa(lineR, LV_OPA_60, 0);
        lv_obj_set_style_border_opa(lineR, LV_OPA_TRANSP, 0);
        lv_obj_align(lineR, LV_ALIGN_RIGHT_MID, -4, 0);
        lv_obj_clear_flag(lineR, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
      }
    }
    display_idx++;

    int p1 = line.indexOf('|');
    int p2 = line.indexOf('|', p1 + 1);
    if (p1 < 0 || p2 < 0) continue;

    int p3 = line.indexOf('|', p2 + 1);
    char status = 0;
    uint16_t repeat_count = 0;
    String sig_info = "";
    String body;
    if (p3 >= 0) {
      String middle = line.substring(p2 + 1, p3);
      body = line.substring(p3 + 1);

      if (out) {
        if (middle.length()) {
          status = middle[0];
          // Parse repeat count from "R3", "R12", etc.
          if (status == 'R' && middle.length() > 1) {
            repeat_count = (uint16_t)atoi(middle.c_str() + 1);
          }
        }
      } else {
        // RX lines store signal info in the middle field.
        sig_info = middle;

        // Recover lines written with a literal '|' inside the signal text, e.g.
        // RX|ts|2 hops | SNR:5|message
        int extra_pipe = body.indexOf('|');
        if (sig_info.indexOf("hop") >= 0 && extra_pipe > 0) {
          String sig_tail = body.substring(0, extra_pipe);
          if (sig_tail.indexOf("SNR:") >= 0) {
            sig_info.trim();
            sig_tail.trim();
            sig_info += ", ";
            sig_info += sig_tail;
            body = body.substring(extra_pipe + 1);
          }
        }
      }
    } else {
      body = line.substring(p2 + 1);
    }

    // Extract inline translation if present
    String trans = "";
    int tr_pos = body.indexOf("{{TR}}");
    if (tr_pos >= 0) {
      trans = body.substring(tr_pos + 6);
      body = body.substring(0, tr_pos);
    }

    chat_add(out, body.c_str(), false, status, sig_info.length() ? sig_info.c_str() : nullptr, repeat_count, nullptr, trans.length() ? trans.c_str() : nullptr);
  }
  g_loading_history = false;
  f.close();

  read_pos_set(key.c_str(), total);
  chat_scroll_to_newest();
#else
  (void)key;
#endif
}

void delete_chat_file_for_key(const String& key) {
#if defined(ESP32)
  String path = chat_path_for(key);
  if (SPIFFS.exists(path)) SPIFFS.remove(path);
#else
  (void)key;
#endif
}

// ---- NVS persistence ----
#if defined(ESP32)
static Preferences g_prefs;

uint32_t clamp_timeout_s(uint32_t s) {
  if (s < 5) s = 5;
  if (s > 3600) s = 3600;
  return s;
}
void save_ui_prefs_nvs() {
  g_prefs.begin("ui", false);
  g_prefs.putUInt("timeout_s",    clamp_timeout_s(g_screen_timeout_s));
  g_prefs.putUChar("auto_contact",  g_auto_contact_enabled  ? 1 : 0);
  g_prefs.putUChar("auto_repeater", g_auto_repeater_enabled ? 1 : 0);
  g_prefs.putUChar("pkt_fwd",      g_packet_forward_enabled ? 1 : 0);
  g_prefs.putUChar("pos_adv",      g_position_advert_enabled ? 1 : 0);
  g_prefs.putUChar("auto_trans",   g_auto_translate_enabled ? 1 : 0);
  g_prefs.putUChar("trans_lang",   (uint8_t)g_translate_lang_idx);
  g_prefs.putUInt("muted_ch",     g_muted_channel_mask);
  g_prefs.putUChar("tz_idx",      (uint8_t)tz_get_index());
  g_prefs.putUChar("speaker_en",   g_speaker_enabled ? 1 : 0);
  g_prefs.putUChar("landscape",    g_landscape_mode ? 1 : 0);
  g_prefs.end();
}
void save_landscape_nvs(bool landscape) {
  g_prefs.begin("ui", false);
  g_prefs.putUChar("landscape", landscape ? 1 : 0);
  g_prefs.end();
}
bool load_landscape_nvs() {
  g_prefs.begin("ui", true);
  bool v = g_prefs.getUChar("landscape", 0) != 0;
  g_prefs.end();
  return v;
}
void load_ui_prefs_nvs() {
  g_prefs.begin("ui", true);
  g_screen_timeout_s      = clamp_timeout_s(g_prefs.getUInt("timeout_s", 30));
  g_auto_contact_enabled  = g_prefs.getUChar("auto_contact",  1) != 0;
  g_auto_repeater_enabled  = g_prefs.getUChar("auto_repeater", 1) != 0;
  g_packet_forward_enabled = g_prefs.getUChar("pkt_fwd",      1) != 0;
  g_position_advert_enabled = g_prefs.getUChar("pos_adv",     1) != 0;
  g_auto_translate_enabled = g_prefs.getUChar("auto_trans",   0) != 0;
  g_translate_lang_idx     = (int)g_prefs.getUChar("trans_lang", 0);
  if (g_translate_lang_idx >= translate_lang_count()) g_translate_lang_idx = 0;
  g_muted_channel_mask    = g_prefs.getUInt("muted_ch",       0);
  int tz_idx              = (int)g_prefs.getUChar("tz_idx", 10); // default Amsterdam
  g_speaker_enabled       = g_prefs.getUChar("speaker_en", 1) != 0;
  g_prefs.end();
  tz_set_index(tz_idx);
  tz_update_offset_now();
}
void save_timeout_s(uint32_t s) {
  g_screen_timeout_s = clamp_timeout_s(s);
  save_ui_prefs_nvs();
}
void save_device_name_nvs(const char* name) {
  if (!name) return;
  g_prefs.begin("ui", false);
  g_prefs.putString("devname", name);
  g_prefs.end();
}
bool load_device_name_nvs(char* out, size_t outlen) {
  if (!out || outlen == 0) return false;
  g_prefs.begin("ui", true);
  String s = g_prefs.getString("devname", "");
  g_prefs.end();
  if (!s.length()) return false;
  StrHelper::strncpy(out, s.c_str(), outlen);
  return true;
}
void save_preset_idx_nvs(int idx) {
  g_prefs.begin("ui", false);
  g_prefs.putInt("preset_idx", idx);
  g_prefs.end();
}
int load_preset_idx_nvs() {
  g_prefs.begin("ui", true);
  int idx = g_prefs.getInt("preset_idx", -1);
  g_prefs.end();
  return idx;
}

// ---- Unread counts (NVS) ----
#pragma pack(push,1)
struct ContactUnreadRecord { uint8_t pub_key[32]; uint16_t count; };
struct ChannelUnreadRecord  { int32_t channel_idx; uint16_t count; };
#pragma pack(pop)

void save_notifications_nvs() {
  ContactUnreadRecord crecs[MAX_UNREAD_SLOTS];
  int nc = 0;
  for (int i = 0; i < MAX_UNREAD_SLOTS; i++)
    if (g_contact_unread[i].valid && g_contact_unread[i].count > 0) {
      memcpy(crecs[nc].pub_key, g_contact_unread[i].pub_key, 32);
      crecs[nc].count = g_contact_unread[i].count;
      nc++;
    }
  ChannelUnreadRecord hrecs[MAX_UNREAD_SLOTS];
  int nh = 0;
  for (int i = 0; i < MAX_UNREAD_SLOTS; i++)
    if (g_channel_unread[i].valid && g_channel_unread[i].count > 0) {
      hrecs[nh].channel_idx = (int32_t)g_channel_unread[i].channel_idx;
      hrecs[nh].count = g_channel_unread[i].count;
      nh++;
    }
  g_prefs.begin("notif", false);
  if (nc > 0) g_prefs.putBytes("contacts", crecs, (size_t)nc * sizeof(ContactUnreadRecord));
  else        g_prefs.remove("contacts");
  if (nh > 0) g_prefs.putBytes("channels", hrecs, (size_t)nh * sizeof(ChannelUnreadRecord));
  else        g_prefs.remove("channels");
  g_prefs.end();
}

void load_notifications_nvs() {
  g_prefs.begin("notif", true);
  size_t csz = g_prefs.getBytesLength("contacts");
  if (csz > 0 && csz % sizeof(ContactUnreadRecord) == 0) {
    ContactUnreadRecord crecs[MAX_UNREAD_SLOTS];
    int n = (int)(csz / sizeof(ContactUnreadRecord));
    if (n > MAX_UNREAD_SLOTS) n = MAX_UNREAD_SLOTS;
    g_prefs.getBytes("contacts", crecs, (size_t)n * sizeof(ContactUnreadRecord));
    for (int i = 0; i < n; i++)
      for (int j = 0; j < MAX_UNREAD_SLOTS; j++)
        if (!g_contact_unread[j].valid) {
          memcpy(g_contact_unread[j].pub_key, crecs[i].pub_key, 32);
          g_contact_unread[j].count = crecs[i].count;
          g_contact_unread[j].valid = true;
          break;
        }
  }
  size_t hsz = g_prefs.getBytesLength("channels");
  if (hsz > 0 && hsz % sizeof(ChannelUnreadRecord) == 0) {
    ChannelUnreadRecord hrecs[MAX_UNREAD_SLOTS];
    int n = (int)(hsz / sizeof(ChannelUnreadRecord));
    if (n > MAX_UNREAD_SLOTS) n = MAX_UNREAD_SLOTS;
    g_prefs.getBytes("channels", hrecs, (size_t)n * sizeof(ChannelUnreadRecord));
    for (int i = 0; i < n; i++)
      for (int j = 0; j < MAX_UNREAD_SLOTS; j++)
        if (!g_channel_unread[j].valid) {
          g_channel_unread[j].channel_idx = (int)hrecs[i].channel_idx;
          g_channel_unread[j].count = hrecs[i].count;
          g_channel_unread[j].valid = true;
          break;
        }
  }
  g_prefs.end();
}

#else
uint32_t clamp_timeout_s(uint32_t s) { return s; }
void save_ui_prefs_nvs()  {}
void load_ui_prefs_nvs()  {}
void save_timeout_s(uint32_t s) { g_screen_timeout_s = clamp_timeout_s(s); }
void save_device_name_nvs(const char*) {}
bool load_device_name_nvs(char*, size_t) { return false; }
void save_preset_idx_nvs(int) {}
int  load_preset_idx_nvs() { return -1; }
void save_notifications_nvs() {}
void load_notifications_nvs() {}
#endif

// ---- Resolve pending TX on boot ----
void resolve_pending_in_file(const String& path) {
#if defined(ESP32)
  File src = SPIFFS.open(path, FILE_READ);
  if (!src) return;
  bool has_pending = false;
  while (src.available()) {
    String line = src.readStringUntil('\n');
    line.trim();
    if (line.startsWith("TX|")) {
      int p1 = line.indexOf('|');
      int p2 = (p1 >= 0) ? line.indexOf('|', p1 + 1) : -1;
      if (p2 > p1 && p2 + 1 < (int)line.length() && line[p2 + 1] == 'P') {
        has_pending = true; break;
      }
    }
  }
  if (!has_pending) { src.close(); return; }
  src.seek(0);
  String tmp = path + ".tmp";
  File out = SPIFFS.open(tmp, FILE_WRITE);
  if (!out) { src.close(); return; }
  while (src.available()) {
    String line = src.readStringUntil('\n');
    line.trim();
    if (line.startsWith("TX|")) {
      int p1 = line.indexOf('|');
      int p2 = (p1 >= 0) ? line.indexOf('|', p1 + 1) : -1;
      if (p2 > p1 && p2 + 1 < (int)line.length() && line[p2 + 1] == 'P')
        line = line.substring(0, p2 + 1) + "N" + line.substring(p2 + 2);
    }
    out.println(line);
  }
  src.close(); out.close();
  SPIFFS.remove(path);
  SPIFFS.rename(tmp, path);
#else
  (void)path;
#endif
}

void resolve_pending_on_boot() {
#if defined(ESP32)
  File root = SPIFFS.open("/");
  if (!root || !root.isDirectory()) return;
  File f = root.openNextFile();
  while (f) {
    String name = "/" + String(f.name());
    f.close();
    if (name.startsWith("/chat_")) resolve_pending_in_file(name);
    f = root.openNextFile();
  }
#endif
}

// ---- Contact soft-delete helpers ----
void rebuild_contacts_file_excluding(const uint8_t* pub32) {
#if defined(ESP32)
  if (!pub32) return;
  if (!SPIFFS.exists("/contacts")) return;
  File in = SPIFFS.open("/contacts", FILE_READ);
  if (!in) return;
  File out = SPIFFS.open("/contacts.tmp", FILE_WRITE);
  if (!out) { in.close(); return; }

  ContactRecord rec;
  while (in.read((uint8_t*)&rec, sizeof(rec)) == sizeof(rec)) {
    if (memcmp(rec.pub_key, pub32, 32) == 0) continue;
    out.write((const uint8_t*)&rec, sizeof(rec));
  }
  in.close();
  out.close();
  SPIFFS.remove("/contacts");
  SPIFFS.rename("/contacts.tmp", "/contacts");
#else
  (void)pub32;
#endif
}

void purge_contacts_file_all() {
#if defined(ESP32)
  if (SPIFFS.exists("/contacts")) SPIFFS.remove("/contacts");
#endif
}
