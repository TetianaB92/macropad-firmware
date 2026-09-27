#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

/** Offline cache of last layout JSON on flash */
class ConfigStore {
public:
  static void init();
  static bool loadFromFlash(JsonDocument& dest);
  static bool saveToFlash(const JsonDocument& doc);
  static bool loadScene(String& dest);
  static bool saveScene(const char* json);
  static bool cachedChromePage(char* pageId, size_t capacity, int width, int height);
  /** Last active page chrome (RGB565) so reboot can paint immediately. */
  static bool saveChrome(const char* pageId, int width, int height,
                         const uint8_t* bytes, size_t length);
  /** Allocates *bytesOut (caller owns). Matches pageId + resolution or fails. */
  static bool loadChrome(const char* pageId, int width, int height,
                         uint8_t** bytesOut, size_t* lengthOut);
};
