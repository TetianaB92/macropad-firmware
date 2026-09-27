#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <ArduinoJson.h>

class EncoderManager {
public:
  static void init();
  static void loop(QueueHandle_t actionQueue);
  /** Bind rotate/press from ScreenLayout.encoders */
  static void applyFromLayout(JsonDocument& layout);
  static void handleSimLine(QueueHandle_t q, const char* text);
  static void emitRotate(QueueHandle_t q, int idx, int dir);
  static void emitPress(QueueHandle_t q, int idx);

private:
  struct Binding {
    bool enabled;
    char rotate[32];
    char press[32];
  };
  static Binding bindings_[5];
  static bool chassisEnabled_;
};
