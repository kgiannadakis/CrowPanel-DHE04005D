#pragma once

#include <cstddef>

namespace mcui {

struct MapTileKey {
    int zoom = -1;
    int x = -1;
    int y = -1;

    bool operator==(const MapTileKey &other) const
    {
        return zoom == other.zoom && x == other.x && y == other.y;
    }
};

// Reserve every overlapping tile before recycling slots for the new border.
template <size_t Capacity>
bool map_tile_layout(const MapTileKey (&existing)[Capacity], int zoom, int x0, int y0,
                     int cols, int rows, int (&slots)[Capacity])
{
    if (cols <= 0 || rows <= 0 || cols > static_cast<int>(Capacity) ||
        rows > static_cast<int>(Capacity) / cols)
        return false;
    const int count = cols * rows;
    bool used[Capacity] = {};
    for (int i = 0; i < count; ++i) {
        slots[i] = -1;
        const MapTileKey wanted{zoom, x0 + i % cols, y0 + i / cols};
        for (size_t j = 0; j < Capacity; ++j) {
            if (!used[j] && existing[j] == wanted) {
                slots[i] = static_cast<int>(j);
                used[j] = true;
                break;
            }
        }
    }
    size_t next = 0;
    for (int i = 0; i < count; ++i) {
        if (slots[i] >= 0) continue;
        while (used[next]) ++next;
        slots[i] = static_cast<int>(next);
        used[next] = true;
    }
    return true;
}

} // namespace mcui
