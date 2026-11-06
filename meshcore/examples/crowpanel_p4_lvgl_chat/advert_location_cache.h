#pragma once
#include <stddef.h>
#include <stdint.h>
#include "neighbour_map_policy.h"

namespace AdvertLocations {
struct Location {
  uint8_t key[32] = {};
  char name[32] = {};
  int32_t lat = 0, lon = 0;
  uint32_t timestamp = 0;
};

// Volatile radio-derived metadata; independent of automatic contact admission.
template <size_t Capacity> class Cache {
  static_assert(Capacity > 0);
  Location entries_[Capacity] = {};
  size_t count_ = 0, next_ = 0;
  static constexpr bool matches(const uint8_t* a, const uint8_t* b, size_t bytes) {
    for (size_t i = 0; i < bytes; ++i) if (a[i] != b[i]) return false;
    return true;
  }
public:
  constexpr size_t size() const { return count_; }
  constexpr bool get(size_t index, Location& result) const {
    if (index >= count_) return false;
    result = entries_[index];
    return true;
  }
  constexpr size_t matchCount(const uint8_t* key, size_t prefix) const {
    if (!key || (prefix != 4 && prefix != 32)) return 0;
    size_t count = 0;
    for (size_t i = 0; i < count_; ++i)
      if (matches(key, entries_[i].key, prefix)) ++count;
    return count;
  }
  constexpr void clear() { count_ = next_ = 0; }
  constexpr bool record(const uint8_t* key, const char* name, int32_t lat, int32_t lon, uint32_t timestamp) {
    if (!NeighbourMap::validPosition(lat / 1.e6, lon / 1.e6)) return false;
    size_t slot = 0;
    while (slot < count_ && !matches(key, entries_[slot].key, 32)) ++slot;
    if (slot < count_ && timestamp <= entries_[slot].timestamp) return false;
    if (slot == count_) {
      if (count_ < Capacity) ++count_;
      else { slot = next_; next_ = (next_ + 1) % Capacity; }
    }
    Location& entry = entries_[slot];
    for (size_t i = 0; i < 32; ++i) entry.key[i] = key[i];
    size_t i = 0;
    for (; i < 31 && name[i]; ++i) entry.name[i] = name[i];
    for (; i < 32; ++i) entry.name[i] = 0;
    entry.lat = lat; entry.lon = lon; entry.timestamp = timestamp;
    return true;
  }
  constexpr bool findUnique(const uint8_t* key, size_t prefix, Location& result) const {
    if (prefix != 4 && prefix != 32) return false;
    size_t found = Capacity;
    for (size_t i = 0; i < count_; ++i) {
      if (!matches(key, entries_[i].key, prefix)) continue;
      if (found != Capacity) return false;
      found = i;
    }
    if (found == Capacity) return false;
    result = entries_[found];
    return true;
  }
};
}
