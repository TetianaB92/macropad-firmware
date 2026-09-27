#pragma once
#define LV_CONF_H

/*
 * Keep this config intentionally small and rely on LVGL defaults for the rest.
 * Build flags enable LV_CONF_INCLUDE_SIMPLE and the Montserrat fonts we use.
 */

#ifndef LV_COLOR_DEPTH
#define LV_COLOR_DEPTH 16
#endif

#ifndef LV_COLOR_16_SWAP
#define LV_COLOR_16_SWAP 0
#endif

/*
 * Use the system allocator for LVGL's internal objects/styles/draw masks.
 * The default 48 KB static pool is fine for the 800x480 board but is exhausted
 * while rendering the full 1024x600 SDUI scene (multi-tile, rounded borders),
 * where the failed allocation returns NULL and faults inside the draw layer.
 */
#ifndef LV_MEM_CUSTOM
#define LV_MEM_CUSTOM 1
#endif

#ifndef LV_FONT_MONTSERRAT_12
#define LV_FONT_MONTSERRAT_12 1
#endif

#ifndef LV_FONT_MONTSERRAT_14
#define LV_FONT_MONTSERRAT_14 1
#endif

#ifndef LV_FONT_MONTSERRAT_28
#define LV_FONT_MONTSERRAT_28 1
#endif

#ifndef LV_USE_LOG
#define LV_USE_LOG 0
#endif

#ifndef LV_USE_SJPG
#if defined(MACRO_PAD_USE_ESP_PANEL)
#define LV_USE_SJPG 1
#else
#define LV_USE_SJPG 0
#endif
#endif

/* LVGL refresh, input and animation timers must advance even without a tick ISR. */
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "esp_timer.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR ((uint32_t)(esp_timer_get_time() / 1000LL))

#ifndef LV_FONT_MONTSERRAT_48
#define LV_FONT_MONTSERRAT_48 1
#endif
