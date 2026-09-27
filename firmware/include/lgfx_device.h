#pragma once

#include "board_config.h"

#if !MACRO_PAD_HEADLESS && !defined(MACRO_PAD_USE_ESP_PANEL)
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>

/**
 * Panel device — pins from board_config.h.
 * Member names use *_instance to avoid clashing with LGFX_Device::_panel.
 */
class LgfxDevice : public lgfx::LGFX_Device {
  lgfx::Bus_RGB _bus_instance;
  lgfx::Panel_RGB _panel_instance;
  lgfx::Light_PWM _light_instance;
  lgfx::Touch_GT911 _touch_instance;

 public:
  LgfxDevice() {
    {
      auto cfg = _bus_instance.config();
      cfg.panel = &_panel_instance;
      cfg.pin_d0 = LCD_PIN_B0;
      cfg.pin_d1 = LCD_PIN_B1;
      cfg.pin_d2 = LCD_PIN_B2;
      cfg.pin_d3 = LCD_PIN_B3;
      cfg.pin_d4 = LCD_PIN_B4;
      cfg.pin_d5 = LCD_PIN_G0;
      cfg.pin_d6 = LCD_PIN_G1;
      cfg.pin_d7 = LCD_PIN_G2;
      cfg.pin_d8 = LCD_PIN_G3;
      cfg.pin_d9 = LCD_PIN_G4;
      cfg.pin_d10 = LCD_PIN_G5;
      cfg.pin_d11 = LCD_PIN_R0;
      cfg.pin_d12 = LCD_PIN_R1;
      cfg.pin_d13 = LCD_PIN_R2;
      cfg.pin_d14 = LCD_PIN_R3;
      cfg.pin_d15 = LCD_PIN_R4;
      cfg.pin_henable = LCD_PIN_DE;
      cfg.pin_vsync = LCD_PIN_VSYNC;
      cfg.pin_hsync = LCD_PIN_HSYNC;
      cfg.pin_pclk = LCD_PIN_PCLK;
      cfg.freq_write = 16000000;
      cfg.hsync_polarity = 0;
      cfg.hsync_front_porch = 8;
      cfg.hsync_pulse_width = 4;
      cfg.hsync_back_porch = 16;
      cfg.vsync_polarity = 0;
      cfg.vsync_front_porch = 4;
      cfg.vsync_pulse_width = 4;
      cfg.vsync_back_porch = 16;
      cfg.pclk_idle_high = 1;
      _bus_instance.config(cfg);
    }
    {
      auto cfg = _panel_instance.config();
      cfg.memory_width = PANEL_WIDTH;
      cfg.memory_height = PANEL_HEIGHT;
      cfg.panel_width = PANEL_WIDTH;
      cfg.panel_height = PANEL_HEIGHT;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      _panel_instance.config(cfg);
    }
    {
      auto cfg = _light_instance.config();
      cfg.pin_bl = PIN_BACKLIGHT;
      cfg.invert = false;
      cfg.freq = 44100;
      cfg.pwm_channel = 7;
      _light_instance.config(cfg);
      _panel_instance.setLight(&_light_instance);
    }
    {
      auto cfg = _touch_instance.config();
      cfg.x_min = 0;
      cfg.x_max = PANEL_WIDTH - 1;
      cfg.y_min = 0;
      cfg.y_max = PANEL_HEIGHT - 1;
      cfg.pin_int = PIN_TOUCH_INT;
      cfg.pin_rst = PIN_TOUCH_RST;
      cfg.bus_shared = false;
      cfg.offset_rotation = 0;
      cfg.i2c_port = 0;
      cfg.i2c_addr = GT911_I2C_ADDR;
      cfg.pin_sda = I2C_SDA;
      cfg.pin_scl = I2C_SCL;
      cfg.freq = 400000;
      _touch_instance.config(cfg);
      _panel_instance.setTouch(&_touch_instance);
    }

    _panel_instance.setBus(&_bus_instance);
    setPanel(&_panel_instance);
  }
};

inline LgfxDevice& lcd() {
  static LgfxDevice inst;
  return inst;
}
#endif
