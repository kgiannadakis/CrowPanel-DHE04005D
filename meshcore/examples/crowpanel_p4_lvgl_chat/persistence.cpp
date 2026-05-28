// ============================================================
// persistence.cpp — LittleFS chat files, NVS prefs, contact records
// ============================================================

#include "persistence.h"
#include "app_globals.h"
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
  // Only mark dirty if the value actually changed, so we don't pound
  // NVS every time load_chat_from_file re-sets the same total.
  if (g_read_pos[i].msg_count != count) {
    g_read_pos[i].msg_count = count;
    g_notifications_dirty = true;   // main loop flushes via save_notifications_nvs
  }
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

// ---- Coalesced chat-line writes ----
// Each incoming message used to do a full LittleFS open/append/close
// per line. Under a busy channel that produced one flash I/O per
// message — visible as a scroll hitch in the chat view. Now we buffer
// formatted lines and flush as a batch (grouped by chat key, so each
// destination file opens once per flush). Trigger points:
//   1. 200 ms of quiet since the last append (main-loop drain).
//   2. Buffer full (new append forces a flush before enqueueing).
//   3. Chat switch / exit (chat_append_flush_sync).
//   4. load_chat_from_file() for the same key (see below).
// Power-loss window: any messages appended in the last 200 ms may be
// lost on a hard reset. Acceptable tradeoff for the responsiveness
// win; the device has no battery anyway so a hard reset mostly only
// happens on USB unplug.
#if defined(ESP32)
static void compact_chat_file(const String& path, int keep_last_n);

struct PendingChatLine {
  bool   active;
  char   key[20];     // "ct_XXXXXXXXXXXX" = 15 chars; "ch_NN" much less
  String line;        // fully formatted "TX|...\n" or "RX|...\n"
};
static const int kPendingMax = 32;
static PendingChatLine s_pending[kPendingMax];
static int s_pending_count = 0;
#endif

// millis() timestamp at which the main loop should flush pending
// lines. 0 = nothing pending. Declared extern in persistence.h so
// main.cpp's housekeeping block can poll it without another global.
uint32_t g_chat_flush_deadline_ms = 0;
static const uint32_t kChatFlushDelayMs = 200;

#if defined(ESP32)
static void chat_flush_all_now() {
  if (s_pending_count == 0) return;
  // Walk pending; for each still-active slot, open its chat file once
  // and coalesce every pending line that targets the same key into
  // one open/write/close cycle. Ordering is preserved because we
  // scan slots in index order.
  for (int i = 0; i < kPendingMax; i++) {
    if (!s_pending[i].active) continue;
    const char* this_key = s_pending[i].key;
    String path = chat_path_for(String(this_key));
    File f = LittleFS.open(path, FILE_APPEND);
    if (!f) f = LittleFS.open(path, FILE_WRITE);
    if (!f) {
      // Mark as inactive so we don't loop forever if writes keep
      // failing; drop the line.
      s_pending[i].active = false;
      continue;
    }
    for (int j = i; j < kPendingMax; j++) {
      if (!s_pending[j].active) continue;
      if (strcmp(s_pending[j].key, this_key) != 0) continue;
      f.print(s_pending[j].line);
      s_pending[j].active = false;
      s_pending[j].line   = String();   // release the String heap
    }
    f.close();
    // IMPORTANT (UI smoothness): do NOT compact on every flush.
    // compact_chat_file() does a full-file pass; running it per append batch
    // creates sustained flash I/O stalls that hitch scrolling/navigation.
    //
    // Retention compaction remains in load_chat_from_file() where it's
    // naturally amortized during chat-open, not during high-frequency RX/TX.
  }
  s_pending_count = 0;
  g_chat_flush_deadline_ms = 0;
}

static bool chat_enqueue_line(const String& key, const String& line) {
  if (s_pending_count >= kPendingMax) {
    // Force-flush to free slots. Safe: we're about to write these
    // lines anyway; the caller is just pushing another one behind.
    chat_flush_all_now();
  }
  for (int i = 0; i < kPendingMax; i++) {
    if (s_pending[i].active) continue;
    s_pending[i].active = true;
    StrHelper::strncpy(s_pending[i].key, key.c_str(), sizeof(s_pending[i].key));
    s_pending[i].line = line;   // String copy (heap)
    s_pending_count++;
    g_chat_flush_deadline_ms = millis() + kChatFlushDelayMs;
    return true;
  }
  return false;   // shouldn't happen after the force-flush above
}
#endif

