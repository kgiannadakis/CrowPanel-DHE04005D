#include "repeater_credentials.h"
#include <LittleFS.h>
#include <stdio.h>
#include <string.h>

bool repeater_password_load(const uint8_t* key, char out[RepeaterPassword::kBufferBytes]) {
  if (!out) return false;
  memset(out, 0, RepeaterPassword::kBufferBytes);
  char path[RepeaterPassword::kPathBytes];
  if (!RepeaterPassword::makePath(key, path, sizeof(path))) return false;
  File file = LittleFS.open(path, "r");
  if (!file) return false;
  RepeaterPassword::Record record;
  const bool ok = file.size() == sizeof(record)
      && file.read(reinterpret_cast<uint8_t*>(&record), sizeof(record)) == sizeof(record)
      && RepeaterPassword::valid(record);
  file.close();
  if (ok) memcpy(out, record.password, sizeof(record.password));
  memset(&record, 0, sizeof(record));
  return ok;
}

bool repeater_password_save(const uint8_t* key, const char* password) {
  if (!password || strlen(password) > RepeaterPassword::kMaxBytes) return false;
  char path[RepeaterPassword::kPathBytes], temp[RepeaterPassword::kPathBytes + 4];
  if (!RepeaterPassword::makePath(key, path, sizeof(path))) return false;
  snprintf(temp, sizeof(temp), "%s.tmp", path);
  char previous[RepeaterPassword::kBufferBytes];
  const bool unchanged = repeater_password_load(key, previous) && strcmp(previous, password) == 0;
  memset(previous, 0, sizeof(previous));
  if (unchanged) return true;
  RepeaterPassword::Record record;
  memcpy(record.password, password, strlen(password));
  File file = LittleFS.open(temp, "w");
  if (!file) { memset(&record, 0, sizeof(record)); return false; }
  const bool written = file.write(reinterpret_cast<const uint8_t*>(&record), sizeof(record)) == sizeof(record);
  file.flush();
  file.close();
  memset(&record, 0, sizeof(record));
  if (!written || !LittleFS.rename(temp, path)) {
    LittleFS.remove(temp);
    return false;
  }
  char verified[RepeaterPassword::kBufferBytes];
  const bool ok = repeater_password_load(key, verified) && strcmp(verified, password) == 0;
  memset(verified, 0, sizeof(verified));
  return ok;
}

bool repeater_password_forget(const uint8_t* key) {
  char path[RepeaterPassword::kPathBytes], temp[RepeaterPassword::kPathBytes + 4];
  if (!RepeaterPassword::makePath(key, path, sizeof(path))) return false;
  // An unmounted filesystem also reports exists=false; do not call that a
  // successful deletion of a credential which may reappear after reboot.
  File root = LittleFS.open("/", "r");
  const bool available = root && root.isDirectory();
  root.close();
  if (!available) return false;
  snprintf(temp, sizeof(temp), "%s.tmp", path);
  const bool removed = !LittleFS.exists(path) || LittleFS.remove(path);
  const bool removed_temp = !LittleFS.exists(temp) || LittleFS.remove(temp);
  return removed && removed_temp;
}
