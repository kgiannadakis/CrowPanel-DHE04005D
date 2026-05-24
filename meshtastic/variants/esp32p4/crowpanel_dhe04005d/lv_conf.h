#if 1

#ifndef LV_CONF_H
#define LV_CONF_H

#if  0 && defined(__ASSEMBLY__)
#include "my_include.h"
#endif

#ifndef LV_COLOR_DEPTH
#define LV_COLOR_DEPTH 16
#endif

#define LV_USE_STDLIB_MALLOC    LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING    LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_BUILTIN

#define LV_STDINT_INCLUDE       <stdint.h>
#define LV_STDDEF_INCLUDE       <stddef.h>
#define LV_STDBOOL_INCLUDE      <stdbool.h>
#define LV_INTTYPES_INCLUDE     <inttypes.h>
#define LV_LIMITS_INCLUDE       <limits.h>
#define LV_STDARG_INCLUDE       <stdarg.h>

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_BUILTIN
#ifndef RAM_SIZE
#define RAM_SIZE 96
#endif

    #define LV_MEM_SIZE (RAM_SIZE * 1024U)

    #define LV_MEM_POOL_EXPAND_SIZE 0

    #define LV_MEM_ADR 0

    #if LV_MEM_ADR == 0
        #if defined BOARD_HAS_PSRAM
          #define LV_MEM_POOL_INCLUDE     <esp_heap_caps.h>
          #define LV_MEM_POOL_ALLOC(size) heap_caps_aligned_alloc(32, size, MALLOC_CAP_SPIRAM)
        #else
          #undef LV_MEM_POOL_INCLUDE
          #undef LV_MEM_POOL_ALLOC
        #endif
    #endif
#endif

#define LV_DEF_REFR_PERIOD  20

#define LV_DPI_DEF 140

#define LV_USE_OS   LV_OS_FREERTOS

#if LV_USE_OS == LV_OS_CUSTOM
    #define LV_OS_CUSTOM_INCLUDE <stdint.h>
#endif
#if LV_USE_OS == LV_OS_FREERTOS

    #define LV_USE_FREERTOS_TASK_NOTIFY 1
#endif

#define LV_DRAW_BUF_STRIDE_ALIGN                1

#define LV_DRAW_BUF_ALIGN                       4

#define LV_DRAW_TRANSFORM_USE_MATRIX            0

#define LV_DRAW_LAYER_SIMPLE_BUF_SIZE    (12 * 1024)

#define LV_DRAW_THREAD_STACK_SIZE    (8 * 1024)

#define LV_USE_DRAW_SW 1
#if LV_USE_DRAW_SW == 1

    #define LV_DRAW_SW_SUPPORT_RGB565        1
    #define LV_DRAW_SW_SUPPORT_RGB565A8      0
    #define LV_DRAW_SW_SUPPORT_RGB888        0
    #define LV_DRAW_SW_SUPPORT_XRGB8888      0
    // Required for PNG map tiles: LVGL's lodepng decoder outputs ARGB8888 draw
    // buffers. If disabled, tiles can be "loaded" but won't render on RGB565.
    #define LV_DRAW_SW_SUPPORT_ARGB8888      1
    #define LV_DRAW_SW_SUPPORT_L8            0
    #define LV_DRAW_SW_SUPPORT_AL88          0
    #define LV_DRAW_SW_SUPPORT_A8            1
    #define LV_DRAW_SW_SUPPORT_I1            0

    #define LV_DRAW_SW_DRAW_UNIT_CNT    1

    #define LV_USE_DRAW_ARM2D_SYNC      0

    #define LV_USE_NATIVE_HELIUM_ASM    0

    #define LV_DRAW_SW_COMPLEX          1

    #if LV_DRAW_SW_COMPLEX == 1
        #define LV_DRAW_SW_SHADOW_CACHE_SIZE 0
        #define LV_DRAW_SW_CIRCLE_CACHE_SIZE 2
    #endif

    #define  LV_USE_DRAW_SW_ASM     LV_DRAW_SW_ASM_NONE

    #if LV_USE_DRAW_SW_ASM == LV_DRAW_SW_ASM_CUSTOM
        #define  LV_DRAW_SW_ASM_CUSTOM_INCLUDE ""
    #endif

    #define LV_USE_DRAW_SW_COMPLEX_GRADIENTS    0
