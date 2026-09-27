/* VIEWE UEP4S070H1024V600C-WBA-B.
 * Touch GPIOs verified against the user's GPIO-definition drawing.
 * GT911 / active-low reset verified against VIEWESMART's esp32_p4_s070_wba BSP:
 * https://github.com/VIEWESMART/7-1024X600-ESP32-P4-C6-TOUCH-DISPLAY
 */

#pragma once

#define ESP_PANEL_BOARD_DEFAULT_USE_CUSTOM (1)

#if ESP_PANEL_BOARD_DEFAULT_USE_CUSTOM
#define ESP_PANEL_BOARD_NAME "VIEWE:UEP4S070H1024V600C-WBA-B"

#define ESP_PANEL_BOARD_WIDTH  (1024)
#define ESP_PANEL_BOARD_HEIGHT (600)

#define ESP_PANEL_BOARD_USE_LCD      (1)
#define ESP_PANEL_BOARD_USE_TOUCH    (1)
#define ESP_PANEL_BOARD_USE_BACKLIGHT (1)
#define ESP_PANEL_BOARD_USE_EXPANDER (0)

#if ESP_PANEL_BOARD_USE_LCD
#define ESP_PANEL_BOARD_LCD_CONTROLLER   EK79007
#define ESP_PANEL_BOARD_LCD_BUS_TYPE     (ESP_PANEL_BUS_TYPE_MIPI_DSI)

#define ESP_PANEL_BOARD_LCD_MIPI_DSI_LANE_NUM       (2)
#define ESP_PANEL_BOARD_LCD_MIPI_DSI_LANE_RATE_MBPS (600)
#define ESP_PANEL_BOARD_LCD_MIPI_DPI_CLK_MHZ        (52)
#define ESP_PANEL_BOARD_LCD_MIPI_DPI_PIXEL_BITS     (ESP_PANEL_LCD_COLOR_BITS_RGB565)
#define ESP_PANEL_BOARD_LCD_MIPI_DPI_HPW            (10)
#define ESP_PANEL_BOARD_LCD_MIPI_DPI_HBP            (160)
#define ESP_PANEL_BOARD_LCD_MIPI_DPI_HFP            (160)
#define ESP_PANEL_BOARD_LCD_MIPI_DPI_VPW            (1)
#define ESP_PANEL_BOARD_LCD_MIPI_DPI_VBP            (23)
#define ESP_PANEL_BOARD_LCD_MIPI_DPI_VFP            (12)
#define ESP_PANEL_BOARD_LCD_MIPI_PHY_LDO_ID         (3)

#define ESP_PANEL_BOARD_LCD_COLOR_BITS       (ESP_PANEL_LCD_COLOR_BITS_RGB565)
#define ESP_PANEL_BOARD_LCD_COLOR_BGR_ORDER  (1)
#define ESP_PANEL_BOARD_LCD_COLOR_INEVRT_BIT (0)

#define ESP_PANEL_BOARD_LCD_SWAP_XY   (0)
#define ESP_PANEL_BOARD_LCD_MIRROR_X  (1)
#define ESP_PANEL_BOARD_LCD_MIRROR_Y  (1)
#define ESP_PANEL_BOARD_LCD_GAP_X     (0)
#define ESP_PANEL_BOARD_LCD_GAP_Y     (0)

#define ESP_PANEL_BOARD_LCD_RST_IO    (22)
#define ESP_PANEL_BOARD_LCD_RST_LEVEL (0)

