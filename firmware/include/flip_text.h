#pragma once
#include <lvgl.h>

// A text presentation primitive. Value, time source and business meaning are
// supplied by the scene. Only the changing face is animated; bounds never move.
namespace FlipText {
lv_obj_t* create(lv_obj_t* parent);
void setFontSize(lv_obj_t* obj, int pixels);
void setPixelFont(lv_obj_t* obj, bool pixel);
bool set(lv_obj_t* obj, const char* text, bool animate);
}
