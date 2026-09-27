#pragma once

#include <Arduino.h>
#include "bridge_queue.h"

class DeviceNetworkManager {
public:
  static void init();
  static void loop();
  static void handleAction(const char* action);
  static void handleSduiAction(const SduiActionRequest& action);

  /** Force immediate layout + widget refresh */
  static void requestSync();

private:
  static bool ensureWifi();
  static bool httpGet(const char* path, String& bodyOut);
  static bool httpGetBinary(const char* path, uint8_t** bodyOut, size_t* lenOut,
                            RenderedScreenFormat* formatOut, int* statusOut = nullptr, size_t expectedBytes = 0);
  static bool httpPostJson(const char* path, const String& json, String& bodyOut);
  static void registerDevice();
  static void pollStatus();
  static void pollLayout();
  static void pollWidgetData();
  static bool pollRenderedScreen();
  static bool pollChrome();
  static void pollScene();
  static void pollSduiState();
  static int tzOffsetMin();

  static uint32_t lastStatusMs_;
  static uint32_t lastLayoutMs_;
  static uint32_t lastWidgetMs_;
  static bool forceSync_;
};
