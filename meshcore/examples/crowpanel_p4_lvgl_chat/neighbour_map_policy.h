#pragma once
#include <stddef.h>

namespace NeighbourMap {
constexpr bool validPosition(double lat, double lon) {
  return lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180 && (lat != 0 || lon != 0);
}
constexpr int entryCount(size_t bytes, unsigned reported) {
  if (bytes < 8) return 0;
  const size_t available = (bytes - 8) / 9;
  return (int)(available < reported ? (available < 20 ? available : 20)
                                  : (reported < 20 ? reported : 20));
}

// Clip in double precision before conversion to LVGL's 16-bit coordinates.
constexpr bool clip(double& x, double& y, double& end_x, double& end_y, int width, int height) {
  if (width <= 0 || height <= 0) return false;
  const double dx = end_x - x, dy = end_y - y;
  const double p[] = {-dx, dx, -dy, dy};
  const double q[] = {x, width - 1.0 - x, y, height - 1.0 - y};
  double lo = 0, hi = 1;
  for (int edge = 0; edge < 4; ++edge) {
    if (p[edge] == 0) { if (q[edge] < 0) return false; }
    else {
      const double ratio = q[edge] / p[edge];
      if (p[edge] < 0) { if (ratio > lo) lo = ratio; }
      else if (ratio < hi) hi = ratio;
    }
  }
  if (lo > hi) return false;
  end_x = x + hi * dx; end_y = y + hi * dy;
  x += lo * dx; y += lo * dy;
  return true;
}
}
