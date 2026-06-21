#if HAS_TFT && USE_MCUI

#include "McFonts.h"
#include "McEmojiFont.h"

LV_FONT_DECLARE(mcui_font_greek_16)
LV_FONT_DECLARE(mcui_font_latin_de_16)

namespace mcui {

// Chat font fallback chain (primary -> fallback -> ...):
//   Montserrat 16 -> Greek 16 -> German Latin 16 [-> color emoji]
// The Greek/German faces are const in flash, so we keep writable copies here to
// extend the .fallback chain past them. The emoji link (g_emoji_font_small from
// McEmojiFont) is appended at the German tail once the SD atlas has loaded.
const lv_font_t *font_16()
{
    static lv_font_t font;   // Montserrat (primary)
    static lv_font_t greek;  // writable copy of mcui_font_greek_16
    static lv_font_t german; // writable copy of mcui_font_latin_de_16
    static bool initialized = false;
    if (!initialized) {
        german = mcui_font_latin_de_16;
        // Color-emoji tail. g_emoji_font_small is non-null only once the SD atlas
        // loaded in initVariant; otherwise the chain just ends at German.
        german.fallback = g_emoji_font_small;

        greek = mcui_font_greek_16;
        greek.fallback = &german;

        font = lv_font_montserrat_16;
        font.fallback = &greek;
        initialized = true;
    }
    return &font;
}

}

#endif