// Public API: called from main-loop housekeeping when the deadline has
// passed. main.cpp polls g_chat_flush_deadline_ms like g_ui_prefs_*.
void chat_append_flush_if_due() {
#if defined(ESP32)
  chat_flush_all_now();
#endif
}

// Synchronous flush: called by chat switch / exit / load so readers
// never see stale disk state. Cheap if nothing's pending.
void chat_append_flush_sync() {
#if defined(ESP32)
  if (s_pending_count > 0) chat_flush_all_now();
#endif
}

void append_chat_to_file(const String& key, bool out, const char* msg, uint32_t msg_ts, const char* signal_info) {
#if defined(ESP32)
  if ((LittleFS.totalBytes() - LittleFS.usedBytes()) < 4096) {
    serialmon_append("LittleFS full - chat write skipped");
    return;
  }
  // Format the line now (timestamp must be captured at arrival time,
  // not at flush time). Writing is deferred to the next flush.
  String line;
  line.reserve(96);
  uint32_t ts = msg_ts ? msg_ts : rtc_clock.getCurrentTime();
  if (out) {
    line += "TX|";
    line += ts;
    line += "|P|";
  } else {
    String safe_signal = signal_info ? sanitize_ascii_string(signal_info) : "";
    safe_signal.replace('|', ',');
    line += "RX|";
    line += ts;
    line += "|";
    if (safe_signal.length()) line += safe_signal;
    line += "|";
  }
  line += msg;
  line += '\n';
  chat_enqueue_line(key, line);
#else
  (void)key; (void)out; (void)msg; (void)msg_ts; (void)signal_info;
#endif
}

// Strip the `[HH:MM:SS] ` timestamp prefix and (for PMs/channels) the
// `Name: ` sender prefix from a raw chat-file RX body, returning just
// the message text. Mirrors the parsing chat_add() performs before
// rendering the body label, so a bare message body queued by the
// translator can be matched back to a file line or a bubble label.
static String strip_chat_body_prefix(const String& raw) {
  String s = raw;
  if (s.startsWith("[")) {
    int rb = s.indexOf(']');
    if (rb > 0 && rb < 16) {
      int cut = rb + 1;
      if (cut < (int)s.length() && s[cut] == ' ') cut++;
      s = s.substring(cut);
    }
  }
  int sep = s.indexOf(": ");
  if (sep > 0) s = s.substring(sep + 2);
  return s;
}

void append_translation_to_last_rx(const String& key, const char* match_text, const char* translation) {
#if defined(ESP32)
  if (!translation || !translation[0]) return;
  if (!match_text || !match_text[0]) return;
  chat_append_flush_sync();   // ensure we see freshly-appended RX lines
  String path = chat_path_for(key);
  File f = LittleFS.open(path, FILE_READ);
  if (!f) return;

  std::vector<String> lines;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length()) lines.push_back(line);
  }
  f.close();

  // Walk the file in reverse and attach the translation to the newest
  // RX line whose body (everything after the third '|' pipe, stripped
  // of any existing {{TR}} marker — there shouldn't be one, but be
  // defensive) equals match_text. Matching by body content means
  // out-of-order completion of concurrent translations lands each one
  // on its correct source message.
  bool appended = false;
  for (int i = (int)lines.size() - 1; i >= 0; i--) {
    if (!lines[i].startsWith("RX|")) continue;
    if (lines[i].indexOf("{{TR}}") >= 0) continue;  // already translated
    // Parse RX|ts|sig|body — three pipes before body.
    int p1 = lines[i].indexOf('|');
    int p2 = (p1 >= 0) ? lines[i].indexOf('|', p1 + 1) : -1;
    int p3 = (p2 >= 0) ? lines[i].indexOf('|', p2 + 1) : -1;
    if (p3 < 0) continue;
    String body = lines[i].substring(p3 + 1);
    if (strip_chat_body_prefix(body) != match_text) continue;
    lines[i] += "{{TR}}";
    lines[i] += translation;
    appended = true;
    break;
  }
  if (!appended) return;  // source no longer on disk (chat cleared, message deleted, etc.)

  esp_task_wdt_reset();
  File w = LittleFS.open(path, FILE_WRITE);
  if (!w) return;
  for (size_t i = 0; i < lines.size(); i++) {
    w.println(lines[i]);
  }
  w.close();
