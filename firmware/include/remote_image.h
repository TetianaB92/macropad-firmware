#pragma once
#include <lvgl.h>
#include "bridge_queue.h"
namespace RemoteImage {
void beginPage();
bool isCurrent(uint32_t epoch);
void attach(lv_obj_t* image, const char* url, unsigned width, unsigned height, uint32_t refreshMs, bool cover);
void tick();
void receive(uint32_t ticket, uint8_t* bytes, size_t length);
// Network task only. A bounded JPEG frame becomes a tile-sized RGB565 image.
uint8_t* decode(const uint8_t* jpeg, size_t length, unsigned width, unsigned height, bool cover);
void fetch(const SnapshotRequest& request);
}
