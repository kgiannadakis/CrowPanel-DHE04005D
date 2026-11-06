#include "../../variants/crowpanel_dhe04005d/stc8_backlight_policy.h"

static_assert(Stc8Backlight::needsStart(-1, 5), "Prime low brightness at boot");
static_assert(Stc8Backlight::needsStart(0, 5), "Prime low brightness after off");
static_assert(Stc8Backlight::needsStart(-1, 80), "Retry unknown hardware state");
static_assert(!Stc8Backlight::needsStart(5, 10), "Do not flash during dimming");
static_assert(!Stc8Backlight::needsStart(0, 0), "Never pulse when sleeping");
static_assert(!Stc8Backlight::needsStart(0, 100), "Full brightness starts directly");
