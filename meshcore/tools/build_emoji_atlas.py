#!/usr/bin/env python3
"""
build_emoji_atlas.py — generate emoji_atlas_{20,32}.bin from Noto Color Emoji

Reads a Noto Color Emoji TTF, renders ~250 curated emoji codepoints at two
sizes (20px for body text, 32px for keyboard/headers), and writes two binary
atlases in the format the firmware's emoji_atlas.cpp expects.

File format (little-endian):

    header (16 bytes):
        magic[4]    = b"EMA1"
        version     uint16 (= 1)
        glyph_px    uint16 (20 or 32)
        glyph_count uint32
        reserved    uint32 (= 0)

    codepoints:  glyph_count × uint32 (sorted ascending for bsearch)
    pixel data:  glyph_count × glyph_px × glyph_px × 3 bytes
                 (RGB565 lo, RGB565 hi, alpha)  — matches
                 LV_IMG_CF_TRUE_COLOR_ALPHA with LV_COLOR_DEPTH=16 and
                 LV_COLOR_16_SWAP=0.

Usage:
    python build_emoji_atlas.py --font /path/to/NotoColorEmoji.ttf
    # Output: emoji_atlas_20.bin, emoji_atlas_32.bin in CWD.
    # Copy those to /sdcard/emoji/ on the SD card.

Requires Pillow >= 10.0 (we use `embedded_color=True` for color glyph rendering).

Where to get Noto Color Emoji:
    https://github.com/googlefonts/noto-emoji/raw/main/fonts/NotoColorEmoji.ttf
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path
from typing import Iterable

from PIL import Image, ImageDraw, ImageFont


# ---------------------------------------------------------------------------
# Codepoint set. Built from two parts:
#
#   1. KEYBOARD_CODEPOINTS — every emoji the on-screen keyboard can type,
#      plus the page-nav arrows. These MUST be in the atlas; any missing
#      one renders as an empty box on the keyboard.
#
#   2. ADDITIONAL_RANGES — Unicode emoji blocks we sweep wholesale.
#      render_codepoint() returns an empty image for codepoints not in
#      Noto Color Emoji; pack_atlas() now skips those, so the final
#      atlas only contains codepoints that actually render. Net result
#      is ~1500-1800 emojis covering everyday messaging down to obscure
#      Unicode 15 additions.
#
# You can grow or shrink the additional ranges below without worrying
# about "holes" (reserved codepoints): empty glyphs are filtered out.
# ---------------------------------------------------------------------------

KEYBOARD_CODEPOINTS: list[int] = [
    # --- Keyboard page 1 & 2 (MUST be in the atlas — these are the
    # emojis our on-screen keyboard lets the user type). Keep this
    # block in sync with kb_emoji_1[] / kb_emoji_2[] in keyboard_helpers.cpp.
    # If we omit any, the keyboard renders empty boxes for them. ---
    # kb_emoji_1 row 1 (grinning faces)
    0x1F600, 0x1F603, 0x1F604, 0x1F601, 0x1F606, 0x1F605, 0x1F602, 0x1F923,
    0x1F60A, 0x1F607,
    # kb_emoji_1 row 2 (heart-eyes / kiss / tongue / zany)
    0x1F60D, 0x1F970, 0x1F618, 0x1F61A, 0x1F60B, 0x1F61B, 0x1F61C, 0x1F92A,
    0x1F61D, 0x1F911,
    # kb_emoji_1 row 3 (hug / think / shades / star-struck / smirk / cry)
    0x1F917, 0x1F914, 0x1F60E, 0x1F929, 0x1F60F, 0x1F612, 0x1F61E, 0x1F622,
    0x1F62D, 0x1F624,
    # kb_emoji_1 row 4 (rage / scared / yawn / pile-of-poo / skull / ghost)
    0x1F621, 0x1F92C, 0x1F631, 0x1F630, 0x1F97A, 0x1F634, 0x1F4A9, 0x1F480,
    0x1F47B, 0x1F47D,
    # kb_emoji_2 row 1 (hand gestures)
    0x1F44D, 0x1F44E, 0x1F44F, 0x1F64C, 0x1F44A, 0x270A,  0x270C,  0x1F91E,
    0x270B,  0x1F64F,
    # kb_emoji_2 row 2 (hearts + fire + 100)
    0x2764,  0x1F9E1, 0x1F49B, 0x1F49A, 0x1F499, 0x1F49C, 0x1F5A4, 0x1F494,
    0x1F525, 0x1F4AF,
    # kb_emoji_2 row 3 (check / sparkle / star / celebration / objects)
    0x2705,  0x2728,  0x2B50,  0x1F31F, 0x1F389, 0x1F381, 0x1F3C6, 0x1F4B0,
    0x1F4F1, 0x1F4BB,
    # kb_emoji_2 row 4 (beer / food / weather / transport / church)
    0x1F37B, 0x1F37A, 0x1F37D, 0x1F37E, 0x2600,  0x26C5,  0x26C4,  0x26FD,
    0x26F5,  0x26EA,
    # Keyboard function row — page-navigation triangles. Montserrat
    # doesn't have these glyphs; if we don't include them they render
    # as empty placeholder boxes.
    0x25B6,  # ▶ next emoji page (on page 1's function row)
    0x25C0,  # ◀ prev emoji page (on page 2's function row)
]


# Unicode emoji blocks we sweep wholesale. render_codepoint() returns an
# empty image for anything Noto Color Emoji doesn't have, and pack_atlas()
# skips empty glyphs — so it's safe to ask for full ranges that contain
# reserved / unused codepoints.
#
# Coverage of these ranges is roughly:
#   1F300-1F5FF: Misc Symbols & Pictographs (weather, food, nature, 768)
#   1F600-1F64F: Emoticons (faces, 80)
#   1F680-1F6FF: Transport & Map (vehicles, signs, 128)
#   1F700-1F77F: Alchemical (mostly non-emoji, a few pass-through)
#   1F780-1F7FF: Geometric Shapes Extended (circles, triangles, a few emoji)
#   1F800-1F8FF: Supplemental Arrows-C (non-emoji mostly)
#   1F900-1F9FF: Supplemental Symbols & Pictographs (people, gestures, 256)
#   1FA70-1FAFF: Symbols and Pictographs Extended-A (newer emoji, 144)
#   1FB00-1FBFF: Symbols for Legacy Computing (non-emoji)
#   2600-26FF:   Misc Symbols (weather, zodiac, ~80 are emoji)
#   2700-27BF:   Dingbats (~30 are emoji)
#   2300-23FF:   Misc Technical (~15 clock/time emoji)
#   25A0-25FF:   Geometric Shapes (▶/◀ etc.)
#   2B00-2BFF:   Misc Symbols & Arrows (⭐ ✅ arrows)
#
# After render filtering, expect ~1500 live glyphs.
ADDITIONAL_RANGES: list[tuple[int, int]] = [
    (0x2300, 0x23FF),
    (0x25A0, 0x25FF),
    (0x2600, 0x26FF),
    (0x2700, 0x27BF),
    (0x2B00, 0x2BFF),
    (0x1F300, 0x1F5FF),
    (0x1F600, 0x1F64F),
    (0x1F680, 0x1F6FF),
    (0x1F900, 0x1F9FF),
    (0x1FA70, 0x1FAFF),
]


def build_codepoint_set() -> list[int]:
    cps: set[int] = set(KEYBOARD_CODEPOINTS)
    for lo, hi in ADDITIONAL_RANGES:
        for cp in range(lo, hi + 1):
            cps.add(cp)
    # Firmware binary-searches the codepoint table and expects strict
    # ascending order.
    return sorted(cps)


EMOJI_CODEPOINTS = build_codepoint_set()


# ---------------------------------------------------------------------------
# Core render + pack
# ---------------------------------------------------------------------------


def rgba_to_rgb565a(px: tuple[int, int, int, int]) -> bytes:
    """RGBA8888 -> 3 bytes (RGB565 lo, RGB565 hi, alpha).

    Matches LV_IMG_CF_TRUE_COLOR_ALPHA with LV_COLOR_DEPTH=16,
    LV_COLOR_16_SWAP=0. See lv_img_buf.c in LVGL 8.3.
    """
    r, g, b, a = px
    c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    return bytes([c & 0xFF, (c >> 8) & 0xFF, a])


def render_codepoint(font: ImageFont.FreeTypeFont, codepoint: int,
                     target_px: int, native_px: int = 109) -> Image.Image:
    """Render one codepoint to an RGBA `target_px × target_px` PIL image.

    Noto Color Emoji stores bitmap glyphs at a fixed native size
    (109 by default); the caller is expected to have loaded the font at
    that native size. We resize to `target_px` with LANCZOS. Returns a
    fully transparent image if the codepoint isn't in the font.
    """
    canvas_px = max(native_px + 8, target_px + 8)
    img = Image.new("RGBA", (canvas_px, canvas_px), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)

    text = chr(codepoint)
    try:
        draw.text((canvas_px // 2, canvas_px // 2), text, font=font,
                  embedded_color=True, anchor="mm")
    except Exception as e:
        sys.stderr.write(f"  warn: U+{codepoint:05X} render raised {e!r}\n")
        return Image.new("RGBA", (target_px, target_px), (0, 0, 0, 0))

    # Crop to the non-transparent bounding box so resize doesn't waste
    # pixels on surrounding padding. Empty glyphs (codepoint not in font)
    # are silently returned empty; pack_atlas() filters them out and
    # prints a summary count — no point spamming per-codepoint when we
    # sweep whole blocks.
    bbox = img.getbbox()
    if bbox is None:
        return Image.new("RGBA", (target_px, target_px), (0, 0, 0, 0))

    cropped = img.crop(bbox)

    # Aspect-preserving resize into a target_px square, centered.
    w, h = cropped.size
    scale = target_px / max(w, h)
    nw, nh = max(1, int(round(w * scale))), max(1, int(round(h * scale)))
    resized = cropped.resize((nw, nh), Image.LANCZOS)

    out = Image.new("RGBA", (target_px, target_px), (0, 0, 0, 0))
    out.paste(resized, ((target_px - nw) // 2, (target_px - nh) // 2), resized)
    return out


def pack_atlas(font: ImageFont.FreeTypeFont, codepoints: Iterable[int],
               glyph_px: int, out_path: Path) -> None:
    all_cps = sorted(set(codepoints))
    if not all_cps:
        raise ValueError("empty codepoint list")

    # First pass: render everything, drop anything the font doesn't cover.
    # This is what lets ADDITIONAL_RANGES sweep wholesale without the atlas
    # carrying kilobytes of blank glyphs for reserved codepoints.
    print(f"  rendering up to {len(all_cps)} glyphs at {glyph_px}x{glyph_px}...",
          end="", flush=True)
    rendered: list[tuple[int, Image.Image]] = []
    for i, cp in enumerate(all_cps):
        if i % 100 == 0 and i > 0:
            print(f" {i}", end="", flush=True)
        img = render_codepoint(font, cp, glyph_px)
        if img.getbbox() is None:
            continue   # codepoint not in font — skip silently
        rendered.append((cp, img))
    print(" done")

    dropped = len(all_cps) - len(rendered)
    print(f"  kept {len(rendered)}, skipped {dropped} (not in font)")

    # Rendered pairs are already sorted because we iterated all_cps in order.
    live_cps = [cp for cp, _ in rendered]

    # Header
    header = struct.pack("<4sHHII", b"EMA1", 1, glyph_px, len(live_cps), 0)

    # Codepoint table (uint32 little-endian, ascending)
    cp_table = struct.pack(f"<{len(live_cps)}I", *live_cps)

    # Pixel data
    pixel_chunks: list[bytes] = []
    for _cp, img in rendered:
        for px in img.getdata():
            pixel_chunks.append(rgba_to_rgb565a(px))

    pixel_data = b"".join(pixel_chunks)
    expected_bytes = len(live_cps) * glyph_px * glyph_px * 3
    assert len(pixel_data) == expected_bytes, (
        f"pixel buffer length mismatch: got {len(pixel_data)}, "
        f"expected {expected_bytes}")

    with out_path.open("wb") as f:
        f.write(header)
        f.write(cp_table)
        f.write(pixel_data)

    size_kb = out_path.stat().st_size / 1024
    print(f"  wrote {out_path} ({size_kb:.1f} KB)")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--font", required=True, type=Path,
                    help="Path to NotoColorEmoji.ttf")
    ap.add_argument("--out-dir", default=Path("."), type=Path,
                    help="Output directory (default: current)")
    ap.add_argument("--sizes", default="20,32",
                    help="Comma-separated glyph sizes to generate (default: 20,32)")
    args = ap.parse_args()

    if not args.font.is_file():
        sys.stderr.write(f"error: font file not found: {args.font}\n")
        return 2

    args.out_dir.mkdir(parents=True, exist_ok=True)

    sizes = [int(s) for s in args.sizes.split(",")]
    print(f"font: {args.font}")
    print(f"candidate codepoints: {len(EMOJI_CODEPOINTS)} "
          f"(keyboard + {len(ADDITIONAL_RANGES)} Unicode ranges)")
    print(f"sizes: {sizes}")
    print(f"output: {args.out_dir}")

    # Load the font once; render_codepoint() is called ~2×len(EMOJI_CODEPOINTS)
    # times so re-creating it per call would waste real wall-time.
    font = ImageFont.truetype(str(args.font), size=109)

    for sz in sizes:
        out = args.out_dir / f"emoji_atlas_{sz}.bin"
        print(f"\n[{sz}px]")
        pack_atlas(font, EMOJI_CODEPOINTS, sz, out)

    print("\nDone. Copy the .bin files to /sdcard/emoji/ on the SD card.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
