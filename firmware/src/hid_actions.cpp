#include "hid_actions.h"
#include "board_config.h"
#include "display_manager.h"
#include "serial_bridge.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <string.h>

#if !MACRO_PAD_HEADLESS && MACRO_PAD_ENABLE_USB_HID
#include "USB.h"
#include "USBHIDConsumerControl.h"
static USBHIDConsumerControl g_consumer;
static bool g_usbOk = false;
#endif

static int brightnessPct = 80;

static void notifyCompanion(const char* kind, int delta) {
  JsonDocument doc;
  doc["protocol"] = "macropad.encoder";
  doc["action"] = kind;
  if (delta) doc["delta"] = delta;
  SerialBridge::send(doc);
}

void HidActions::init() {
#if !MACRO_PAD_HEADLESS && MACRO_PAD_ENABLE_USB_HID
  USB.begin();
  g_consumer.begin();
  g_usbOk = true;
  Serial.println("HidActions: USB ConsumerControl started");
#else
  Serial.println("HidActions: serial/backlight mode");
#endif
  DisplayManager::setBacklightPercent(brightnessPct);
}

void HidActions::volumeDelta(int steps) {
  Serial.printf("HID volume %+d\n", steps);
  notifyCompanion("volume", steps);
#if !MACRO_PAD_HEADLESS && MACRO_PAD_ENABLE_USB_HID
  if (!g_usbOk) return;
  for (int i = 0; i < abs(steps); i++) {
    if (steps > 0)
      g_consumer.press(CONSUMER_CONTROL_VOLUME_INCREMENT);
    else
      g_consumer.press(CONSUMER_CONTROL_VOLUME_DECREMENT);
    g_consumer.release();
    delay(5);
  }
#endif
}

void HidActions::muteToggle() {
  Serial.println("HID mute toggle");
  notifyCompanion("mute", 0);
#if !MACRO_PAD_HEADLESS && MACRO_PAD_ENABLE_USB_HID
  if (!g_usbOk) return;
  g_consumer.press(CONSUMER_CONTROL_MUTE);
  g_consumer.release();
#endif
}

void HidActions::brightnessDelta(int steps) {
  brightnessPct = constrain(brightnessPct + steps * 5, 5, 100);
  Serial.printf("Brightness → %d%%\n", brightnessPct);
  DisplayManager::setBacklightPercent(brightnessPct);
}

void HidActions::dispatch(const char* action) {
  if (!action) return;
  if (strncmp(action, "ENC_ROTATE:", 11) == 0) {
    const char* p = action + 11;
    while (*p && *p != ':') p++;
    if (*p == ':') p++;
    char rotate[32] = {0};
    int i = 0;
    while (*p && *p != ':' && i < 31) rotate[i++] = *p++;
    int dir = 1;
    if (*p == ':' && *(p + 1) == '-') dir = -1;
    if (strcmp(rotate, "system_volume") == 0) volumeDelta(dir);
    else if (strcmp(rotate, "display_brightness") == 0)
      brightnessDelta(dir);
    else if (strcmp(rotate, "monitor_brightness") == 0)
      notifyCompanion("monitor_brightness", dir);
    else if (strcmp(rotate, "hotkey_brackets") == 0)
      notifyCompanion("hotkey_brackets", dir);
    else
      Serial.printf("Custom rotate %s dir=%d\n", rotate, dir);
    return;
  }
  if (strncmp(action, "ENC_PRESS:", 10) == 0) {
    const char* p = action + 10;
    while (*p && *p != ':') p++;
    if (*p == ':') p++;
    if (strcmp(p, "system_mute") == 0) muteToggle();
    else Serial.printf("Custom press %s\n", p);
  }
}
