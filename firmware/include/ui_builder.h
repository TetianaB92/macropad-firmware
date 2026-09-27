#pragma once

#include <stddef.h>
#include <stdint.h>

#include <ArduinoJson.h>
#include <lvgl.h>
#include "sdui_engine.h"

// Note: JDEC and JRECT forward declarations are no longer needed
// as we handle it differently based on LV_USE_SJPG flag in cpp

class UIBuilder {
public:
  static void init();
  static void showBoot(const char* msg);
  static void showProvisioning(const char* apName);
  static void showPairing(const char* code);
  static void showPairedSuccess();
  static bool showRenderedScreen(const uint8_t* imageBytes, size_t imageLen);
  static bool showRenderedScreenRgb565(uint8_t* imageBytes, size_t imageLen);
  /** Put a full-screen RGB565 bake under SDUI overlays (does not wipe the scene). */
  static bool applyChromeRgb565(uint8_t* imageBytes, size_t imageLen, const char* pageId, uint32_t generation);
  static void buildFromLayout(JsonDocument& layout);
  static void updateLiveData(JsonDocument& widgetData);
  static void tickLocalWidgets();
  
  // SDUI Entry points
  static bool loadSDUIScene(const char* jsonScene, uint32_t generation = 0);
  static SDUIEngine& getSDUIEngine();

private:
  static lv_obj_t* root_;
  static lv_obj_t* overlay_;
  static lv_obj_t* screenImage_;
  static uint8_t* screenImageBytes_;
  static lv_img_dsc_t screenImageDsc_;
  static uint32_t lastLocalTick_;
  static SDUIEngine sduiEngine_;

  static void clearUi();
  static lv_obj_t* makeTile(lv_obj_t* parent, int px, int py, int pw, int ph);
  static void styleTile(lv_obj_t* tile);
  static lv_obj_t* addTitle(lv_obj_t* tile, const char* title);
  static lv_obj_t* addBody(lv_obj_t* tile, const char* text);
  static lv_obj_t* addBar(lv_obj_t* tile, int y);
  static void buildWidget(lv_obj_t* parent, JsonObjectConst widget, int panelW,
                          int panelH, int cols, int rows);
};