#else
  (void)key; (void)match_text; (void)translation;
#endif
}

#if defined(ESP32)
// Scan `f` forward; for every line starting with "TX|" that also matches
// `ts_match` (empty string = accept all TX lines), record the byte
// offsets of the status field. Leaves `out_*` unset if no match found.
// `out_status_offset` is the absolute byte offset of the first char of
// the status field; `out_status_length` is the field's current length
// (the span between the 2nd and 3rd '|').
static bool scan_tx_status_field(File& f, const String& ts_match,
                                 int* out_line_start,
                                 int* out_status_offset,
                                 int* out_status_length) {
  int best_line_start = -1, best_status_off = -1, best_status_len = -1;
  int offset = 0;
  while (f.available()) {
    int ls = offset;
    String line = f.readStringUntil('\n');
    offset = (int)f.position();
    line.trim();
    if (!line.startsWith("TX|")) continue;
    int p1 = 2;                                  // index of first '|' in "TX|"
    int p2 = line.indexOf('|', p1 + 1);
    int p3 = (p2 >= 0) ? line.indexOf('|', p2 + 1) : -1;
    if (p2 < 0 || p3 < 0 || p3 <= p2) continue;
    if (ts_match.length()) {
      String file_ts = line.substring(p1 + 1, p2);
      if (file_ts != ts_match) continue;
      best_line_start = ls;
      best_status_off = ls + p2 + 1;
      best_status_len = p3 - (p2 + 1);
      break;                                     // match by ts → stop
    }
    // No ts filter: keep last TX line seen.
    best_line_start = ls;
    best_status_off = ls + p2 + 1;
    best_status_len = p3 - (p2 + 1);
  }
  if (best_line_start < 0) return false;
  *out_line_start    = best_line_start;
  *out_status_offset = best_status_off;
  *out_status_length = best_status_len;
  return true;
}

// Rewrite path — only used when the new status string has a different
// length than the existing one (the on-disk line would shift). Shared
// by update_last_tx_status_in_file and update_tx_status_by_msg_ts.
static void rewrite_tx_status(const String& path, int target_line_start,
                              const String& new_status_str) {
  File src = LittleFS.open(path, FILE_READ);
  if (!src) return;
  String tmp = path + ".tmp";
  File dst = LittleFS.open(tmp, FILE_WRITE);
  if (!dst) { src.close(); return; }

  int offset = 0;
  while (src.available()) {
    int line_start = offset;
    String line = src.readStringUntil('\n');
    offset = (int)src.position();
    line.trim();
    if (!line.length()) continue;

    if (line_start == target_line_start && line.startsWith("TX|")) {
      int p1 = line.indexOf('|');
      int p2 = (p1 >= 0) ? line.indexOf('|', p1 + 1) : -1;
      int p3 = (p2 >= 0) ? line.indexOf('|', p2 + 1) : -1;
      if (p2 >= 0 && p3 >= 0 && p3 > p2) {
        line = line.substring(0, p2 + 1) + new_status_str + line.substring(p3);
      }
    }
    dst.println(line);
  }
  src.close(); dst.close();
  LittleFS.remove(path);
  LittleFS.rename(tmp, path);
}
#endif

void update_last_tx_status_in_file(const String& key, char status, int16_t repeat_count) {
#if defined(ESP32)
  String new_status_str;
  if (status == 'R' && repeat_count >= 0) {
    new_status_str = String("R") + String((int)repeat_count);
  } else {
    new_status_str = String((char)status);
  }

  String path = chat_path_for(key);
  File f = LittleFS.open(path, FILE_READ);
  if (!f) return;
  int line_start = 0, status_off = 0, status_len = 0;
  bool found = scan_tx_status_field(f, String(), &line_start, &status_off, &status_len);
  f.close();
  if (!found) return;

  // Fast path: status string is the same length as what's on disk — seek
  // and overwrite one field, no rewrite. This is the common case
  // (single-char statuses transitioning Sending→Sent→Delivered etc.).
  if ((int)new_status_str.length() == status_len) {
    File rw = LittleFS.open(path, "r+");
    if (rw) {
      rw.seek(status_off);
      rw.write((const uint8_t*)new_status_str.c_str(), new_status_str.length());
      rw.close();
      return;
    }
    // Open-for-patch failed — fall through to whole-file rewrite.
  }

  // Slow path: length changed (typically single-char → "R<n>"), rewrite.
  rewrite_tx_status(path, line_start, new_status_str);
#else
  (void)key; (void)status; (void)repeat_count;
#endif
}

