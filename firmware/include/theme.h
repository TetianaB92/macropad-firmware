#pragma once

#include <lvgl.h>

/** Visual language aligned with web preview (accent #d97736) */
namespace Theme {
inline lv_color_t bg() { return lv_color_hex(0x0c0c0e); }
inline lv_color_t panel() { return lv_color_hex(0x16161a); }
inline lv_color_t border() { return lv_color_hex(0x2a2a30); }
inline lv_color_t accent() { return lv_color_hex(0xd97736); }
inline lv_color_t text() { return lv_color_hex(0xf4f4f5); }
inline lv_color_t muted() { return lv_color_hex(0x71717a); }
inline lv_color_t danger() { return lv_color_hex(0xef4444); }
}
