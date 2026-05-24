#!/usr/bin/env python3
"""
Convert slippy-map PNG tiles to LVGL v8 built-in .bin image files.

Input layout (default):
  <root>/<z>/<x>/<y>.png

Output:
  <root>/<z>/<x>/<y>.bin

LVGL built-in .bin format (v8):
  - 4-byte lv_img_header_t (little-endian packed bitfields)
  - raw pixel data in LV_IMG_CF_TRUE_COLOR (RGB565), row-major

Header packing for little-endian systems:
  bits  0..4   : cf (LV_IMG_CF_TRUE_COLOR = 4)
  bits  5..7   : always_zero (0)
  bits  8..9   : reserved (0)
  bits 10..20  : width  (11 bits)
  bits 21..31  : height (11 bits)
"""

from __future__ import annotations

import argparse
import os
import struct
import sys
from pathlib import Path

try:
    from PIL import Image
except Exception:
    print("Pillow is required. Install with: pip install pillow", file=sys.stderr)
    raise


LV_IMG_CF_TRUE_COLOR = 4


def pack_lvgl_header_true_color(w: int, h: int) -> bytes:
    if w <= 0 or h <= 0 or w > 0x7FF or h > 0x7FF:
        raise ValueError(f"Invalid tile size for LVGL header: {w}x{h}")
    v = 0
    v |= (LV_IMG_CF_TRUE_COLOR & 0x1F)          # cf: 5 bits
    v |= ((w & 0x7FF) << 10)                    # width: 11 bits
    v |= ((h & 0x7FF) << 21)                    # height: 11 bits
    return struct.pack("<I", v)


def rgb888_to_rgb565_le(r: int, g: int, b: int) -> bytes:
    px = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    return struct.pack("<H", px)


def convert_png_to_lvgl_bin(src_png: Path, dst_bin: Path, overwrite: bool = False, delete_png: bool = False) -> bool:
    if dst_bin.exists() and not overwrite:
        # Skip if destination is newer or equal.
        if dst_bin.stat().st_mtime >= src_png.stat().st_mtime:
            return False

    with Image.open(src_png) as im:
        im = im.convert("RGB")
        w, h = im.size
        header = pack_lvgl_header_true_color(w, h)

        # Build raw RGB565 payload.
        # Use getdata() to avoid per-pixel getpixel calls.
        pixels = im.getdata()
        payload = bytearray(w * h * 2)
        o = 0
        for (r, g, b) in pixels:
            px = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            payload[o] = px & 0xFF
            payload[o + 1] = (px >> 8) & 0xFF
            o += 2

    dst_bin.parent.mkdir(parents=True, exist_ok=True)
    tmp = dst_bin.with_suffix(".bin.tmp")
    with tmp.open("wb") as f:
        f.write(header)
        f.write(payload)
    tmp.replace(dst_bin)
    if delete_png:
        try:
            src_png.unlink()
        except Exception:
            # Conversion succeeded; keep going even if delete fails.
            pass
    return True


def find_png_tiles(root: Path):
    # Strict slippy tree search: */*/*.png under root
    for p in root.rglob("*.png"):
        yield p


def main() -> int:
    ap = argparse.ArgumentParser(description="Convert PNG map tiles to LVGL .bin (RGB565 true-color).")
    ap.add_argument("root", type=Path, help="Tile root folder (contains z/x/y.png)")
    ap.add_argument("--overwrite", action="store_true", help="Rewrite all .bin files regardless of mtime")
    ap.add_argument("--limit", type=int, default=0, help="Optional max tile count to convert")
    ap.add_argument("--keep-png", action="store_true", help="Keep source PNGs (default deletes PNG after successful conversion)")
    args = ap.parse_args()

    root = args.root
    if not root.exists():
        print(f"Root not found: {root}", file=sys.stderr)
        return 2

    converted = 0
    skipped = 0
    failed = 0

    for png in find_png_tiles(root):
        if args.limit and (converted + skipped + failed) >= args.limit:
            break
        try:
            bin_path = png.with_suffix(".bin")
            changed = convert_png_to_lvgl_bin(
                png,
                bin_path,
                overwrite=args.overwrite,
                delete_png=(not args.keep_png),
            )
            if changed:
                converted += 1
            else:
                skipped += 1
        except Exception as e:
            failed += 1
            print(f"[ERR] {png}: {e}", file=sys.stderr)

    print(f"Done. converted={converted} skipped={skipped} failed={failed}")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
