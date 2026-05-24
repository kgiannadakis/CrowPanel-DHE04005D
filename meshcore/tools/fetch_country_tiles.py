#!/usr/bin/env python3
"""
fetch_country_tiles.py — Bulk OSM tile downloader for offline SD card use.

Select one or more countries by ISO code; the script walks the
country's bounding box at every requested zoom level and downloads the
slippy-map tiles into a tiles/{z}/{x}/{y}.png tree ready to copy to the
device's SD card root.

Quick start:

    # List available countries
    python fetch_country_tiles.py --list

    # Download Belgium + Netherlands at zooms 8-13 (default range)
    python fetch_country_tiles.py --countries BE,NL

    # Narrower zoom range (smaller footprint, faster)
    python fetch_country_tiles.py --countries DE --zoom-min 10 --zoom-max 12

    # Estimate tile count and disk size WITHOUT downloading
    python fetch_country_tiles.py --countries BE,NL --dry-run

Idempotent: re-runs skip tiles already on disk. Throttled to 1 req/s
with a descriptive User-Agent.

IMPORTANT — OSM tile usage policy
---------------------------------
Bulk downloads from tile.openstreetmap.org violate the OSM Foundation's
Tile Usage Policy (https://operations.osmfoundation.org/policies/tiles/).
Small country requests (one or two tens of thousands of tiles) at
1 req/s are tolerated in practice but are not compliant. For
country-scale or continent-scale coverage, run your own tileserver-gl
from an OSM extract (Geofabrik has downloads per country) and point
this script at it with `--tile-server http://localhost:8080/styles/osm/256`.

The script prints a warning and asks for confirmation before starting
any download larger than --confirm-threshold tiles (default 500).
"""

from __future__ import annotations

import argparse
import math
import sys
import time
from pathlib import Path
from urllib.request import Request, urlopen
from urllib.error import URLError


USER_AGENT = "meshcore-crowpanel-dhe04005d/Phase2 (country-tile-bulk)"
DEFAULT_TILE_SERVER = "https://tile.openstreetmap.org"
REQUEST_DELAY_S = 1.0
AVG_TILE_BYTES = 15_000   # rough mean for OSM-style 256 px PNG


