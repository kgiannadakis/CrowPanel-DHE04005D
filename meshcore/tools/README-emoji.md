# Color emoji atlas generator

`build_emoji_atlas.py` produces the two binary files the firmware loads from
the SD card for color emoji rendering:

- `emoji_atlas_20.bin` — 20×20 glyphs, used for 12/14 pt chat body text
- `emoji_atlas_32.bin` — 32×32 glyphs, used for 16/18/20 pt keyboard, headers

Both contain a curated set of ~250 common emojis from Noto Color Emoji.

## One-time setup

1. **Install Pillow** (you already have 12.1.1 — skip if so):
   ```
   python -m pip install Pillow
   ```

2. **Download Noto Color Emoji:**
   ```
   https://github.com/googlefonts/noto-emoji/raw/main/fonts/NotoColorEmoji.ttf
   ```
   Save it somewhere convenient, e.g. `C:\fonts\NotoColorEmoji.ttf`.

## Generate atlases

From the `meshcore/tools/` directory:

```
python build_emoji_atlas.py --font C:\fonts\NotoColorEmoji.ttf
```

This writes `emoji_atlas_20.bin` and `emoji_atlas_32.bin` to the current
directory. Expect ~800 KB and ~2 MB respectively.

Progress is logged:
```
font: C:\fonts\NotoColorEmoji.ttf
codepoints: 252 curated
sizes: [20, 32]
output: .

[20px]
  rendering 252 glyphs at 20x20... 50 100 150 200 250 done
  wrote emoji_atlas_20.bin (443.2 KB)

[32px]
  rendering 252 glyphs at 32x32... 50 100 150 200 250 done
  wrote emoji_atlas_32.bin (1134.5 KB)

Done. Copy the .bin files to /sdcard/emoji/ on the SD card.
```

## Copy to SD card

Create the directory `/emoji/` on the microSD root (if it doesn't already
exist) and copy the two `.bin` files there. Paths the firmware looks for:

```
/emoji/emoji_atlas_20.bin
/emoji/emoji_atlas_32.bin
```

Reinsert the card, reboot. The serial log should show:

```
SD mounted at /sdcard - 30436 MB
emoji: test atlas loaded (3 glyphs, 20x20)
emoji: test atlas loaded (3 glyphs, 20x20)
emoji: SD atlas loaded /sdcard/emoji/emoji_atlas_20.bin - 252 glyphs, 20x20
emoji: SD atlas loaded /sdcard/emoji/emoji_atlas_32.bin - 252 glyphs, 32x32
```

The SD atlas replaces the 3-glyph bundled test fallback.

## Changing the emoji set

Edit the `EMOJI_CODEPOINTS` list near the top of `build_emoji_atlas.py` to
add or remove emojis. Each entry is a Unicode codepoint in hex. The script
sorts + deduplicates before writing, so order doesn't matter and duplicates
are harmless.

After editing: rerun the script, copy the new `.bin` files to the SD card,
reboot. No firmware rebuild needed.

## File format (for reference)

Little-endian, matches the parser in
`examples/crowpanel_p4_lvgl_chat/emoji_atlas.cpp`:

```
header (16 bytes):
  magic[4]    = "EMA1"
  version     uint16 (= 1)
  glyph_px    uint16 (20 or 32)
  glyph_count uint32
  reserved    uint32 (= 0)

codepoints:  glyph_count × uint32 (sorted ascending)
pixel data:  glyph_count × glyph_px × glyph_px × 3 bytes
             (RGB565 lo, RGB565 hi, alpha)
```

The pixel format matches LVGL's `LV_IMG_CF_TRUE_COLOR_ALPHA` with
`LV_COLOR_DEPTH=16` and `LV_COLOR_16_SWAP=0`, so the firmware can
hand the byte buffer straight to `lv_draw_img` with no conversion.

## Troubleshooting

- **"warn: U+XXXXX empty glyph"**: codepoint isn't in Noto Color Emoji.
  Either the codepoint is too new (update the TTF), or it's a regional
  indicator pair / ZWJ sequence that isn't represented as a single
  Unicode character. Individual flag codepoints like 🇧 🇪 won't render
  as flags — flag-pair handling is a separate firmware feature (Phase
  2b).
- **Pillow says `embedded_color` unknown**: your Pillow is too old.
  Upgrade to ≥ 10.0.
- **Output files look too small / all-zero**: Pillow may not have the
  color bitmap support compiled in. Rare on Windows; reinstall with
  `python -m pip install --upgrade --force-reinstall Pillow`.