#endif

#define LV_USE_DRAW_VGLITE 0

#if LV_USE_DRAW_VGLITE

    #define LV_USE_VGLITE_BLIT_SPLIT 0

    #if LV_USE_OS

        #define LV_USE_VGLITE_DRAW_THREAD 1

        #if LV_USE_VGLITE_DRAW_THREAD

            #define LV_USE_VGLITE_DRAW_ASYNC 1
        #endif
    #endif

    #define LV_USE_VGLITE_ASSERT 0
#endif

#define LV_USE_PXP 0

#if LV_USE_PXP

    #define LV_USE_DRAW_PXP 1

    #define LV_USE_ROTATE_PXP 0

    #if LV_USE_DRAW_PXP && LV_USE_OS

        #define LV_USE_PXP_DRAW_THREAD 1
    #endif

    #define LV_USE_PXP_ASSERT 0
#endif

#define LV_USE_DRAW_DAVE2D 0

#define LV_USE_DRAW_SDL 0

#define LV_USE_DRAW_VG_LITE 0

#if LV_USE_DRAW_VG_LITE

    #define LV_VG_LITE_USE_GPU_INIT 0

    #define LV_VG_LITE_USE_ASSERT 0

    #define LV_VG_LITE_FLUSH_MAX_COUNT 8

    #define LV_VG_LITE_USE_BOX_SHADOW 0

    #define LV_VG_LITE_GRAD_CACHE_CNT 32

    #define LV_VG_LITE_STROKE_CACHE_CNT 32

#endif

#ifndef LV_USE_LOG
#define LV_USE_LOG 0
#endif
#if LV_USE_LOG

    #define LV_LOG_LEVEL LV_LOG_LEVEL_ERROR

    #define LV_LOG_PRINTF 0

    #define LV_LOG_USE_TIMESTAMP 1

    #define LV_LOG_USE_FILE_LINE 1

    #define LV_LOG_TRACE_MEM        0
    #define LV_LOG_TRACE_TIMER      0
    #define LV_LOG_TRACE_INDEV      0
    #define LV_LOG_TRACE_DISP_REFR  0
    #define LV_LOG_TRACE_EVENT      0
    #define LV_LOG_TRACE_OBJ_CREATE 0
    #define LV_LOG_TRACE_LAYOUT     0
    #define LV_LOG_TRACE_ANIM       0
    #define LV_LOG_TRACE_CACHE      1

#endif

#define LV_USE_ASSERT_NULL          1
#define LV_USE_ASSERT_MALLOC        1
#define LV_USE_ASSERT_STYLE         0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ           0

#define LV_ASSERT_HANDLER_INCLUDE <stdint.h>
#define LV_ASSERT_HANDLER while(1);

#define LV_USE_REFR_DEBUG 0

#define LV_USE_LAYER_DEBUG 0

#define LV_USE_PARALLEL_DRAW_DEBUG 0

#define LV_ENABLE_GLOBAL_CUSTOM 0
#if LV_ENABLE_GLOBAL_CUSTOM

    #define LV_GLOBAL_CUSTOM_INCLUDE <stdint.h>
#endif

