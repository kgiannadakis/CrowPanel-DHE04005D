#include "graphics/mcui/screens/MapTileLayout.h"
#include <cassert>
#include <cstdio>

using mcui::MapTileKey;
using mcui::map_tile_layout;

static int layout(MapTileKey (&keys)[30], int zoom, int x, int y, int cols, int rows)
{
    int slots[30];
    assert(map_tile_layout(keys, zoom, x, y, cols, rows, slots));
    bool used[30] = {};
    int reused = 0;
    for (int i = 0; i < cols * rows; ++i) {
        assert(slots[i] >= 0 && slots[i] < 30);
        assert(!used[slots[i]]);
        used[slots[i]] = true;
        const MapTileKey wanted{zoom, x + i % cols, y + i / cols};
        if (keys[slots[i]] == wanted) ++reused;
    }
    for (int i = 0; i < cols * rows; ++i)
        keys[slots[i]] = {zoom, x + i % cols, y + i / cols};
    return reused;
}

int main()
{
    MapTileKey keys[30];
    assert(layout(keys, 11, 100, 200, 5, 6) == 0);
    assert(layout(keys, 11, 100, 200, 5, 6) == 30);
    assert(layout(keys, 11, 101, 200, 5, 6) == 24);
    assert(layout(keys, 11, 100, 200, 5, 6) == 24);
    assert(layout(keys, 11, 100, 201, 5, 6) == 25);
    assert(layout(keys, 11, 101, 202, 5, 6) == 20);
    assert(layout(keys, 12, 101, 202, 5, 6) == 0);
    assert(layout(keys, 12, 101, 202, 4, 5) == 20);
    assert(layout(keys, 12, 101, 202, 5, 6) == 30);

    MapTileKey detailed[30];
    const int lastTile = (1 << 18) - 1;
    assert(layout(detailed, 18, lastTile - 4, lastTile - 5, 5, 6) == 0);
    assert(layout(detailed, 18, lastTile - 4, lastTile - 5, 5, 6) == 30);
    assert(layout(detailed, 18, lastTile - 3, lastTile - 5, 5, 6) == 24);
    assert(layout(detailed, 18, lastTile - 3, lastTile - 4, 5, 6) == 25);
    assert(layout(detailed, 17, lastTile / 2, lastTile / 2, 5, 6) == 0);
    assert(layout(detailed, 18, lastTile - 4, lastTile - 5, 5, 6) == 0);

    // Includes missing/out-of-world keys: failed lookups must also be reusable.
    MapTileKey edge[30];
    assert(layout(edge, 7, -1, -1, 5, 6) == 0);
    assert(layout(edge, 7, 0, -1, 5, 6) == 24);
    int slots[30];
    assert(!map_tile_layout(keys, 12, 0, 0, 0, 6, slots));
    assert(!map_tile_layout(keys, 12, 0, 0, 6, 6, slots));
    assert(!map_tile_layout(keys, 12, 0, 0, 31, 1, slots));
    assert(!map_tile_layout(keys, 12, 0, 0, 1, -1, slots));

    MapTileKey walk[30];
    layout(walk, 11, 500, 500, 5, 6);
    int x = 500, y = 500;
    unsigned rng = 0x31415926;
    for (int i = 0; i < 10000; ++i) {
        rng = rng * 1664525U + 1013904223U;
        const int dx = static_cast<int>(rng % 3) - 1;
        rng = rng * 1664525U + 1013904223U;
        const int dy = static_cast<int>(rng % 3) - 1;
        x += dx;
        y += dy;
        assert(layout(walk, 11, x, y, 5, 6) == (5 - (dx != 0)) * (6 - (dy != 0)));
    }
    std::puts("Map tile layout tests passed: overlap, zoom, edges, bounds, 10000 pans");
}
