#include "../../examples/crowpanel_p4_lvgl_chat/neighbour_map_policy.h"
#include "../../examples/crowpanel_p4_lvgl_chat/advert_location_cache.h"
constexpr bool advertCacheChecks() {
  AdvertLocations::Cache<2> cache;
  uint8_t a[32] = {1, 2, 3, 4, 5};
  uint8_t b[32] = {1, 2, 3, 4, 6};
  uint8_t c[32] = {7};
  AdvertLocations::Location result;
  if (cache.size() != 0 || cache.get(0, result)) return false;
  if (cache.matchCount(nullptr, 4) != 0 || cache.matchCount(a, 3) != 0) return false;
  if (cache.findUnique(a, 4, result)) return false;
  if (cache.record(a, "No GPS", 0, 0, 1)) return false;
  if (!cache.record(a, "Over radio", 51200000, 4400000, 2)) return false;
  if (cache.size() != 1 || !cache.get(0, result) || cache.get(1, result)) return false;
  if (cache.matchCount(a, 4) != 1 || cache.matchCount(a, 32) != 1) return false;
  if (!cache.findUnique(a, 4, result) || result.lat != 51200000 || result.name[0] != 'O') return false;
  if (cache.record(a, "Old", 51300000, 4500000, 1)) return false;
  if (!cache.record(a, "New", 51300000, 4500000, 3)) return false;
  if (!cache.findUnique(a, 32, result) || result.lat != 51300000) return false;
  if (!cache.record(b, "Collision", 51000000, 4300000, 4)) return false;
  if (cache.matchCount(a, 4) != 2 || cache.matchCount(a, 32) != 1) return false;
  if (cache.findUnique(a, 4, result)) return false;
  if (!cache.findUnique(a, 32, result)) return false;
  if (!cache.record(c, "Replacement", 50000000, 4200000, 5)) return false;
  if (cache.findUnique(a, 32, result)) return false;
  if (!cache.findUnique(c, 4, result)) return false;
  cache.clear();
  return cache.size() == 0 && !cache.get(0, result) &&
    cache.matchCount(c, 4) == 0 && !cache.findUnique(c, 4, result);
}
static_assert(advertCacheChecks());
static_assert(NeighbourMap::validPosition(51.202189, 4.461754));
static_assert(NeighbourMap::validPosition(0, 4));
static_assert(NeighbourMap::validPosition(-90, -180));
static_assert(!NeighbourMap::validPosition(0, 0));
static_assert(!NeighbourMap::validPosition(91, 4));
static_assert(!NeighbourMap::validPosition(51, 181));
static_assert(NeighbourMap::entryCount(0, 20) == 0);
static_assert(NeighbourMap::entryCount(7, 20) == 0);
static_assert(NeighbourMap::entryCount(16, 20) == 0);
static_assert(NeighbourMap::entryCount(17, 20) == 1);
static_assert(NeighbourMap::entryCount(188, 65535) == 20);
static_assert(NeighbourMap::entryCount(188, 2) == 2);
constexpr bool clippingChecks() {
  double x = -1000000, y = 50, ex = 1000000, ey = 50;
  if (!NeighbourMap::clip(x, y, ex, ey, 100, 100) || x < -0.000001 || x > 0.000001 ||
      ex < 98.999999 || ex > 99.000001) return false;
  x = -10; ex = -10; y = 0; ey = 99;
  if (NeighbourMap::clip(x, y, ex, ey, 100, 100)) return false;
  x = 50; y = 50; ex = 50; ey = 50;
  return NeighbourMap::clip(x, y, ex, ey, 100, 100);
}
static_assert(clippingChecks());
