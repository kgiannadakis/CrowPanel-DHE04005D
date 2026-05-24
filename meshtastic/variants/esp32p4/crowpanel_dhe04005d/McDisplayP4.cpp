#if HAS_TFT && USE_MCUI

#include "CrowPanelP4Display.h"
#include "graphics/mcui/McDisplay.h"
#include "graphics/mcui/McUI.h"
#include "configuration.h"

#include <esp_log.h>
#include <lvgl.h>

namespace mcui {

void display_init()
{
    LOG_INFO("mcui: display_init (P4 backend)");

    if (!crowpanel_p4::display_init()) {
        LOG_ERROR("mcui: P4 display init failed — UI will be invisible");
        return;
    }

    lv_display_t* disp = crowpanel_p4::lv_display();
    if (disp) {
        lv_timer_t* refr = lv_display_get_refr_timer(disp);
        if (refr) lv_timer_set_period(refr, 20);
    }

    LOG_INFO("mcui: display_init complete (logical %dx%d)", SCR_W, SCR_H);
}

}

#endif
