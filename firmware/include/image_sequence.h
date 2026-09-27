#pragma once
#include <ArduinoJson.h>
#include <lvgl.h>
#include "bridge_queue.h"
namespace ImageSequence {
// GUI thread only. The network worker owns requests; completed bytes transfer here.
void attach(lv_obj_t* image, JsonArray ids, unsigned width, unsigned height, uint32_t intervalMs, const char* cacheKey = "");
void beginScene();
void tick();
void receive(uint32_t ticket, uint8_t* bytes, size_t length);
}
