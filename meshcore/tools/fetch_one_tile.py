#!/usr/bin/env python3
"""
fetch_one_tile.py — Download a small block of OSM tiles for Phase 1
map-view testing and print the firmware-side constants to paste.

Give it a lat/lon/zoom (match the URL bar on
https://www.openstreetmap.org e.g. #map=13/51.09652/4.44981), and it:

  1. Computes the slippy-map tile coordinates for that point.
  2. Downloads a 3x2 block of tiles (3 wide, 2 tall) centered
     horizontally on the given lon, with the target tile in the
     TOP row (so the firmware shows it without scrolling).
  3. Prints the C constants to paste into
     examples/crowpanel_p4_lvgl_chat/map_view.cpp.

Usage:
    python fetch_one_tile.py --lat 51.09652 --lon 4.44981 --zoom 13

The script rate-limits to 1 request/second and uses a descriptive
User-Agent, staying well within OSM's individual-test-use policy.
Re-runs skip tiles already on disk.

DO NOT modify this for bulk downloads. For Phase 2 region-wide
coverage (Belgium + Netherlands at multiple zooms = ~10 000 tiles)
we'll use an offline tile-generation pipeline that doesn't hit OSM
servers.
"""

from __future__ import annotations

import argparse
import math
import sys
import time
from pathlib import Path
from urllib.request import Request, urlopen
from urllib.error import URLError


USER_AGENT = "meshcore-crowpanel-dhe04005d/Phase1 (single-tile-test)"
REQUEST_DELAY_S = 1.0


def latlon_to_tile(lat_deg: float, lon_deg: float, zoom: int) -> tuple[int, int]:
    """Standard slippy-map projection, matches the formulae on
    https://wiki.openstreetmap.org/wiki/Slippy_map_tilenames."""
    n = 2 ** zoom
    x = int((lon_deg + 180.0) / 360.0 * n)
    lat_rad = math.radians(lat_deg)
    y = int((1.0 - math.asinh(math.tan(lat_rad)) / math.pi) / 2.0 * n)
    # Clamp to valid range (web Mercator blows up at ±85.0511°).
    x = max(0, min(x, n - 1))
    y = max(0, min(y, n - 1))
    return x, y


def fetch(url: str) -> bytes:
    req = Request(url, headers={"User-Agent": USER_AGENT})
    with urlopen(req, timeout=15) as resp:
        return resp.read()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--lat", required=True, type=float,
                    help="Center latitude (degrees)")
    ap.add_argument("--lon", required=True, type=float,
                    help="Center longitude (degrees)")
    ap.add_argument("--zoom", required=True, type=int,
                    help="Zoom level (0-19). 12=city, 13=neighborhood, 14=street")
    ap.add_argument("--cols", default=8, type=int,
                    help="Grid width in tiles (default: 8 — enough to fill "
                         "the 800 px viewport plus panning buffer)")
    ap.add_argument("--rows", default=6, type=int,
                    help="Grid height in tiles (default: 6 — enough to fill "
                         "the 440 px viewport plus panning buffer)")
    ap.add_argument("--out-dir", default=Path("."), type=Path,
                    help="Output directory (default: current)")
    args = ap.parse_args()

    cx, cy = latlon_to_tile(args.lat, args.lon, args.zoom)
    print(f"center:    ({args.lat:.5f}, {args.lon:.5f}) @ zoom {args.zoom}")
    print(f"center tile: ({cx}, {cy})")

    # Grid centered BOTH horizontally and vertically on (cx, cy). For
    # odd counts the center tile sits exactly in the middle; for even
    # counts we bias one tile north/west of center, and the firmware's
    # 1-tile edge padding covers the off-center case either way.
    half_cols = (args.cols - 1) // 2
    xs = [cx - half_cols + i for i in range(args.cols)]
    half_rows = (args.rows - 1) // 2
    ys = [cy - half_rows + i for i in range(args.rows)]

    tiles = [(x, y) for y in ys for x in xs]
    n = len(tiles)
    print(f"grid:      {args.cols}x{args.rows} = {n} tiles")
    print()

    # Fetch each tile from OSM. 1 s delay between requests.
    # Individual tile failures (transient 4xx/5xx, network blip) do NOT
    # abort the whole run — we log them and move on. Re-running the
    # script picks up where we left off thanks to the skip-if-present
    # check.
    failures: list[tuple[int, int, str]] = []
    for i, (x, y) in enumerate(tiles):
        url = f"https://tile.openstreetmap.org/{args.zoom}/{x}/{y}.png"
        out = args.out_dir / "tiles" / str(args.zoom) / str(x) / f"{y}.png"
        out.parent.mkdir(parents=True, exist_ok=True)

        if out.is_file() and out.stat().st_size > 0:
            print(f"[{i+1}/{n}] skip  {out} (already present)")
            continue

        print(f"[{i+1}/{n}] fetch {url}")
        try:
            data = fetch(url)
        except URLError as e:
            sys.stderr.write(f"          FAILED: {e}\n")
            failures.append((x, y, str(e)))
            time.sleep(REQUEST_DELAY_S)
            continue

        out.write_bytes(data)
        print(f"          saved {out} ({len(data)} bytes)")
        if i < n - 1:
            time.sleep(REQUEST_DELAY_S)

    if failures:
        print()
        print(f"WARNING: {len(failures)} tile(s) failed to download:")
        for x, y, err in failures:
            print(f"  {args.zoom}/{x}/{y}.png  -- {err}")
        print("Re-run the script to retry just the failed tiles.")

    # Emit the exact constants to paste into map_view.cpp so the
    # firmware looks for the tiles we just produced. Cols as ascending
    # left-to-right, rows as ascending top-to-bottom.
    print()
    print("---- Paste these into examples/crowpanel_p4_lvgl_chat/map_view.cpp ----")
    print(f"static constexpr int kTileZ      = {args.zoom};")
    print(f"static constexpr int kGridCols   = {args.cols};")
    print(f"static constexpr int kGridRows   = {args.rows};")
    print(f"static constexpr int kTilePx     = 256;")
    print(f"static const int     kTileX[kGridCols] = {{ "
          + ", ".join(str(x) for x in xs) + " }};")
    print(f"static const int     kTileY[kGridRows] = {{ "
          + ", ".join(str(y) for y in ys) + " }};")
    print("-----------------------------------------------------------------------")
    print()
    print(f"Copy the 'tiles/' directory to the SD card root so files resolve as:")
    print(f"    /sdcard/tiles/{args.zoom}/{cx}/{cy}.png  (and {n-1} others)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
