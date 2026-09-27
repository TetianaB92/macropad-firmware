#include "geometry.h"
#include "board_config.h"
#include <string.h>

ScreenGeom makeGeom(int panelW, int panelH, int cols, int rows) {
  ScreenGeom g{};
  g.panelW = panelW;
  g.panelH = panelH;
  g.pad = LAYOUT_SCREEN_PAD;
  g.gap = LAYOUT_GRID_GAP;
  g.cols = cols > 0 ? cols : 4;
  g.rows = rows > 0 ? rows : 3;
  g.contentW = panelW - g.pad * 2;
  g.contentH = panelH - g.pad * 2;
  g.cellW =
      (g.contentW - (g.cols - 1) * g.gap) / (float)g.cols;
  g.cellH =
      (g.contentH - (g.rows - 1) * g.gap) / (float)g.rows;
  return g;
}

CellRect cellToPx(const ScreenGeom& g, int x, int y, int w, int h) {
  CellRect r{};
  r.x = g.pad + (int)(x * (g.cellW + g.gap));
  r.y = g.pad + (int)(y * (g.cellH + g.gap));
  r.w = (int)(w * g.cellW + (w - 1) * g.gap);
  r.h = (int)(h * g.cellH + (h - 1) * g.gap);
  if (r.w < 8) r.w = 8;
  if (r.h < 8) r.h = 8;
  return r;
}

void resolvePresetSize(const char* presetId, int* outW, int* outH) {
  *outW = PANEL_WIDTH;
  *outH = PANEL_HEIGHT;
  if (!presetId) return;
  if (strcmp(presetId, "5inch") == 0 || strcmp(presetId, "7inch_wvga") == 0) {
    *outW = 800;
    *outH = 480;
  } else if (strcmp(presetId, "7inch") == 0) {
    *outW = 1024;
    *outH = 600;
  } else if (strcmp(presetId, "7inch_hd") == 0) {
    *outW = 1280;
    *outH = 800;
  }
}
