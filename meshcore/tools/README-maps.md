# Map tiles on SD

## Phase 1 — static 3×2 composite around Brussels

The firmware's **Maps** screen (Web Apps → "Open map") displays a
static 3×2 grid of OSM tiles at zoom 12 covering metropolitan
Brussels (~30 × 20 km). Content scrolls vertically to reach the
bottom row. This phase validates the pipeline: SD mount → LVGL
POSIX-FS driver → PNG decode → on-screen display.

### Get the tiles for your chosen area

```
python fetch_one_tile.py --lat 51.09652 --lon 4.44981 --zoom 13
```

- `--lat` / `--lon`: center point. Copy from the URL bar on
  https://www.openstreetmap.org — e.g. `#map=13/51.09652/4.44981`
  ⇒ `--zoom 13 --lat 51.09652 --lon 4.44981`.
- `--zoom`: 12 ≈ city-wide, 13 ≈ neighborhood, 14 ≈ streets named.
- Default grid is 3×2 (change with `--cols` / `--rows` if you want).

The script rate-limits to 1 request/second, uses a descriptive
User-Agent, and is idempotent (skips tiles already on disk). After
fetching it prints the exact `kTileZ` / `kTileX[]` / `kTileY[]`
constants to paste into `map_view.cpp` — so the firmware looks for
the tiles you just produced.

**Do not modify this script to bulk-download.** For Phase 2 region
coverage we'll use an offline tile-generation pipeline that doesn't
hit OSM servers.

### Copy to SD

Copy the whole `tiles/` directory into the SD card root so the final
paths are e.g. `/sdcard/tiles/12/2097/1371.png` (and 5 siblings).

### Verify

Flash firmware. Go to **Web Apps → Open map**. You should see:

- A map header bar with a back arrow + "Maps" title.
- A 768 × 512 pixel map filling the screen, with Brussels in the
  middle tile. Scroll down slightly to see the bottom row.
- Serial log line `maps: all 6 tiles loaded from SD`.

If any tile is missing, that cell of the grid is replaced by a dark
square with the missing tile's `{z}/{x}/{y}` printed inside — makes
"forgot to copy one file" a visible failure, not a silent blank.

## Phase 2 — pan / zoom + country-scale tile trees

The Maps screen now pans and zooms (zoom buttons + drag). Tiles at the
current viewport load on demand from `/sdcard/tiles/{z}/{x}/{y}.png`;
missing tiles render as dark squares labelled with their `{z}/{x}/{y}`
coords. Viewport state (lat/lon/zoom/filter) is persisted to NVS so the
next entry reopens where you left off.

### Bulk-download tiles for one or more countries

```
# See the full country list
python fetch_country_tiles.py --list

# Download Belgium + Netherlands at zooms 8-13 (default range)
python fetch_country_tiles.py --countries BE,NL

# Narrower zoom range is much smaller + faster
python fetch_country_tiles.py --countries DE --zoom-min 10 --zoom-max 12

# Estimate the tile count + size WITHOUT downloading
python fetch_country_tiles.py --countries BE,NL --dry-run
```

Typical footprints at zooms 8-13 (OSM PNG, ~14 KB/tile):

| Selection | Tiles  | Disk  | Time at 1 req/s |
| --------- | ------ | ----- | --------------- |
| LU z10-12 |    200 |  3 MB | ~3 min          |
| BE        |  8 800 | 125 MB| ~2.5 h          |
| BE + NL   | 18 600 | 265 MB| ~5 h            |

The script is idempotent (skips tiles already on disk) and de-duplicates
tiles that cross country borders. Re-run after a partial / failed run
to pick up where you left off.

**OSM tile-usage policy**: `tile.openstreetmap.org` is the default tile
source but its policy forbids bulk downloads. For country-scale pulls
stand up a local tileserver-gl from a Geofabrik OSM extract and pass
`--tile-server http://localhost:8080/styles/osm/256`. The script prints
a warning and asks for confirmation before starting any download that
exceeds `--confirm-threshold` (default 500 tiles).

### Copy to SD

Copy the whole `tiles/` directory into the SD card root so paths
resolve as `/sdcard/tiles/{z}/{x}/{y}.png`. On next device boot, open
**Maps** tab — tiles for any covered area show up as you pan + zoom.