#ifndef LV_CACHE_DEF_SIZE
// Map tiles decode to 256 KB ARGB8888 buffers, served from the boot-time PSRAM
// arena (see PngDecodeArena.cpp, 12 MB). 6 MB caches ~24 decoded tiles — more
// than the visible portrait viewport — so tiles are not re-decoded on every
// pan, while leaving the arena room for a tile's decode scratch in flight.
// The old 128 KB budget rejected every tile (entry > budget), which made
// decoder_open() destroy the decoded buffer and return LV_RESULT_INVALID.
#define LV_CACHE_DEF_SIZE       (6U * 1024U * 1024U)
#endif

#define LV_IMAGE_HEADER_CACHE_DEF_CNT 8

#define LV_GRADIENT_MAX_STOPS   2

#define LV_COLOR_MIX_ROUND_OFS  0

#define LV_OBJ_STYLE_CACHE      0

#define LV_USE_OBJ_ID           0

#define LV_USE_OBJ_ID_BUILTIN   0

#define LV_USE_OBJ_PROPERTY 0

#define LV_USE_OBJ_PROPERTY_NAME 0

#define LV_USE_VG_LITE_THORVG  0

#if LV_USE_VG_LITE_THORVG

    #define LV_VG_LITE_THORVG_LVGL_BLEND_SUPPORT 0

    #define LV_VG_LITE_THORVG_YUV_SUPPORT 0

    #define LV_VG_LITE_THORVG_LINEAR_GRADIENT_EXT_SUPPORT 0

    #define LV_VG_LITE_THORVG_16PIXELS_ALIGN 1

    #define LV_VG_LITE_THORVG_BUF_ADDR_ALIGN 64

    #define LV_VG_LITE_THORVG_THREAD_RENDER 0

#endif

#define LV_BIG_ENDIAN_SYSTEM 0

#define LV_ATTRIBUTE_TICK_INC

#define LV_ATTRIBUTE_TIMER_HANDLER

#define LV_ATTRIBUTE_FLUSH_READY

#define LV_ATTRIBUTE_MEM_ALIGN_SIZE 1

#define LV_ATTRIBUTE_MEM_ALIGN

#define LV_ATTRIBUTE_LARGE_CONST

#define LV_ATTRIBUTE_LARGE_RAM_ARRAY

#if defined(ARCH_ESP32)
    #define LV_ATTRIBUTE_FAST_MEM IRAM_ATTR
#endif

#define LV_EXPORT_CONST_INT(int_value) struct _silence_gcc_warning

#define LV_ATTRIBUTE_EXTERN_DATA

#define LV_USE_FLOAT            0

#define LV_USE_MATRIX           0

#define LV_USE_PRIVATE_API        0

#define LV_FONT_MONTSERRAT_8  0
#define LV_FONT_MONTSERRAT_10 1
#define LV_FONT_MONTSERRAT_12 0
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 0
#define LV_FONT_MONTSERRAT_20 0
#define LV_FONT_MONTSERRAT_22 0
#define LV_FONT_MONTSERRAT_24 0
#define LV_FONT_MONTSERRAT_26 0
#define LV_FONT_MONTSERRAT_28 0
#define LV_FONT_MONTSERRAT_30 0
#define LV_FONT_MONTSERRAT_32 0
#define LV_FONT_MONTSERRAT_34 0
#define LV_FONT_MONTSERRAT_36 0
#define LV_FONT_MONTSERRAT_38 0
#define LV_FONT_MONTSERRAT_40 0
#define LV_FONT_MONTSERRAT_42 0
#define LV_FONT_MONTSERRAT_44 0
#define LV_FONT_MONTSERRAT_46 0
#define LV_FONT_MONTSERRAT_48 0

#define LV_FONT_MONTSERRAT_28_COMPRESSED 0
#define LV_FONT_DEJAVU_16_PERSIAN_HEBREW 0
#define LV_FONT_SIMSUN_14_CJK            0
#define LV_FONT_SIMSUN_16_CJK            0

#define LV_FONT_UNSCII_8  0
#define LV_FONT_UNSCII_16 0

#define LV_FONT_DEFAULT &lv_font_montserrat_14

