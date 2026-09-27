#include "device_state.h"
#include <WiFi.h>
#include <Preferences.h>
#include <esp_mac.h>

DevicePhase DeviceState::phase_ = DevicePhase::Boot;
char DeviceState::mac_[24] = {0};
char DeviceState::token_[80] = {0};
char DeviceState::pairCode_[16] = {0};
char DeviceState::layoutUpdated_[40] = {0};
bool DeviceState::paired_ = false;
bool DeviceState::hasLayout_ = false;
bool DeviceState::hasScene_ = false;
JsonDocument DeviceState::layout_;
JsonDocument DeviceState::widgetData_;
String DeviceState::sceneJson_;
String DeviceState::sduiPatchJson_;

static Preferences prefs;

void DeviceState::init() {
  layout_.to<JsonObject>();
  widgetData_.to<JsonObject>();

  uint8_t b[6] = {0};
  esp_efuse_mac_get_default(b);
  snprintf(mac_, sizeof(mac_), "%02X:%02X:%02X:%02X:%02X:%02X",
           b[0], b[1], b[2], b[3], b[4], b[5]);

  loadNvs();
  phase_ = DevicePhase::Boot;
  Serial.printf("DeviceState: MAC %s token=%s\n", mac_,
                token_[0] ? "(present)" : "(none)");
}

DevicePhase DeviceState::phase() { return phase_; }
void DeviceState::setPhase(DevicePhase p) { phase_ = p; }

const char* DeviceState::mac() { return mac_; }
const char* DeviceState::deviceToken() { return token_; }

void DeviceState::setDeviceToken(const char* token) {
  if (!token) {
    token_[0] = 0;
    return;
  }
  strncpy(token_, token, sizeof(token_) - 1);
  token_[sizeof(token_) - 1] = 0;
  saveNvs();
}

const char* DeviceState::pairingCode() { return pairCode_; }
void DeviceState::setPairingCode(const char* code) {
  if (!code) {
    pairCode_[0] = 0;
    return;
  }
  strncpy(pairCode_, code, sizeof(pairCode_) - 1);
  pairCode_[sizeof(pairCode_) - 1] = 0;
}

bool DeviceState::paired() { return paired_; }
void DeviceState::setPaired(bool v) {
  paired_ = v;
  saveNvs();
}

const char* DeviceState::layoutUpdatedAt() { return layoutUpdated_; }
void DeviceState::setLayoutUpdatedAt(const char* iso) {
  if (!iso) {
    layoutUpdated_[0] = 0;
    return;
  }
  strncpy(layoutUpdated_, iso, sizeof(layoutUpdated_) - 1);
  layoutUpdated_[sizeof(layoutUpdated_) - 1] = 0;
}

JsonDocument& DeviceState::layoutDoc() { return layout_; }
JsonDocument& DeviceState::widgetDataDoc() { return widgetData_; }
bool DeviceState::hasLayout() { return hasLayout_; }
void DeviceState::setHasLayout(bool v) { hasLayout_ = v; }

const char* DeviceState::sceneJson() { return sceneJson_.c_str(); }
void DeviceState::setSceneJson(const String& json) {
  sceneJson_ = json;
  hasScene_ = json.length() > 0;
}
bool DeviceState::hasScene() { return hasScene_; }

const char* DeviceState::sduiPatchJson() { return sduiPatchJson_.c_str(); }
void DeviceState::setSduiPatchJson(const String& json) { sduiPatchJson_ = json; }

void DeviceState::loadNvs() {
  prefs.begin("macropad", true);
  String t = prefs.getString("token", "");
  if (t.length() > 0) {
    strncpy(token_, t.c_str(), sizeof(token_) - 1);
  }
  paired_ = prefs.getBool("paired", false);
  prefs.end();
}

void DeviceState::saveNvs() {
  prefs.begin("macropad", false);
  prefs.putString("token", token_);
  prefs.putBool("paired", paired_);
  prefs.end();
}

void DeviceState::factoryReset() {
  prefs.begin("macropad", false);
  prefs.clear();
  prefs.end();
  Serial.println("[FACTORY] NVS wiped — rebooting…");
  delay(200);
  ESP.restart();
}
