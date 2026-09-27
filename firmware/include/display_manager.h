#pragma once

#include <lvgl.h>
#include <Arduino.h>

class DisplayManager {
public:
  static void init();
  static void loop();
  static int width();
  static int height();
  static void setLogicalSize(int w, int h);
  static void setBacklightPercent(int pct);
  static void drawRgb565(int x, int y, int w, int h, const uint16_t* pix);
  /** Copy only the scanlines that differ from `previous` into the live buffer. */
  static void drawRgb565Diff(int w, int h, const uint16_t* next, const uint16_t* previous);
  /** Fill the whole scan-out buffer with one RGB565 color (clears garbage). */
  static void fillColor(uint16_t rgb565);
  /** When true, LVGL stops flushing so a full-screen RGB565 bake stays visible. */
  static void setLvglPaused(bool paused);
  static bool isLvglPaused();

private:
  static int w_;
  static int h_;
  static bool lvglPaused_;
};
