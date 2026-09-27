#pragma once

#include <Arduino.h>
#include "board_config.h"
#include <lvgl.h>

/**
 * HTTP JPEG / MJPEG → RGB565 buffer for camera & photos_ambient tiles.
 * Works in headless (decode only) and on-panel (optional direct blit).
 */
class MediaPipeline {
public:
  static void init();
  static void loop();

  /** Bind an LVGL img/canvas object to a URL (MJPEG or still JPEG).
   *  refreshMs: <0 default poll, 0 fetch once, >0 poll interval. */
  static bool attach(lv_obj_t* img, const char* url, bool mjpeg,
                     int refreshMs = -1);
  static void detachAll();

  /** Cycle photos_ambient URLs from JSON array string list. */
  static bool attachAlbum(lv_obj_t* img, const char* const* urls, int count,
                          uint32_t rotateMs);

private:
  struct Slot {
    bool used;
    bool mjpeg;
    bool album;
    lv_obj_t* img;
    char url[512];
    char albumUrls[8][256];
    uint32_t pollMs;
    int albumCount;
    int albumIndex;
    uint32_t rotateMs;
    uint32_t lastMs;
    uint16_t* rgb;
    lv_img_dsc_t dsc;
    int w;
    int h;
  };

  static constexpr int kSlots = 8;
  static Slot slots_[kSlots];

  static bool httpFetch(const char* url, uint8_t** out, size_t* outLen);
  static bool httpFetchMjpegFrame(const char* url, uint8_t** out,
                                  size_t* outLen);
  static bool decodeJpegToSlot(Slot& s, const uint8_t* jpg, size_t len);
  static void refreshSlot(Slot& s);
};
