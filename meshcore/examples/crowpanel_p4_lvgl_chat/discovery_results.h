#pragma once

#include <stdint.h>
#include <string.h>

constexpr int MAX_DISCOVER_RESULTS = 64;

struct DiscoveryResult {
  uint8_t pub_key[32];
  char name[32];
  float rssi;
  int8_t rx_snr_x4;
  int8_t request_snr_x4;
  uint16_t replies;
};

class DiscoveryResults {
 public:
  void clear() { count_ = 0; }

  void record(const uint8_t* key, const char* name, float rssi,
              int8_t rx_snr_x4, int8_t request_snr_x4) {
    int i = 0;
    while (i < count_ && memcmp(results_[i].pub_key, key, 32) != 0) ++i;
    if (i == count_) {
      if (count_ == MAX_DISCOVER_RESULTS) return;
      results_[count_++] = {};
      memcpy(results_[i].pub_key, key, 32);
    }
    DiscoveryResult& r = results_[i];
    strncpy(r.name, name, sizeof(r.name) - 1);
    r.name[sizeof(r.name) - 1] = '\0';
    r.rssi = rssi;
    r.rx_snr_x4 = rx_snr_x4;
    r.request_snr_x4 = request_snr_x4;
    if (r.replies < UINT16_MAX) ++r.replies;
  }

  int snapshot(DiscoveryResult* out, int capacity) const {
    if (!out || capacity <= 0) return 0;
    const int n = capacity < count_ ? capacity : count_;
    memcpy(out, results_, n * sizeof(*out));
    return n;
  }

  bool find(const uint8_t* key, DiscoveryResult& out) const {
    for (int i = 0; i < count_; ++i) {
      if (memcmp(results_[i].pub_key, key, 32) == 0) {
        out = results_[i];
        return true;
      }
    }
    return false;
  }

 private:
  DiscoveryResult results_[MAX_DISCOVER_RESULTS] = {};
  int count_ = 0;
};
