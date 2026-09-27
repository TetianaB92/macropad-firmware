#pragma once

#include <Arduino.h>

/** SoftAP captive portal — friend enters home Wi‑Fi on phone (no code). */
class Provisioning {
public:
  static void begin();
  static void loop();
  static bool isActive();
  static const char* apName();
};