# ---------------------------------------------------------------------
# Country bounding boxes.
# Values are (west_lon, south_lat, east_lon, north_lat) in degrees.
# Sources: Natural Earth + Wikipedia; rounded to the nearest ~0.1°.
# Bboxes are intentionally slightly loose so coastal areas aren't
# clipped. Extend the dict below for new countries — the rest of the
# script is data-driven.
# ---------------------------------------------------------------------
COUNTRIES = {
    # Western Europe — priority region for MeshCore Benelux users
    "BE": ("Belgium",           ( 2.54, 49.50,   6.41, 51.51)),
    "NL": ("Netherlands",       ( 3.36, 50.75,   7.23, 53.55)),
    "LU": ("Luxembourg",        ( 5.74, 49.45,   6.53, 50.18)),
    "DE": ("Germany",           ( 5.87, 47.27,  15.04, 55.06)),
    "FR": ("France",            (-5.14, 41.33,   9.56, 51.09)),
    "UK": ("United Kingdom",    (-7.57, 49.86,   1.76, 59.36)),
    "IE": ("Ireland",           (-10.48,51.42,  -5.98, 55.39)),
    "CH": ("Switzerland",       ( 5.96, 45.82,  10.49, 47.81)),
    "AT": ("Austria",           ( 9.53, 46.37,  17.16, 49.02)),
    # Southern Europe
    "ES": ("Spain",             (-9.30, 35.95,   4.33, 43.79)),
    "PT": ("Portugal",          (-9.50, 36.95,  -6.19, 42.15)),
    "IT": ("Italy",             ( 6.63, 36.65,  18.51, 47.10)),
    "GR": ("Greece",            (19.37, 34.80,  28.25, 41.75)),
    # Nordic
    "DK": ("Denmark",           ( 8.09, 54.56,  12.69, 57.75)),
    "SE": ("Sweden",            (11.11, 55.34,  24.17, 69.06)),
    "NO": ("Norway",            ( 4.99, 57.97,  31.29, 71.19)),
    "FI": ("Finland",           (20.55, 59.81,  31.59, 70.09)),
    "IS": ("Iceland",           (-24.55,63.39, -13.50, 66.57)),
    # Central / Eastern Europe
    "PL": ("Poland",            (14.12, 49.00,  24.15, 54.84)),
    "CZ": ("Czechia",           (12.09, 48.55,  18.86, 51.06)),
    "SK": ("Slovakia",          (16.83, 47.73,  22.57, 49.61)),
    "HU": ("Hungary",           (16.11, 45.74,  22.90, 48.58)),
    "RO": ("Romania",           (20.26, 43.62,  29.69, 48.27)),
    "BG": ("Bulgaria",          (22.36, 41.23,  28.61, 44.21)),
    "HR": ("Croatia",           (13.49, 42.40,  19.45, 46.55)),
    "SI": ("Slovenia",          (13.38, 45.42,  16.61, 46.88)),
    "RS": ("Serbia",            (18.83, 42.23,  23.01, 46.18)),
    "EE": ("Estonia",           (21.77, 57.52,  28.21, 59.68)),
    "LV": ("Latvia",            (20.97, 55.67,  28.24, 58.08)),
    "LT": ("Lithuania",         (20.95, 53.89,  26.83, 56.45)),
    # North America
    "US": ("United States",     (-125.0,24.4,   -66.9, 49.4)),
    "CA": ("Canada",            (-141.0,41.7,   -52.6, 83.2)),
    "MX": ("Mexico",            (-118.4,14.5,   -86.7, 32.7)),
    # Oceania
    "AU": ("Australia",         (112.9,-43.7,  153.6,-10.7)),
    "NZ": ("New Zealand",       (166.4,-47.3,  178.6,-34.4)),
    # Asia — a small curated set, extend as needed
    "JP": ("Japan",             (122.9, 24.0,  145.8, 45.5)),
    "KR": ("South Korea",       (125.0, 33.1,  129.6, 38.6)),
}


def latlon_to_tile(lat_deg: float, lon_deg: float, zoom: int) -> tuple[int, int]:
    """Standard slippy-map projection (see OSM wiki: Slippy_map_tilenames)."""
    n = 2 ** zoom
    x = int((lon_deg + 180.0) / 360.0 * n)
    lat_rad = math.radians(lat_deg)
    y = int((1.0 - math.asinh(math.tan(lat_rad)) / math.pi) / 2.0 * n)
    x = max(0, min(x, n - 1))
    y = max(0, min(y, n - 1))
    return x, y


def tile_range_for_bbox(bbox: tuple[float, float, float, float], zoom: int
                        ) -> tuple[int, int, int, int]:
    """(x_min, y_min, x_max, y_max) inclusive covering the bbox at zoom."""
    west, south, east, north = bbox
    x_min, y_max = latlon_to_tile(south, west, zoom)   # south-west corner
    x_max, y_min = latlon_to_tile(north, east, zoom)   # north-east corner
    return x_min, y_min, x_max, y_max


def count_tiles(bbox: tuple[float, float, float, float], zooms: list[int]
                ) -> dict[int, int]:
    """Return {zoom: tile_count} for this bbox across the requested zooms."""
    result: dict[int, int] = {}
    for z in zooms:
        x_min, y_min, x_max, y_max = tile_range_for_bbox(bbox, z)
        result[z] = (x_max - x_min + 1) * (y_max - y_min + 1)
    return result


def fetch(url: str) -> bytes:
    req = Request(url, headers={"User-Agent": USER_AGENT})
    with urlopen(req, timeout=20) as resp:
        return resp.read()


