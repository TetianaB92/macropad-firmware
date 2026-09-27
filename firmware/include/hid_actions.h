#pragma once

/** USB HID consumer / display brightness stubs */
class HidActions {
public:
  static void init();
  /** "ENC_ROTATE:0:system_volume:+1" etc. */
  static void dispatch(const char* action);
  static void volumeDelta(int steps);
  static void muteToggle();
  static void brightnessDelta(int steps);
};
