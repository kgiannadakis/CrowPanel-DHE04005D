#if HAS_TFT && USE_MCUI

#include "McEmojiFont.h"
#include "McEmojiAtlas.h"

#include <string.h>

lv_font_t *g_emoji_font_small = nullptr;
lv_font_t *g_emoji_font_big = nullptr;

namespace {

// LVGL 9.2 image-glyph contract: get_glyph_dsc reports the glyph as an image and
// stashes the lv_image_dsc_t* in gid.src; get_glyph_bitmap just returns it.
bool fill_dsc(lv_font_glyph_dsc_t *out, const lv_image_dsc_t *src)
{
    if (!src)
        return false;
    out->is_placeholder = 0;
    // Direct get_glyph_dsc callbacks report advance in pixels. LVGL's fmt_txt
    // fonts store 1/16 px internally, but convert before filling this output.
    out->adv_w = src->header.w;
    out->box_w = src->header.w;
    out->box_h = src->header.h;
    out->ofs_x = 0;
    out->ofs_y = 0; // bottom-aligned to text baseline; tune if emoji sits high/low
    out->format = LV_FONT_GLYPH_FORMAT_IMAGE;
    out->gid.src = src;
    return true;
}

bool small_get_glyph_dsc(const lv_font_t *, lv_font_glyph_dsc_t *out, uint32_t cp, uint32_t)
{
    return fill_dsc(out, emoji_atlas_lookup(cp, EMOJI_SIZE_SMALL));
}

bool big_get_glyph_dsc(const lv_font_t *, lv_font_glyph_dsc_t *out, uint32_t cp, uint32_t)
{
    return fill_dsc(out, emoji_atlas_lookup(cp, EMOJI_SIZE_BIG));
}

const void *emoji_get_glyph_bitmap(lv_font_glyph_dsc_t *g_dsc, lv_draw_buf_t *)
{
    return g_dsc->gid.src; // the lv_image_dsc_t set in fill_dsc
}

lv_font_t s_font_small = {};
lv_font_t s_font_big = {};

} // namespace

bool emoji_font_init(void)
{
    const uint16_t small_px = emoji_atlas_glyph_px(EMOJI_SIZE_SMALL);
    const uint16_t big_px = emoji_atlas_glyph_px(EMOJI_SIZE_BIG);

    if (small_px > 0 && !g_emoji_font_small) {
        memset(&s_font_small, 0, sizeof(s_font_small));
        s_font_small.get_glyph_dsc = small_get_glyph_dsc;
        s_font_small.get_glyph_bitmap = emoji_get_glyph_bitmap;
        s_font_small.subpx = LV_FONT_SUBPX_NONE;
        s_font_small.line_height = small_px;
        s_font_small.base_line = 0;
        g_emoji_font_small = &s_font_small;
    }

    if (big_px > 0 && !g_emoji_font_big) {
        memset(&s_font_big, 0, sizeof(s_font_big));
        s_font_big.get_glyph_dsc = big_get_glyph_dsc;
        s_font_big.get_glyph_bitmap = emoji_get_glyph_bitmap;
        s_font_big.subpx = LV_FONT_SUBPX_NONE;
        s_font_big.line_height = big_px;
        s_font_big.base_line = 0;
        g_emoji_font_big = &s_font_big;
    }

    return g_emoji_font_small != nullptr || g_emoji_font_big != nullptr;
}

#endif