def format_size(n_bytes: int) -> str:
    for unit in ("B", "KB", "MB", "GB"):
        if n_bytes < 1024:
            return f"{n_bytes:.1f} {unit}"
        n_bytes /= 1024
    return f"{n_bytes:.1f} TB"


def print_country_list() -> None:
    print("Available countries (ISO 3166-1 alpha-2 code - Name - bbox):")
    for code in sorted(COUNTRIES):
        name, bbox = COUNTRIES[code]
        w, s, e, n = bbox
        print(f"  {code}  {name:<20}  W={w:>7.2f} S={s:>6.2f} "
              f"E={e:>7.2f} N={n:>6.2f}")
    print()
    print(f"{len(COUNTRIES)} total. Pass with --countries XX or XX,YY,ZZ.")


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__.split("\n\n")[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="See the module docstring for the full OSM tile-usage notice.")
    ap.add_argument("--list", action="store_true",
                    help="Print the country preset list and exit")
    ap.add_argument("--countries", default="",
                    help="Comma-separated ISO codes, e.g. BE,NL,DE")
    ap.add_argument("--zoom-min", type=int, default=8,
                    help="Minimum zoom level (default 8 — regional overview)")
    ap.add_argument("--zoom-max", type=int, default=13,
                    help="Maximum zoom level (default 13 — neighborhood detail)")
    ap.add_argument("--out-dir", default=Path("."), type=Path,
                    help="Output directory (default: current)")
    ap.add_argument("--tile-server", default=DEFAULT_TILE_SERVER,
                    help=f"Tile server base URL (default {DEFAULT_TILE_SERVER}). "
                         "Use a self-hosted tileserver-gl for bulk pulls.")
    ap.add_argument("--delay", type=float, default=REQUEST_DELAY_S,
                    help=f"Seconds between requests (default {REQUEST_DELAY_S})")
    ap.add_argument("--dry-run", action="store_true",
                    help="Print tile count + size estimate, don't download")
    ap.add_argument("--confirm-threshold", type=int, default=500,
                    help="Ask for confirmation if total tiles exceed this "
                         "(default 500). Set to 0 to always ask; "
                         "to -1 to never ask.")
    ap.add_argument("--yes", action="store_true",
                    help="Skip the confirmation prompt (for scripting)")
    args = ap.parse_args()

    if args.list:
        print_country_list()
        return 0

    if not args.countries:
        ap.error("--countries required (or use --list to see options)")

    if args.zoom_min < 0 or args.zoom_max > 19 or args.zoom_min > args.zoom_max:
        ap.error(f"invalid zoom range {args.zoom_min}..{args.zoom_max} "
                 "(must satisfy 0 <= zoom_min <= zoom_max <= 19)")

    codes = [c.strip().upper() for c in args.countries.split(",") if c.strip()]
    unknown = [c for c in codes if c not in COUNTRIES]
    if unknown:
        ap.error(f"unknown country code(s): {', '.join(unknown)}. "
                 "Use --list to see available codes.")

    zooms = list(range(args.zoom_min, args.zoom_max + 1))

    # --- Plan ---------------------------------------------------------
    # Build the full (code, z, x, y) list up-front so we can print the
    # estimate and check for existing files without hitting the network.
    plan: list[tuple[str, int, int, int]] = []
    per_country_counts: dict[str, int] = {}
    for code in codes:
        name, bbox = COUNTRIES[code]
        counts = count_tiles(bbox, zooms)
        country_total = sum(counts.values())
        per_country_counts[code] = country_total
        for z in zooms:
            x_min, y_min, x_max, y_max = tile_range_for_bbox(bbox, z)
            for y in range(y_min, y_max + 1):
                for x in range(x_min, x_max + 1):
                    plan.append((code, z, x, y))
        # Friendly per-country breakdown
        counts_str = "  ".join(f"z{z}={counts[z]}" for z in zooms)
        print(f"{code} {name}: {country_total} tiles  ({counts_str})")

    # De-duplicate across countries (neighbours share tiles at low zooms)
    unique_tiles = {(z, x, y) for (_, z, x, y) in plan}
    total = len(unique_tiles)
    est_bytes = total * AVG_TILE_BYTES
    print()
    print(f"Total unique tiles across selected countries: {total}")
    print(f"Estimated disk usage:   {format_size(est_bytes)}  "
          f"(~{AVG_TILE_BYTES // 1024} KB/tile)")
    print(f"Estimated wall-clock time at {args.delay} s/req: "
          f"{total * args.delay / 60:.1f} min")

    if args.dry_run:
        print()
        print("--dry-run: not downloading. Remove the flag to proceed.")
        return 0

    # --- Policy warning + confirmation ---------------------------------
    if args.tile_server.startswith(DEFAULT_TILE_SERVER):
        print()
        print("WARNING: downloading from tile.openstreetmap.org in bulk")
        print("         violates the OSM tile-usage policy. For country-scale")
        print("         downloads, run your own tileserver-gl and pass")
        print("         --tile-server http://localhost:8080/...")
        print("         Proceeding will throttle to 1 req/s (polite) but you")
        print("         should still expect to be rate-limited or blocked.")

    threshold = args.confirm_threshold
    needs_prompt = (threshold == 0) or (threshold > 0 and total > threshold)
    if needs_prompt and not args.yes:
        resp = input(f"Continue and download {total} tiles? [y/N] ").strip().lower()
        if resp not in ("y", "yes"):
            print("Aborted.")
            return 1

    # --- Download pass ------------------------------------------------
    # Walk unique (z,x,y) — skipping duplicate tiles that appeared in
    # multiple countries. Existing files on disk are skipped silently.
    tile_server = args.tile_server.rstrip("/")
    downloaded = 0
    skipped = 0
    failures: list[tuple[int, int, int, str]] = []
    t0 = time.monotonic()

    ordered = sorted(unique_tiles)      # ascending z then x then y
    n = len(ordered)
    for i, (z, x, y) in enumerate(ordered):
        out = args.out_dir / "tiles" / str(z) / str(x) / f"{y}.png"
        if out.is_file() and out.stat().st_size > 0:
            skipped += 1
            continue

        out.parent.mkdir(parents=True, exist_ok=True)
        url = f"{tile_server}/{z}/{x}/{y}.png"
        try:
            data = fetch(url)
        except URLError as e:
            sys.stderr.write(f"[{i+1}/{n}] FAIL {url}  -- {e}\n")
            failures.append((z, x, y, str(e)))
            time.sleep(args.delay)
            continue

        out.write_bytes(data)
        downloaded += 1
        # Periodic progress line — every 20 tiles so we don't spam
        if downloaded % 20 == 0 or downloaded == 1:
            elapsed = time.monotonic() - t0
            rate = downloaded / elapsed if elapsed > 0 else 0.0
            remaining = (n - i - 1)
            eta_min = remaining * args.delay / 60 if args.delay > 0 else 0
            print(f"  [{i+1}/{n}]  saved {z}/{x}/{y}.png  "
                  f"dl={downloaded} skip={skipped} fail={len(failures)}  "
                  f"rate={rate:.2f}/s  ETA ~{eta_min:.1f} min")
        time.sleep(args.delay)

    print()
    print(f"Done. downloaded={downloaded}  already-had={skipped}  "
          f"failed={len(failures)}")
    if failures:
        print(f"  {len(failures)} tile(s) failed. Re-run to retry just these:")
        for z, x, y, err in failures[:10]:
            print(f"    {z}/{x}/{y}.png  -- {err}")
        if len(failures) > 10:
            print(f"    ...and {len(failures) - 10} more")

    print()
    print(f"Copy '{args.out_dir / 'tiles'}/' to the SD card root so paths resolve as:")
    print(f"    /sdcard/tiles/{{z}}/{{x}}/{{y}}.png")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
