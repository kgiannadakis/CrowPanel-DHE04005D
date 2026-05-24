#pragma once

#if HAS_TFT && USE_MCUI

#define TH_BG           0x0E1621
#define TH_SURFACE      0x17212B
#define TH_SURFACE2     0x1E2C3A
#define TH_INPUT        0x242F3D
#define TH_BORDER       0x2B3B4D
#define TH_SEPARATOR    0x1C2A36

#define TH_ACCENT       0x3390EC
#define TH_ACCENT_LIGHT 0x5EB5F7
#define TH_GREEN        0x6DC264
#define TH_RED          0xE05555
#define TH_AMBER        0xF5A623

#define TH_TEXT         0xF5F5F5
#define TH_TEXT2        0x8696A0
#define TH_TEXT3        0x6C7883

#define TH_BUBBLE_OUT   0x2B5278
#define TH_BUBBLE_IN    0x182533

#define TH_TAB_BG       0x0E1621
#define TH_TAB_ACTIVE   0x3390EC
#define TH_TAB_INACTIVE 0x546E7A

#define TH_BADGE_BG     0xE05555

#define TH_AVATAR_PALETTE_COUNT 12
#define TH_AVATAR_PALETTE_LIST  \
    0x3390EC,  \
    0x4CAF50,                   \
    0xAB47BC,                  \
    0xFF7043,             \
    0x26A69A,                    \
    0xEC407A,                    \
    0xFFB300,                   \
    0x5C6BC0,                  \
    0x29B6F6,                    \
    0xEF5350,                     \
    0x9CCC65,                    \
    0x8D6E63

#endif
