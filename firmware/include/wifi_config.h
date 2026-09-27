#pragma once

#include <Arduino.h>

/**
 * Wi‑Fi + API URL stored in NVS (product flow).
 * Friend never edits secrets.h — SoftAP portal on first boot.
 */
class WifiConfig {
public:
  static void init();

  static bool hasWifi();
  static const char* ssid();
  static const char* password();
  static const char* apiBaseUrl();

  static void save(const char* ssid, const char* password, const char* apiUrl);
  static void clear();

  /** Optional compile-time factory default (production site). */
  static const char* factoryApiDefault();
};