void update_tx_status_by_msg_ts(const String& key, uint32_t msg_ts, char status) {
#if defined(ESP32)
  if (!msg_ts) return;
  chat_append_flush_sync();   // TX line must be on disk before status update
  String ts_str = String(msg_ts);
  String path = chat_path_for(key);
  File f = LittleFS.open(path, FILE_READ);
  if (!f) return;
  int line_start = 0, status_off = 0, status_len = 0;
  bool found = scan_tx_status_field(f, ts_str, &line_start, &status_off, &status_len);
  f.close();
  if (!found) return;

  // Single-char status: always length 1. Patch in place.
  String new_status_str = String((char)status);
  if ((int)new_status_str.length() == status_len) {
    File rw = LittleFS.open(path, "r+");
    if (rw) {
      rw.seek(status_off);
      rw.write((const uint8_t*)new_status_str.c_str(), new_status_str.length());
      rw.close();
      return;
    }
  }
  rewrite_tx_status(path, line_start, new_status_str);
#else
  (void)key; (void)msg_ts; (void)status;
#endif
}

// Maximum messages held in the in-memory ring while loading a chat.
// Bumped from 30 to 2000 so a chat shows ~7 days of history even on busy
// channels (the on-disk file is already pruned to that window by
// kChatHistoryKeepSeconds, so this is effectively "show everything we
// kept"). The ring is allocated on the heap (see std::vector below) so
// the larger size does not blow the LVGL/loop task stack.
static const int MAX_DISPLAY_MESSAGES = 2000;

// Time-based retention: keep chat lines from the last 7 days.
// This survives reboot/power loss because messages are persisted on disk
// with timestamps and pruning is applied when loading chat history.
static const uint32_t kChatHistoryKeepDays    = 7;
static const uint32_t kChatHistoryKeepSeconds = kChatHistoryKeepDays * 24UL * 60UL * 60UL;

#if defined(ESP32)
// Prune `path` to TX/RX lines newer than (now - keep_seconds), where now
// comes from RTC. If clock is not valid yet, keep file unchanged.
// Returns the number of kept TX/RX lines after pruning.
static int prune_chat_file_to_recent_seconds(const String& path, uint32_t keep_seconds) {
  uint32_t now = rtc_clock.getCurrentTime();
  if (keep_seconds == 0 || now < keep_seconds) {
    File src_count = LittleFS.open(path, FILE_READ);
    if (!src_count) return 0;
    int kept = 0;
    while (src_count.available()) {
      String line = src_count.readStringUntil('\n');
      line.trim();
      if (line.startsWith("TX|") || line.startsWith("RX|")) kept++;
    }
    src_count.close();
    return kept;
  }
  uint32_t cutoff = now - keep_seconds;

  File src = LittleFS.open(path, FILE_READ);
  if (!src) return 0;
  int total = 0;
  int keep = 0;
  while (src.available()) {
    String line = src.readStringUntil('\n');
    line.trim();
    if (!line.startsWith("TX|") && !line.startsWith("RX|")) continue;
    total++;
    int p1 = line.indexOf('|');
    int p2 = (p1 >= 0) ? line.indexOf('|', p1 + 1) : -1;
    if (p1 < 0 || p2 < 0) continue;
    uint32_t ts = (uint32_t)strtoul(line.substring(p1 + 1, p2).c_str(), nullptr, 10);
    if (ts >= cutoff) keep++;
  }
  if (keep == total) { src.close(); return keep; }

  src.seek(0);
  String tmp = path + ".tmp";
  File dst = LittleFS.open(tmp, FILE_WRITE);
  if (!dst) { src.close(); return total; }

  int kept = 0;
  while (src.available()) {
    String line = src.readStringUntil('\n');
    line.trim();
    if (!line.startsWith("TX|") && !line.startsWith("RX|")) continue;
    int p1 = line.indexOf('|');
    int p2 = (p1 >= 0) ? line.indexOf('|', p1 + 1) : -1;
    if (p1 < 0 || p2 < 0) continue;
    uint32_t ts = (uint32_t)strtoul(line.substring(p1 + 1, p2).c_str(), nullptr, 10);
    if (ts < cutoff) continue;
    dst.println(line);
    kept++;
  }
  src.close();
  dst.close();
  LittleFS.remove(path);
  LittleFS.rename(tmp, path);
  Serial.printf("Chat retention: %s %d->%d lines (%u days)\n",
                path.c_str(), total, kept, (unsigned)kChatHistoryKeepDays);
  return kept;
}
#endif

