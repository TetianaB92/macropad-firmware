#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

enum class DevicePhase {
  Boot,
  Provisioning,
  WifiConnecting,
  Unpaired,
  Synced,
  Active,
  Error,
};

class DeviceState {
public:
  static void init();

  static DevicePhase phase();
  static void setPhase(DevicePhase p);

  static const char* mac();
  static const char* deviceToken();
  static void setDeviceToken(const char* token);

  static const char* pairingCode();
  static void setPairingCode(const char* code);

  static bool paired();
  static void setPaired(bool v);

  static const char* layoutUpdatedAt();
  static void setLayoutUpdatedAt(const char* iso);

  /** Full ScreenLayout document */
  static JsonDocument& layoutDoc();
  static bool hasLayout();
  static void setHasLayout(bool v);

  /** Last /widget-data payload */
  static JsonDocument& widgetDataDoc();

  static const char* sceneJson();
  static void setSceneJson(const String& json);
  static bool hasScene();

  static const char* sduiPatchJson();
  static void setSduiPatchJson(const String& json);

  static void loadNvs();
  static void saveNvs();
  static void factoryReset();

private:
  static DevicePhase phase_;
  static char mac_[24];
  static char token_[80];
  static char pairCode_[16];
  static char layoutUpdated_[40];
  static bool paired_;
  static JsonDocument layout_;
  static JsonDocument widgetData_;
  static bool hasLayout_;
  static String sceneJson_;
  static String sduiPatchJson_;
  static bool hasScene_;
};