#define LV_FONT_FMT_TXT_LARGE 0

#define LV_USE_FONT_COMPRESSED 0

#define LV_USE_FONT_PLACEHOLDER 0

#define LV_TXT_ENC LV_TXT_ENC_UTF8

#define LV_TXT_BREAK_CHARS " ,.;:-_)]}"

#define LV_TXT_LINE_BREAK_LONG_LEN 0

#define LV_TXT_LINE_BREAK_LONG_PRE_MIN_LEN 3

#define LV_TXT_LINE_BREAK_LONG_POST_MIN_LEN 3

#define LV_USE_BIDI 0
#if LV_USE_BIDI

    #define LV_BIDI_BASE_DIR_DEF LV_BASE_DIR_AUTO
#endif

#define LV_USE_ARABIC_PERSIAN_CHARS 0

#define LV_WIDGETS_HAS_DEFAULT_VALUE  1

#define LV_USE_ANIMIMG    0

#define LV_USE_ARC        1

#define LV_USE_BAR        1

#define LV_USE_BUTTON        1

#define LV_USE_BUTTONMATRIX  1

#define LV_USE_CALENDAR   0
#if LV_USE_CALENDAR
    #define LV_CALENDAR_WEEK_STARTS_MONDAY 0
    #if LV_CALENDAR_WEEK_STARTS_MONDAY
        #define LV_CALENDAR_DEFAULT_DAY_NAMES {"Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"}
    #else
        #define LV_CALENDAR_DEFAULT_DAY_NAMES {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"}
    #endif

    #define LV_CALENDAR_DEFAULT_MONTH_NAMES {"January", "February", "March",  "April", "May",  "June", "July", "August", "September", "October", "November", "December"}
    #define LV_USE_CALENDAR_HEADER_ARROW 1
    #define LV_USE_CALENDAR_HEADER_DROPDOWN 1
    #define LV_USE_CALENDAR_CHINESE 0
#endif

#define LV_USE_CANVAS     0

#define LV_USE_CHART      0

#define LV_USE_CHECKBOX   1

#define LV_USE_DROPDOWN   1

#define LV_USE_IMAGE      1

#define LV_USE_IMAGEBUTTON     1

#define LV_USE_KEYBOARD   1

#define LV_USE_LABEL      1
#if LV_USE_LABEL
    #define LV_LABEL_TEXT_SELECTION 1
    #define LV_LABEL_LONG_TXT_HINT 1
    #define LV_LABEL_WAIT_CHAR_COUNT 3
#endif

#define LV_USE_LED        0

#define LV_USE_LINE       1

#define LV_USE_LIST       1

#define LV_USE_LOTTIE     0

#define LV_USE_MENU       0

#define LV_USE_MSGBOX     0

#define LV_USE_ROLLER     0

#define LV_USE_SCALE      0

#define LV_USE_SLIDER     1

#define LV_USE_SPAN       0
#if LV_USE_SPAN

    #define LV_SPAN_SNIPPET_STACK_SIZE 64
#endif

#define LV_USE_SPINBOX    0

#define LV_USE_SPINNER    0

#define LV_USE_SWITCH     1

#define LV_USE_TEXTAREA   1
#if LV_USE_TEXTAREA != 0
    #define LV_TEXTAREA_DEF_PWD_SHOW_TIME 1500
#endif

#define LV_USE_TABLE      1

#define LV_USE_TABVIEW    1

#define LV_USE_TILEVIEW   0

#define LV_USE_WIN        0

#define LV_USE_THEME_DEFAULT 1
#if LV_USE_THEME_DEFAULT

    #define LV_THEME_DEFAULT_DARK 1

    #define LV_THEME_DEFAULT_GROW 0

    #define LV_THEME_DEFAULT_TRANSITION_TIME 0
#endif

#define LV_USE_THEME_SIMPLE 0

#define LV_USE_THEME_MONO 0

