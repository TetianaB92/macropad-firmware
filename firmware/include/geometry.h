#pragma once

#include <Arduino.h>

struct ScreenGeom {
  int panelW;
  int panelH;
  int pad;
  int gap;
  int cols;
  int rows;
  int contentW;
  int contentH;
  float cellW;
  float cellH;
};

struct CellRect {
  int x;
  int y;
  int w;
  int h;
};

ScreenGeom makeGeom(int panelW, int panelH, int cols, int rows);
CellRect cellToPx(const ScreenGeom& g, int x, int y, int w, int h);

void resolvePresetSize(const char* presetId, int* outW, int* outH);