void load_chat_from_file(const String& key) {
  // Publish the active chat key BEFORE chat_clear() so translate's
  // live-update path can no longer accidentally target a bubble that
  // belongs to the chat we're about to replace. (translate_loop()
  // reads g_current_chat_key before deciding whether to touch
  // ui_chatpanel.)
  strncpy(g_current_chat_key, key.c_str(), sizeof(g_current_chat_key) - 1);
  g_current_chat_key[sizeof(g_current_chat_key) - 1] = '\0';
  chat_clear();
#if defined(ESP32)
  // Flush any buffered appends so we read the very latest state, not
  // whatever was on disk 200 ms ago.
  chat_append_flush_sync();
  String path = chat_path_for(key);
  File f = LittleFS.open(path, FILE_READ);
  if (!f) return;

  // Single-pass: count all valid lines AND keep the newest
  // MAX_DISPLAY_MESSAGES in a ring. Previously this was two full-file
  // reads (count, then seek(0) and parse) — the ring collapses that
  // to one pass, ~halving chat-open latency on large files.
  // Heap allocation — at MAX_DISPLAY_MESSAGES = 2000 a stack array would
  // be ~32 KB of String headers and would overflow the task stack.
  std::vector<String> ring(MAX_DISPLAY_MESSAGES);
  int   ring_head  = 0;
  int   ring_count = 0;
  int   total      = 0;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (!line.startsWith("TX|") && !line.startsWith("RX|")) continue;
    ring[ring_head] = line;
    ring_head = (ring_head + 1) % MAX_DISPLAY_MESSAGES;
    if (ring_count < MAX_DISPLAY_MESSAGES) ring_count++;
    total++;
  }
  f.close();

  int last_read = read_pos_get(key.c_str());
  int skip = total - ring_count;            // messages NOT in the ring
  // divider_at is the display-position (post-skip) where the "NEW
  // MESSAGES" divider goes. If last_read is unknown or all messages
  // are already read, no divider. If last_read is BEFORE the start
  // of the visible window (very stale — many unread messages pushed
  // out by MAX_DISPLAY_MESSAGES), clamp to 0 so the divider sits
  // at the very top: every displayed message is unread.
  int divider_at;
  if (last_read < 0 || last_read >= total) {
    divider_at = -1;                      // no unread → no divider
  } else if (last_read < skip) {
    divider_at = 0;                       // all visible are unread
  } else {
    divider_at = last_read - skip;
  }

  int display_idx = 0;
  bool divider_inserted = false;
  lv_obj_t* new_msg_divider = nullptr;   // captured for post-load scroll
  g_loading_history = true;

  // Iterate the ring in insertion order. Oldest entry sits at ring_head
  // when the ring is full; otherwise entries start at index 0.
  const int start = (ring_count == MAX_DISPLAY_MESSAGES) ? ring_head : 0;
  for (int i = 0; i < ring_count; i++) {
    String& line = ring[(start + i) % MAX_DISPLAY_MESSAGES];
    bool out = line.startsWith("TX|");

    if (!divider_inserted && divider_at >= 0 && display_idx >= divider_at) {
      divider_inserted = true;
      if (ui_chatpanel) {
        lv_obj_t* div = lv_obj_create(ui_chatpanel);
        new_msg_divider = div;   // captured so we can scroll to it below
        lv_obj_set_width(div, lv_pct(100));
        lv_obj_set_height(div, 28);
        lv_obj_set_style_bg_opa(div, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(div, 0, 0);
        lv_obj_set_style_pad_all(div, 0, 0);
        lv_obj_clear_flag(div, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t* lineL = lv_obj_create(div);
        lv_obj_set_size(lineL, lv_pct(30), 1);
        lv_obj_set_style_bg_color(lineL, lv_color_hex(g_theme->new_divider), 0);
        lv_obj_set_style_bg_opa(lineL, LV_OPA_COVER, 0);
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
        lv_obj_set_style_bg_opa(lineR, LV_OPA_COVER, 0);
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

    // Layer C: auto-translate on re-entry, but only for RX lines that
    // never carried a {{TR}} marker at all — those messages arrived
    // while Wi-Fi was down (or before translate landed) and have never
    // been attempted. Lines with a marker, even an empty one, are
    // considered "already handled" and are left alone, so re-opening
    // a chat doesn't spam translate.googleapis.com with re-requests
    // for every visible message on every entry.
    extern bool g_wifi_connected;
    if (!out && g_auto_translate_enabled && g_wifi_connected && tr_pos < 0 && body.length()) {
      String bare = strip_chat_body_prefix(body);
      if (bare.length()) translate_request_to_file(bare.c_str(), key.c_str());
    }
  }
  g_loading_history = false;

  read_pos_set(key.c_str(), total);

  // Time-based retention pruning: keep only the last 7 days on disk.
  // This enforces persistent history by time window rather than count.
  int kept_after_prune = prune_chat_file_to_recent_seconds(path, kChatHistoryKeepSeconds);
  if (kept_after_prune > 0 && kept_after_prune != total) {
    read_pos_set(key.c_str(), kept_after_prune);
  }

  // Scroll priority: if a "NEW MESSAGES" divider was created (i.e.
  // there are unread messages), position the view so the divider
  // sits near the top — the user reads forward from the OLDEST
  // unread, not from the newest. If no divider (all caught up),
  // fall through to the normal "jump to newest" behaviour so the
  // chat opens on the most recent message.
  //
  // We DEFER the divider scroll to the main-loop drain rather than
  // scrolling inline here. Scrolling inline was unreliable: the
  // chat panel's flex layout hadn't always finished computing bubble
  // positions, so lv_obj_get_y(divider) sometimes returned 0 and we
  // ended up scrolling to the top instead of the divider. By the
  // time the main loop picks it up (next iteration, after LVGL's
  // own task has ticked), the layout is settled.
  if (new_msg_divider && ui_chatpanel) {
    g_pending_scroll_to_divider = new_msg_divider;
    g_suppress_next_scroll_bottom = true;
  } else {
    chat_scroll_to_newest();
  }
#else
  (void)key;
#endif
}

void delete_chat_file_for_key(const String& key) {
#if defined(ESP32)
  chat_append_flush_sync();   // avoid a pending enqueue re-creating the file after remove
  String path = chat_path_for(key);
  LittleFS.remove(path);   // no-op if the file doesn't exist
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
  // Debounced: just mark dirty and timestamp; the main-loop housekeeping
  // block flushes once the stream of toggle events goes quiet.
  g_ui_prefs_save_dirty = true;
  g_ui_prefs_save_ms    = millis();
}
void save_ui_prefs_nvs_now() {
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
  g_prefs.putUChar("notif_en",     g_notifications_enabled ? 1 : 0);
  g_prefs.putUChar("spk_vol",      g_speaker_volume);
  g_prefs.putUChar("snd_idx",      g_notification_sound_idx);
  g_prefs.putUChar("hdr_bytes",    (g_text_header_bytes >= 1 && g_text_header_bytes <= 3) ? g_text_header_bytes : 1);
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
  g_notifications_enabled = g_prefs.getUChar("notif_en",   1) != 0;
  g_speaker_volume        = g_prefs.getUChar("spk_vol", 60);
  if (g_speaker_volume > 100) g_speaker_volume = 60;
  g_notification_sound_idx = g_prefs.getUChar("snd_idx", 0);
  g_text_header_bytes      = g_prefs.getUChar("hdr_bytes", 1);
  if (g_text_header_bytes < 1 || g_text_header_bytes > 3) g_text_header_bytes = 1;
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
// Persisted read-position tracker so the "NEW MESSAGES" divider
// survives a reboot. Without this, g_read_pos starts fresh on every
// boot → read_pos_get() returns -1 → the divider logic short-circuits
// to "no divider, scroll to newest" even when unread messages exist.
struct ReadPosRecord { char key[20]; int32_t msg_count; };
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
  // Serialise g_read_pos so the NEW-MESSAGES divider position survives
  // a reboot. Without persistence, last_read=-1 on the next boot means
  // load_chat_from_file falls through to chat_scroll_to_newest.
  ReadPosRecord rprecs[MAX_READ_POS];
  int np = 0;
  for (int i = 0; i < MAX_READ_POS; i++) {
    if (!g_read_pos[i].valid) continue;
    memset(&rprecs[np], 0, sizeof(rprecs[np]));
    strncpy(rprecs[np].key, g_read_pos[i].key, sizeof(rprecs[np].key) - 1);
    rprecs[np].msg_count = g_read_pos[i].msg_count;
    np++;
  }

  g_prefs.begin("notif", false);
  if (nc > 0) {
    g_prefs.putBytes("contacts", crecs, (size_t)nc * sizeof(ContactUnreadRecord));
  } else if (g_prefs.isKey("contacts")) {
    // Only call remove() if the key actually exists — otherwise Arduino's
    // Preferences layer logs nvs_erase_key NOT_FOUND as an [E] error for
    // a no-op. isKey() is a cheap check and keeps the serial clean.
    g_prefs.remove("contacts");
  }
  if (nh > 0) {
    g_prefs.putBytes("channels", hrecs, (size_t)nh * sizeof(ChannelUnreadRecord));
  } else if (g_prefs.isKey("channels")) {
    g_prefs.remove("channels");
  }
  if (np > 0) {
    g_prefs.putBytes("readpos", rprecs, (size_t)np * sizeof(ReadPosRecord));
  } else if (g_prefs.isKey("readpos")) {
    g_prefs.remove("readpos");
  }
  g_prefs.end();
}

void load_notifications_nvs() {
  g_prefs.begin("notif", true);
  // isKey() gates the getBytesLength() calls so a fresh device (or one
  // just after a purge) doesn't log "[E] getBytesLength: contacts
  // NOT_FOUND" at boot. The keys only exist after the first save.
  if (g_prefs.isKey("contacts")) {
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
  }
  if (g_prefs.isKey("channels")) {
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
  }
  // Restore read positions so the NEW MESSAGES divider can be placed
  // correctly in load_chat_from_file() on the first open after reboot.
  if (g_prefs.isKey("readpos")) {
    size_t rsz = g_prefs.getBytesLength("readpos");
    if (rsz > 0 && rsz % sizeof(ReadPosRecord) == 0) {
      ReadPosRecord rprecs[MAX_READ_POS];
      int n = (int)(rsz / sizeof(ReadPosRecord));
      if (n > MAX_READ_POS) n = MAX_READ_POS;
      g_prefs.getBytes("readpos", rprecs, (size_t)n * sizeof(ReadPosRecord));
      for (int i = 0; i < n; i++) {
        if (!rprecs[i].key[0]) continue;
        for (int j = 0; j < MAX_READ_POS; j++) {
          if (g_read_pos[j].valid) continue;
          strncpy(g_read_pos[j].key, rprecs[i].key, sizeof(g_read_pos[j].key) - 1);
          g_read_pos[j].key[sizeof(g_read_pos[j].key) - 1] = '\0';
          g_read_pos[j].msg_count = (int)rprecs[i].msg_count;
          g_read_pos[j].valid = true;
          break;
        }
      }
    }
  }
  g_prefs.end();
}

#else
uint32_t clamp_timeout_s(uint32_t s) { return s; }
void save_ui_prefs_nvs()  {}
void save_ui_prefs_nvs_now() {}
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
  File src = LittleFS.open(path, FILE_READ);
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
  File out = LittleFS.open(tmp, FILE_WRITE);
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
  LittleFS.remove(path);
  LittleFS.rename(tmp, path);
#else
  (void)path;
#endif
}

void resolve_pending_on_boot() {
#if defined(ESP32)
  File root = LittleFS.open("/");
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
  File in = LittleFS.open("/contacts", FILE_READ);
  if (!in) return;
  File out = LittleFS.open("/contacts.tmp", FILE_WRITE);
  if (!out) { in.close(); return; }

  ContactRecord rec;
  while (in.read((uint8_t*)&rec, sizeof(rec)) == sizeof(rec)) {
    if (memcmp(rec.pub_key, pub32, 32) == 0) continue;
    out.write((const uint8_t*)&rec, sizeof(rec));
  }
  in.close();
  out.close();
  LittleFS.remove("/contacts");
  LittleFS.rename("/contacts.tmp", "/contacts");
#else
  (void)pub32;
#endif
}

void purge_contacts_file_all() {
#if defined(ESP32)
  LittleFS.remove("/contacts");   // no-op if absent
#endif
}