#define LV_USE_FLEX 1

#define LV_USE_GRID 1

#define LV_FS_DEFAULT_DRIVER_LETTER 'S'

#define LV_USE_FS_STDIO 0
#if LV_USE_FS_STDIO
    #define LV_FS_STDIO_LETTER '\0'
    #define LV_FS_STDIO_PATH ""
    #define LV_FS_STDIO_CACHE_SIZE 0
#endif

#define LV_USE_FS_POSIX 1
#if LV_USE_FS_POSIX
    #define LV_FS_POSIX_LETTER 'S'
    #define LV_FS_POSIX_PATH "/sdcard"
    #define LV_FS_POSIX_CACHE_SIZE 4096
#endif

#define LV_USE_FS_WIN32 0
#if LV_USE_FS_WIN32
    #define LV_FS_WIN32_LETTER '\0'
    #define LV_FS_WIN32_PATH ""
    #define LV_FS_WIN32_CACHE_SIZE 0
#endif

#define LV_USE_FS_FATFS 0
#if LV_USE_FS_FATFS
    #define LV_FS_FATFS_LETTER '\0'
    #define LV_FS_FATFS_CACHE_SIZE 0
#endif

#define LV_USE_FS_MEMFS 0
#if LV_USE_FS_MEMFS
    #define LV_FS_MEMFS_LETTER '\0'
#endif

#define LV_USE_FS_LITTLEFS 0
#if LV_USE_FS_LITTLEFS
    #define LV_FS_LITTLEFS_LETTER '\0'
#endif

#define LV_USE_FS_ARDUINO_ESP_LITTLEFS 0
#if LV_USE_FS_ARDUINO_ESP_LITTLEFS
    #define LV_FS_ARDUINO_ESP_LITTLEFS_LETTER 'F'
#endif

#ifndef LV_USE_FS_ARDUINO_SD
#define LV_USE_FS_ARDUINO_SD 0
#endif
#if LV_USE_FS_ARDUINO_SD
    #define LV_FS_ARDUINO_SD_LETTER 'S'
#endif

#ifndef LV_USE_LODEPNG
#define LV_USE_LODEPNG 1
#endif

#ifndef LV_USE_LIBPNG
#define LV_USE_LIBPNG 0
#endif

#define LV_USE_BMP 0

#define LV_USE_TJPGD 1

#define LV_USE_LIBJPEG_TURBO 0

#define LV_USE_GIF 0
#if LV_USE_GIF

    #define LV_GIF_CACHE_DECODE_DATA 0
#endif

#define LV_BIN_DECODER_RAM_LOAD 0

#define LV_USE_RLE 0

#define LV_USE_QRCODE 0

#define LV_USE_BARCODE 0

#define LV_USE_FREETYPE 0
#if LV_USE_FREETYPE

    #define LV_FREETYPE_USE_LVGL_PORT 0

    #define LV_FREETYPE_CACHE_FT_GLYPH_CNT 256
#endif

#define LV_USE_TINY_TTF 0
#if LV_USE_TINY_TTF

    #define LV_TINY_TTF_FILE_SUPPORT 0
    #define LV_TINY_TTF_CACHE_GLYPH_CNT 256
#endif

#define LV_USE_RLOTTIE 0

#define LV_USE_VECTOR_GRAPHIC  0

#define LV_USE_THORVG_INTERNAL 0

#define LV_USE_THORVG_EXTERNAL 0

#define LV_USE_LZ4_INTERNAL  0

#define LV_USE_LZ4_EXTERNAL  0

#define LV_USE_FFMPEG 0
#if LV_USE_FFMPEG

    #define LV_FFMPEG_DUMP_FORMAT 0
#endif

#define LV_USE_SNAPSHOT 0

#ifndef LV_USE_SYSMON
#define LV_USE_SYSMON   0
#endif

