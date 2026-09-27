#pragma once

/**
 * Hardware profile for current dev target.
 * Existing defaults still match the earlier ESP32-S3 + 800x480 bring-up board.
 * Adjust pins / resolution for the ESP32-P4C6 1024x600 board when moving to it.
 */

#ifndef MACRO_PAD_HEADLESS
#define MACRO_PAD_HEADLESS 1
#endif

#ifndef MACRO_PAD_ENABLE_USB_HID
#define MACRO_PAD_ENABLE_USB_HID 1
#endif

#ifndef PANEL_WIDTH
#if defined(CONFIG_IDF_TARGET_ESP32P4)
#define PANEL_WIDTH 1024
#else
#define PANEL_WIDTH 800
#endif
#endif
#ifndef PANEL_HEIGHT
#if defined(CONFIG_IDF_TARGET_ESP32P4)
#define PANEL_HEIGHT 600
#else
#define PANEL_HEIGHT 480
#endif
#endif

#define LAYOUT_SCREEN_PAD 16
#define LAYOUT_GRID_GAP 12

#define LAYOUT_POLL_MS 8000
#define WIDGET_DATA_POLL_MS 30000
#define SCREEN_RENDER_POLL_MS 30000
#define STATUS_POLL_UNPAIRED_MS 5000
#define MEDIA_POLL_MS 15000
#define CAMERA_FRAME_MS 200

/** Backlight PWM */
#ifndef PIN_BACKLIGHT
#if defined(CONFIG_IDF_TARGET_ESP32P4)
#define PIN_BACKLIGHT 26
#else
#define PIN_BACKLIGHT 2
#endif
#endif

/** I2C bus (touch GT911 + MCP23017 encoders) */
#ifndef I2C_SDA
#if defined(CONFIG_IDF_TARGET_ESP32P4)
#define I2C_SDA 7
#else
#define I2C_SDA 8
#endif
#endif
#ifndef I2C_SCL
#if defined(CONFIG_IDF_TARGET_ESP32P4)
#define I2C_SCL 8
#else
#define I2C_SCL 9
#endif
#endif
#define ENC_I2C_ADDR 0x20
#if defined(CONFIG_IDF_TARGET_ESP32P4)
// Keep the same legacy driver family as touch, on a separate controller.
#define ENC_I2C_DISABLED 0
#define ENC_I2C_SDA 47
#define ENC_I2C_SCL 48
#define ENC_THIRD_B_PIN 46
#else
#define ENC_I2C_DISABLED 0
#define ENC_I2C_SDA I2C_SDA
#define ENC_I2C_SCL I2C_SCL
#endif
#define GT911_I2C_ADDR 0x5D

/** GT911 reset / interrupt — set to -1 if unused */
#ifndef PIN_TOUCH_RST
#if defined(CONFIG_IDF_TARGET_ESP32P4)
#define PIN_TOUCH_RST 20
#else
#define PIN_TOUCH_RST -1
#endif
#endif
#ifndef PIN_TOUCH_INT
#if defined(CONFIG_IDF_TARGET_ESP32P4)
#define PIN_TOUCH_INT 21
#else
#define PIN_TOUCH_INT -1
#endif
#endif

/**
 * RGB panel data/clock pins (LovyanGFX Parallel 16-bit example).
 * Replace with your schematic values.
 */
#ifndef LCD_PIN_DE
#define LCD_PIN_DE 40
#define LCD_PIN_VSYNC 41
#define LCD_PIN_HSYNC 39
#define LCD_PIN_PCLK 42
#define LCD_PIN_R0 45
#define LCD_PIN_R1 48
#define LCD_PIN_R2 47
#define LCD_PIN_R3 21
#define LCD_PIN_R4 14
#define LCD_PIN_G0 5
#define LCD_PIN_G1 6
#define LCD_PIN_G2 7
#define LCD_PIN_G3 15
#define LCD_PIN_G4 16
#define LCD_PIN_G5 4
#define LCD_PIN_B0 17
#define LCD_PIN_B1 3
#define LCD_PIN_B2 46
#define LCD_PIN_B3 18
#define LCD_PIN_B4 1
#endif

/** Max JPEG decode target (photos / camera frame) */
#if defined(CONFIG_IDF_TARGET_ESP32P4)
#define MEDIA_MAX_W 1024
#define MEDIA_MAX_H 600
#else
#define MEDIA_MAX_W 800
#define MEDIA_MAX_H 480
#endif
#define MEDIA_JPEG_MAX_BYTES (512 * 1024)
#define MEDIA_RGB565_FRAME_BYTES ((size_t)MEDIA_MAX_W * MEDIA_MAX_H * sizeof(uint16_t))
