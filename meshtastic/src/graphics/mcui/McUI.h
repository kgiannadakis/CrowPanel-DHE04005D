#pragma once

#if HAS_TFT && USE_MCUI

namespace mcui {

#if defined(CROWPANEL_DHE04005D)
constexpr int PORTRAIT_SCR_W  = 479;
constexpr int PORTRAIT_SCR_H  = 792;
constexpr int LANDSCAPE_SCR_W = 792;
constexpr int LANDSCAPE_SCR_H = 479;
#else
constexpr int PORTRAIT_SCR_W  = 480;
constexpr int PORTRAIT_SCR_H  = 800;
constexpr int LANDSCAPE_SCR_W = 800;
constexpr int LANDSCAPE_SCR_H = 480;
#endif
constexpr int STATUS_H = 40;
constexpr int TAB_H = 60;
constexpr int PAGE_Y = STATUS_H;

int screen_width();
int screen_height();
int page_height();
int keyboard_height();

bool landscape_active();
bool orientation_save(bool landscape);
bool position_advert_enabled();
bool position_advert_save(bool enabled);

enum Tab {
    TAB_CHATS = 0,
    TAB_NODES = 1,
    TAB_MAPS = 2,
    TAB_SETTINGS = 3,
};

void setup();

void switchTab(int idx);

}

#define SCR_W (::mcui::screen_width())
#define SCR_H (::mcui::screen_height())
#define PAGE_H (::mcui::page_height())

#endif