#if LV_USE_SYSMON

    #define LV_SYSMON_GET_IDLE lv_timer_get_idle

    #ifndef LV_USE_PERF_MONITOR
    #define LV_USE_PERF_MONITOR 0
    #endif
    #if LV_USE_PERF_MONITOR
        #define LV_USE_PERF_MONITOR_POS LV_ALIGN_BOTTOM_RIGHT

        #define LV_USE_PERF_MONITOR_LOG_MODE 0
    #endif

    #ifndef LV_USE_MEM_MONITOR
    #define LV_USE_MEM_MONITOR 0
    #endif
    #if LV_USE_MEM_MONITOR
        #define LV_USE_MEM_MONITOR_POS LV_ALIGN_BOTTOM_LEFT
    #endif

#endif

#ifndef LV_USE_PROFILER
#define LV_USE_PROFILER 0
#endif
#if LV_USE_PROFILER

    #define LV_USE_PROFILER_BUILTIN 1
    #if LV_USE_PROFILER_BUILTIN

        #define LV_PROFILER_BUILTIN_BUF_SIZE (16 * 1024)
    #endif

    #define LV_PROFILER_INCLUDE "src/misc/lv_profiler_builtin.h"

    #define LV_PROFILER_BEGIN    LV_PROFILER_BUILTIN_BEGIN

    #define LV_PROFILER_END      LV_PROFILER_BUILTIN_END

    #define LV_PROFILER_BEGIN_TAG LV_PROFILER_BUILTIN_BEGIN_TAG

    #define LV_PROFILER_END_TAG   LV_PROFILER_BUILTIN_END_TAG

    #define LV_PROFILER_LAYOUT 0

    #define LV_PROFILER_REFR 0

    #define LV_PROFILER_DRAW 0

    #define LV_PROFILER_INDEV 0

    #define LV_PROFILER_DECODER 0

    #define LV_PROFILER_FONT 0

    #define LV_PROFILER_FS 0

    #define LV_PROFILER_STYLE 0

    #define LV_PROFILER_TIMER 0

    #define LV_PROFILER_CACHE 1
#endif

#define LV_USE_MONKEY 0

#define LV_USE_GRIDNAV 1

#define LV_USE_FRAGMENT 0

#define LV_USE_IMGFONT 0

#define LV_USE_OBSERVER 1

#define LV_USE_IME_PINYIN 0
#if LV_USE_IME_PINYIN

    #define LV_IME_PINYIN_USE_DEFAULT_DICT 1

    #define LV_IME_PINYIN_CAND_TEXT_NUM 6

    #define LV_IME_PINYIN_USE_K9_MODE      1
    #if LV_IME_PINYIN_USE_K9_MODE == 1
        #define LV_IME_PINYIN_K9_CAND_TEXT_NUM 3
    #endif
#endif

#define LV_USE_FILE_EXPLORER                     0
#if LV_USE_FILE_EXPLORER

    #define LV_FILE_EXPLORER_PATH_MAX_LEN        (128)

    #define LV_FILE_EXPLORER_QUICK_ACCESS        1
#endif

#define LV_USE_SDL              0
#if LV_USE_SDL
    #define LV_SDL_INCLUDE_PATH     <SDL2/SDL.h>
    #define LV_SDL_RENDER_MODE      LV_DISPLAY_RENDER_MODE_DIRECT
    #define LV_SDL_BUF_COUNT        1
    #define LV_SDL_ACCELERATED      1
    #define LV_SDL_FULLSCREEN       0
    #define LV_SDL_DIRECT_EXIT      1
    #define LV_SDL_MOUSEWHEEL_MODE  LV_SDL_MOUSEWHEEL_MODE_ENCODER
#endif

#define LV_USE_X11              USE_X11
#if LV_USE_X11
    #define LV_X11_DIRECT_EXIT         1
    #define LV_X11_DOUBLE_BUFFER       1

    #define LV_X11_RENDER_MODE_PARTIAL 1
    #define LV_X11_RENDER_MODE_DIRECT  0
    #define LV_X11_RENDER_MODE_FULL    0