#define ESP_PANEL_BOARD_LCD_VENDOR_INIT_CMD() { \
    {0x30, (uint8_t []){0x00}, 1, 0}, \
    {0xF7, (uint8_t []){0x49, 0x61, 0x02, 0x00}, 4, 0}, \
    {0x30, (uint8_t []){0x01}, 1, 0}, \
    {0x04, (uint8_t []){0x0C}, 1, 0}, \
    {0x05, (uint8_t []){0x08}, 1, 0}, \
    {0x0B, (uint8_t []){0x11}, 1, 0}, \
    {0x20, (uint8_t []){0x04}, 1, 0}, \
    {0x1F, (uint8_t []){0x00}, 1, 0}, \
    {0x23, (uint8_t []){0x38}, 1, 0}, \
    {0x28, (uint8_t []){0x18}, 1, 0}, \
    {0x29, (uint8_t []){0x29}, 1, 0}, \
    {0x2A, (uint8_t []){0x01}, 1, 0}, \
    {0x2B, (uint8_t []){0x29}, 1, 0}, \
    {0x2C, (uint8_t []){0x01}, 1, 0}, \
    {0x30, (uint8_t []){0x02}, 1, 0}, \
    {0x00, (uint8_t []){0x05}, 1, 0}, \
    {0x01, (uint8_t []){0x22}, 1, 0}, \
    {0x02, (uint8_t []){0x08}, 1, 0}, \
    {0x03, (uint8_t []){0x12}, 1, 0}, \
    {0x04, (uint8_t []){0x16}, 1, 0}, \
    {0x05, (uint8_t []){0x64}, 1, 0}, \
    {0x06, (uint8_t []){0x00}, 1, 0}, \
    {0x07, (uint8_t []){0x00}, 1, 0}, \
    {0x08, (uint8_t []){0x78}, 1, 0}, \
    {0x09, (uint8_t []){0x00}, 1, 0}, \
    {0x0A, (uint8_t []){0x04}, 1, 0}, \
    {0x0B, (uint8_t []){0x16,0x17,0x0B,0x0D,0x0D,0x0D,0x11,0x10,0x07,0x07,0x09}, 11, 0}, \
    {0x0C, (uint8_t []){0x09,0x1E,0x1E,0x1C,0x1C,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D}, 11, 0}, \
    {0x0D, (uint8_t []){0x0A,0x05,0x0B,0x0D,0x0D,0x0D,0x11,0x10,0x06,0x06,0x08}, 11, 0}, \
    {0x0E, (uint8_t []){0x08,0x1F,0x1F,0x1D,0x1D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D}, 11, 0}, \
    {0x0F, (uint8_t []){0x0A,0x05,0x0D,0x0B,0x0D,0x0D,0x11,0x10,0x1D,0x1D,0x1F}, 11, 0}, \
    {0x10, (uint8_t []){0x1F,0x08,0x08,0x06,0x06,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D}, 11, 0}, \
    {0x11, (uint8_t []){0x16,0x17,0x0D,0x0B,0x0D,0x0D,0x11,0x10,0x1C,0x1C,0x1E}, 11, 0}, \
    {0x12, (uint8_t []){0x1E,0x09,0x09,0x07,0x07,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D}, 11, 0}, \
    {0x13, (uint8_t []){0x00,0x00,0x00,0x00}, 4, 0}, \
    {0x14, (uint8_t []){0x00,0x00,0x41,0x41}, 4, 0}, \
    {0x15, (uint8_t []){0x00,0x00,0x00,0x00}, 4, 0}, \
    {0x17, (uint8_t []){0x00}, 1, 0}, \
    {0x18, (uint8_t []){0x85}, 1, 0}, \
    {0x19, (uint8_t []){0x06,0x09}, 2, 0}, \
    {0x1A, (uint8_t []){0x05,0x08}, 2, 0}, \
    {0x1B, (uint8_t []){0x0A,0x04}, 2, 0}, \
    {0x26, (uint8_t []){0x00}, 1, 0}, \
    {0x27, (uint8_t []){0x00}, 1, 0}, \
    {0x30, (uint8_t []){0x06}, 1, 0}, \
    {0x12, (uint8_t []){0x3F,0x26,0x27,0x35,0x2D,0x34,0x3F,0x3F,0x3F,0x35,0x2A,0x20,0x16,0x08}, 14, 0}, \
    {0x13, (uint8_t []){0x3F,0x26,0x28,0x35,0x27,0x29,0x29,0x2F,0x35,0x2F,0x26,0x20,0x16,0x08}, 14, 0}, \
    {0x30, (uint8_t []){0x0A}, 1, 0}, \
    {0x02, (uint8_t []){0x4F}, 1, 0}, \
    {0x0B, (uint8_t []){0x40}, 1, 0}, \
    {0x30, (uint8_t []){0x0D}, 1, 0}, \
    {0x0D, (uint8_t []){0x04}, 1, 0}, \
    {0x10, (uint8_t []){0x0C}, 1, 0}, \
    {0x11, (uint8_t []){0x0C}, 1, 0}, \
    {0x12, (uint8_t []){0x0C}, 1, 0}, \
    {0x13, (uint8_t []){0x0C}, 1, 0}, \
    {0x30, (uint8_t []){0x00}, 1, 0}, \
    {0x11, (uint8_t []){0x00}, 0, 120}, \
    {0x29, (uint8_t []){0x00}, 0, 20} \
}
#endif

#if ESP_PANEL_BOARD_USE_TOUCH
#define ESP_PANEL_BOARD_TOUCH_CONTROLLER GT911
#define ESP_PANEL_BOARD_TOUCH_BUS_TYPE (ESP_PANEL_BUS_TYPE_I2C)
#define ESP_PANEL_BOARD_TOUCH_BUS_SKIP_INIT_HOST (0)
#define ESP_PANEL_BOARD_TOUCH_I2C_HOST_ID (0)
#define ESP_PANEL_BOARD_TOUCH_I2C_CLK_HZ (400 * 1000)
#define ESP_PANEL_BOARD_TOUCH_I2C_SCL_PULLUP (1)
#define ESP_PANEL_BOARD_TOUCH_I2C_SDA_PULLUP (1)
#define ESP_PANEL_BOARD_TOUCH_I2C_IO_SCL (8)
#define ESP_PANEL_BOARD_TOUCH_I2C_IO_SDA (7)
#define ESP_PANEL_BOARD_TOUCH_I2C_ADDRESS (0) // Driver default GT911 address
#define ESP_PANEL_BOARD_TOUCH_SWAP_XY (0)
#define ESP_PANEL_BOARD_TOUCH_MIRROR_X (0)
#define ESP_PANEL_BOARD_TOUCH_MIRROR_Y (0)
#define ESP_PANEL_BOARD_TOUCH_RST_IO (20)
#define ESP_PANEL_BOARD_TOUCH_RST_LEVEL (0)
#define ESP_PANEL_BOARD_TOUCH_INT_IO (21)
#define ESP_PANEL_BOARD_TOUCH_INT_LEVEL (0)
#endif

#if ESP_PANEL_BOARD_USE_BACKLIGHT
#define ESP_PANEL_BOARD_BACKLIGHT_TYPE     (ESP_PANEL_BACKLIGHT_TYPE_PWM_LEDC)
#define ESP_PANEL_BOARD_BACKLIGHT_IO       (23)
#define ESP_PANEL_BOARD_BACKLIGHT_ON_LEVEL (1)
#define ESP_PANEL_BOARD_BACKLIGHT_IDLE_OFF (0)
#endif

#define ESP_PANEL_BOARD_CUSTOM_FILE_VERSION_MAJOR 1
#define ESP_PANEL_BOARD_CUSTOM_FILE_VERSION_MINOR 2
#define ESP_PANEL_BOARD_CUSTOM_FILE_VERSION_PATCH 0
#endif