#endif

#define LV_USE_WAYLAND          0
#if LV_USE_WAYLAND
    #define LV_WAYLAND_WINDOW_DECORATIONS   0
    #define LV_WAYLAND_WL_SHELL             0
#endif

#define LV_USE_LINUX_FBDEV      USE_FRAMEBUFFER
#if LV_USE_LINUX_FBDEV
    #define LV_LINUX_FBDEV_BSD           0
    #define LV_LINUX_FBDEV_RENDER_MODE   LV_DISPLAY_RENDER_MODE_PARTIAL
    #define LV_LINUX_FBDEV_BUFFER_COUNT  0
    #define LV_LINUX_FBDEV_BUFFER_SIZE   60
#endif

#define LV_USE_NUTTX    0

#if LV_USE_NUTTX
    #define LV_USE_NUTTX_LIBUV    0

    #define LV_USE_NUTTX_CUSTOM_INIT    0

    #define LV_USE_NUTTX_LCD      0
    #if LV_USE_NUTTX_LCD
        #define LV_NUTTX_LCD_BUFFER_COUNT    0
        #define LV_NUTTX_LCD_BUFFER_SIZE     60
    #endif

    #define LV_USE_NUTTX_TOUCHSCREEN    0

#endif

#define LV_USE_LINUX_DRM        0

#define LV_USE_TFT_ESPI         0

#ifndef LV_USE_EVDEV
#define LV_USE_EVDEV    0
#endif

#ifndef LV_USE_LIBINPUT
#define LV_USE_LIBINPUT    0
#endif

#if LV_USE_LIBINPUT
    #define LV_LIBINPUT_BSD    0

    #define LV_LIBINPUT_XKB    1
    #if LV_LIBINPUT_XKB

        #define LV_LIBINPUT_XKB_KEY_MAP { .rules = "evdev", .model = "pc105", .layout = "us", .variant = NULL, .options = NULL }
    #endif
#endif

#define LV_USE_ST7735        0
#define LV_USE_ST7789        0
#define LV_USE_ST7796        0
#define LV_USE_ILI9341       0

#define LV_USE_GENERIC_MIPI (LV_USE_ST7735 | LV_USE_ST7789 | LV_USE_ST7796 | LV_USE_ILI9341)

#define LV_USE_RENESAS_GLCDC    0

#define LV_USE_WINDOWS    0

#define LV_USE_OPENGLES   0
#if LV_USE_OPENGLES
    #define LV_USE_OPENGLES_DEBUG        1
#endif

#define LV_USE_QNX              0
#if LV_USE_QNX
    #define LV_QNX_BUF_COUNT        1
#endif

#define LV_BUILD_EXAMPLES 0

#define LV_USE_DEMO_WIDGETS 0

#define LV_USE_DEMO_KEYPAD_AND_ENCODER 0

#define LV_USE_DEMO_BENCHMARK 0

#define LV_USE_DEMO_RENDER 0

#define LV_USE_DEMO_STRESS 0

#define LV_USE_DEMO_MUSIC 0
#if LV_USE_DEMO_MUSIC
    #define LV_DEMO_MUSIC_SQUARE    0
    #define LV_DEMO_MUSIC_LANDSCAPE 0
    #define LV_DEMO_MUSIC_ROUND     0
    #define LV_DEMO_MUSIC_LARGE     0
    #define LV_DEMO_MUSIC_AUTO_PLAY 0
#endif

#define LV_USE_DEMO_FLEX_LAYOUT     0

#define LV_USE_DEMO_MULTILANG       0

#define LV_USE_DEMO_TRANSFORM       0

#define LV_USE_DEMO_SCROLL          0

#define LV_USE_DEMO_VECTOR_GRAPHIC  0

#endif

#endif
