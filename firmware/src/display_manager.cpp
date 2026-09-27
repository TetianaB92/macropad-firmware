#include "display_manager.h"
#include "board_config.h"
#include "frame_buffer.h"
#include "theme.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <freertos/semphr.h>
#if !MACRO_PAD_HEADLESS && defined(MACRO_PAD_USE_ESP_PANEL)
#include <esp_display_panel.hpp>
#elif !MACRO_PAD_HEADLESS
#include "lgfx_device.h"
#endif

int DisplayManager::w_ = PANEL_WIDTH;
int DisplayManager::h_ = PANEL_HEIGHT;
bool DisplayManager::lvglPaused_ = false;

// expose statics via anonymous — keep in cpp
namespace {
lv_disp_draw_buf_t draw_buf;
lv_color_t* buf1 = nullptr;
lv_color_t* buf2 = nullptr;
int backlightPct = 80;

#if !MACRO_PAD_HEADLESS && defined(MACRO_PAD_USE_ESP_PANEL)
esp_panel::board::Board* board = nullptr;
static_assert(sizeof(lv_color_t) == sizeof(uint16_t), "Panel requires RGB565 LVGL pixels");
uint32_t flushCount = 0;
uint32_t presentCount = 0;
uint32_t lastFlushReport = 0;
bool scanoutArmed = false;
SemaphoreHandle_t frameDone = nullptr;
bool doubleBuffered = false;
IRAM_ATTR bool frameFinished(void*) {
  BaseType_t wake = pdFALSE;
  if (frameDone) xSemaphoreGiveFromISR(frameDone, &wake);
  return wake == pdTRUE;
}

void armScanout(esp_panel::drivers::LCD* lcd, void* fb) {
  if (scanoutArmed || !lcd || !fb) return;
  lcd->switchFrameBufferTo(fb);
  scanoutArmed = true;
}

void copyScanoutRect(esp_panel::drivers::LCD* lcd, int x, int y, int w, int h,
                     const uint16_t* pix) {
  if (!lcd || !pix || w <= 0 || h <= 0) return;
  const int fw = lcd->getFrameWidth();
  const int fh = lcd->getFrameHeight();
  if (x < 0 || y < 0 || x + w > fw || y + h > fh) return;
  auto* fb = static_cast<uint16_t*>(lcd->getFrameBufferByIndex(0));
  if (!fb) return;
  if (x == 0 && w == fw) {
    const size_t bytes = (size_t)h * (size_t)w * sizeof(uint16_t);
    memcpy(fb + (size_t)y * (size_t)fw, pix, bytes);
    psramWriteback(fb + (size_t)y * (size_t)fw, bytes);
    armScanout(lcd, fb);
    return;
  }
  const size_t rowBytes = (size_t)w * sizeof(uint16_t);
  for (int row = 0; row < h; ++row) {
    auto* dest = fb + (size_t)(y + row) * (size_t)fw + x;
    memcpy(dest, pix + (size_t)row * (size_t)w, rowBytes);
    psramWriteback(dest, rowBytes);
  }
  armScanout(lcd, fb);
}

void flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_p) {
  if (DisplayManager::isLvglPaused()) {
    lv_disp_flush_ready(drv);
    return;
  }
  auto* lcd = board ? board->getLCD() : nullptr;
  if (!lcd) {
    lv_disp_flush_ready(drv);
    return;
  }
  if (doubleBuffered) {
    // LVGL draws a complete composition into the inactive driver-owned buffer.
    // Switch only at frame completion and wait before reusing the old buffer.
    psramWriteback(color_p, size_t(lcd->getFrameWidth()) * lcd->getFrameHeight() * sizeof(lv_color_t));
    lcd->switchFrameBufferTo(color_p);
    while (xSemaphoreTake(frameDone, 0) == pdTRUE) {}
    xSemaphoreTake(frameDone, portMAX_DELAY);
    lv_disp_flush_ready(drv);
    return;
  }
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;
  // Copy completed LVGL strips into the same driver-owned scan-out buffer.
  // This is synchronous: flush_ready cannot race an asynchronous DMA reader.
  auto* fb = static_cast<uint16_t*>(lcd->getFrameBufferByIndex(0));
  if (fb && area->x1 >= 0 && area->y1 >= 0 &&
      area->x2 < lcd->getFrameWidth() && area->y2 < lcd->getFrameHeight()) {
    const size_t stride = lcd->getFrameWidth();
    for (uint32_t row = 0; row < h; ++row) {
      auto* dest = fb + (area->y1 + row) * stride + area->x1;
      memcpy(dest, color_p + row * w, w * sizeof(lv_color_t));
      psramWriteback(dest, w * sizeof(lv_color_t));
    }
  }
  else {
    static bool reported = false;
    if (!reported) {
      Serial.printf("LCD flush rejected fb=%p area=%d,%d..%d,%d\n", fb,
                    area->x1, area->y1, area->x2, area->y2);
      reported = true;
    }
  }
  ++flushCount;
  if (fb && lv_disp_flush_is_last(drv)) {
    // The DPI controller is already scanning fb0. Switching to the same
    // pointer on every overlay tick restarted the panel timing and showed up
    // as random full-screen flicker after the dashboard looked correct.
    armScanout(lcd, fb);
    ++presentCount;
    const uint32_t now = millis();
    if (presentCount <= 3 || now - lastFlushReport >= 5000) {
      lastFlushReport = now;
      Serial.printf("LCD present=%lu strips=%lu tick=%lu fb=%p samples=%04x,%04x,%04x\n",
                    (unsigned long)presentCount, (unsigned long)flushCount,
                    (unsigned long)lv_tick_get(), fb,
                    fb[0], fb[(size_t)lcd->getFrameWidth() * lcd->getFrameHeight() / 2],
                    fb[(size_t)lcd->getFrameWidth() * lcd->getFrameHeight() - 1]);
    }
  }
  lv_disp_flush_ready(drv);
}

void panel_blit_rgb565(int x, int y, int w, int h, const uint16_t* pix) {
  auto* lcd = board ? board->getLCD() : nullptr;
  if (!lcd || !pix || w <= 0 || h <= 0) return;

  const size_t bytes = (size_t)w * h * sizeof(uint16_t);
  psramInvalidate(pix, bytes);

  copyScanoutRect(lcd, x, y, w, h, pix);
}

void touch_read(lv_indev_drv_t* drv, lv_indev_data_t* data) {
  (void)drv;
  auto* touch = board ? board->getTouch() : nullptr;
  if (!touch) {
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }

  esp_panel::drivers::TouchPoint points[esp_panel::drivers::Touch::POINTS_MAX_NUM];
  int count =
      touch->readPoints(points, esp_panel::drivers::Touch::POINTS_MAX_NUM, 0);
  static bool wasPressed = false;
  if (count > 0) {
    if (!wasPressed)
      Serial.printf("Touch down x=%d y=%d\n", points[0].x, points[0].y);
    wasPressed = true;
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = points[0].x;
    data->point.y = points[0].y;
  } else {
    wasPressed = false;
    data->state = LV_INDEV_STATE_RELEASED;
  }
}
#elif !MACRO_PAD_HEADLESS
void flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_p) {
  if (doubleBuffered) {
    // LVGL draws a complete composition into the inactive driver-owned buffer.
    // Switch only at frame completion and wait before reusing the old buffer.
    psramWriteback(color_p, size_t(lcd->getFrameWidth()) * lcd->getFrameHeight() * sizeof(lv_color_t));
    lcd->switchFrameBufferTo(color_p);
    while (xSemaphoreTake(frameDone, 0) == pdTRUE) {}
    xSemaphoreTake(frameDone, portMAX_DELAY);
    lv_disp_flush_ready(drv);
    return;
  }
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;
  auto& gfx = lcd();
  gfx.startWrite();
  gfx.setAddrWindow(area->x1, area->y1, w, h);
  gfx.pushPixels(reinterpret_cast<uint16_t*>(color_p), w * h);
  gfx.endWrite();
  lv_disp_flush_ready(drv);
}

void touch_read(lv_indev_drv_t* drv, lv_indev_data_t* data) {
  (void)drv;
  lgfx::touch_point_t tp;
  auto& gfx = lcd();
  if (gfx.getTouch(&tp)) {
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = tp.x;
    data->point.y = tp.y;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}
#else
void flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_p) {
  (void)area;
  (void)color_p;
  lv_disp_flush_ready(drv);
}
#endif
}  // namespace

// Need width_/height_ as class statics — declared in header private
int DisplayManager::width() { return w_; }
int DisplayManager::height() { return h_; }

void DisplayManager::setLogicalSize(int w, int h) {
  w_ = w;
  h_ = h;
}

void DisplayManager::setBacklightPercent(int pct) {
  backlightPct = constrain(pct, 5, 100);
#if !MACRO_PAD_HEADLESS && defined(MACRO_PAD_USE_ESP_PANEL)
  if (board && board->getBacklight()) {
    board->getBacklight()->setBrightness(backlightPct);
  }
#elif !MACRO_PAD_HEADLESS
  lcd().setBrightness((uint8_t)map(backlightPct, 0, 100, 0, 255));
#endif
}

void DisplayManager::drawRgb565(int x, int y, int w, int h,
                                const uint16_t* pix) {
#if !MACRO_PAD_HEADLESS && defined(MACRO_PAD_USE_ESP_PANEL)
  panel_blit_rgb565(x, y, w, h, pix);
#elif !MACRO_PAD_HEADLESS
  if (!pix || w <= 0 || h <= 0) return;
  auto& gfx = lcd();
  gfx.pushImage(x, y, w, h, pix);
#else
  (void)x;
  (void)y;
  (void)w;
  (void)h;
  (void)pix;
#endif
}

void DisplayManager::fillColor(uint16_t rgb565) {
#if !MACRO_PAD_HEADLESS && defined(MACRO_PAD_USE_ESP_PANEL)
  auto* lcd = board ? board->getLCD() : nullptr;
  if (!lcd) return;
  auto* fb = static_cast<uint16_t*>(lcd->getFrameBufferByIndex(0));
  if (!fb) return;
  const size_t count = (size_t)lcd->getFrameWidth() * lcd->getFrameHeight();
  for (size_t i = 0; i < count; ++i) fb[i] = rgb565;
  psramWriteback(fb, count * sizeof(uint16_t));
  armScanout(lcd, fb);
#else
  (void)rgb565;
#endif
}

void DisplayManager::drawRgb565Diff(int w, int h, const uint16_t* next,
                                    const uint16_t* previous) {
#if !MACRO_PAD_HEADLESS && defined(MACRO_PAD_USE_ESP_PANEL)
  if (!next || w <= 0 || h <= 0) return;
  if (!previous) {
    drawRgb565(0, 0, w, h, next);
    return;
  }
  const size_t rowBytes = (size_t)w * sizeof(uint16_t);
  int y = 0;
  int copied = 0;
  while (y < h) {
    while (y < h && memcmp(previous + (size_t)y * w, next + (size_t)y * w, rowBytes) == 0)
      ++y;
    const int start = y;
    while (y < h && memcmp(previous + (size_t)y * w, next + (size_t)y * w, rowBytes) != 0)
      ++y;
    if (y > start) {
      drawRgb565(0, start, w, y - start, next + (size_t)start * w);
      copied += y - start;
    }
  }
  if (copied)
    Serial.printf("DisplayManager: chrome diff rows=%d/%d\n", copied, h);
#else
  (void)w;
  (void)h;
  (void)next;
  (void)previous;
#endif
}

void DisplayManager::setLvglPaused(bool paused) { lvglPaused_ = paused; }

bool DisplayManager::isLvglPaused() { return lvglPaused_; }

void DisplayManager::init() {
  lv_init();

#if !MACRO_PAD_HEADLESS && defined(MACRO_PAD_USE_ESP_PANEL)
  // Power on LCD matrix for Viewe UEP4S070H1024V600C-WBA
  pinMode(4, OUTPUT);
  pinMode(5, OUTPUT);
  digitalWrite(4, HIGH);
  delay(30);
  digitalWrite(5, HIGH);
  delay(30);

  board = new esp_panel::board::Board();
  if (!board->init()) {
    Serial.println("DisplayManager: ESP32_Display_Panel init failed");
  } else {
    auto* lcd = board->getLCD();
    if (lcd) {
      // Avoid updating pixels while the MIPI controller scans them out.
      lcd->configFrameBufferNumber(2);
    }
    if (!board->begin()) {
      Serial.println("DisplayManager: ESP32_Display_Panel begin failed");
    } else {
      if (lcd) {
        Serial.printf("LCD runtime: %dx%d bpp=%d lv_color=%u framebuffer=%p touch=%d\n",
                      lcd->getFrameWidth(), lcd->getFrameHeight(),
                      lcd->getFrameColorBits(), (unsigned)sizeof(lv_color_t),
                      lcd->getFrameBufferByIndex(0), board->getTouch() != nullptr);
        frameDone = xSemaphoreCreateBinary();
        doubleBuffered = frameDone && lcd->getFrameBufferByIndex(1) && lcd->attachRefreshFinishCallback(frameFinished);
        Serial.printf("LCD double buffering=%d\n", int(doubleBuffered));
        bool displayOn = lcd->setDisplayOnOff(true);
        Serial.printf("DisplayManager: setDisplayOnOff(true)=%d\n",
                      (int)displayOn);
      }
    }
  }

  if (board && board->getBacklight()) {
    bool backlightOn = board->getBacklight()->on();
    Serial.printf("DisplayManager: backlight->on()=%d\n", (int)backlightOn);
    board->getBacklight()->setBrightness(backlightPct);
    Serial.printf("DisplayManager: backlight brightness=%d\n", backlightPct);
  }
  if (board && board->getLCD()) {
    w_ = board->getLCD()->getFrameWidth();
    h_ = board->getLCD()->getFrameHeight();
  }
#elif !MACRO_PAD_HEADLESS
  auto& gfx = lcd();
  gfx.init();
  gfx.setRotation(0);
  gfx.setBrightness(map(backlightPct, 0, 100, 0, 255));
  w_ = gfx.width();
  h_ = gfx.height();
#endif

  size_t n = (size_t)w_ * 48;
#if !MACRO_PAD_HEADLESS && defined(MACRO_PAD_USE_ESP_PANEL)
  if (doubleBuffered) {
    n = size_t(w_) * h_;
    // Start on fb1, while the controller initially scans fb0.
    buf1 = static_cast<lv_color_t*>(board->getLCD()->getFrameBufferByIndex(1));
    buf2 = static_cast<lv_color_t*>(board->getLCD()->getFrameBufferByIndex(0));
  } else {
    buf1 = (lv_color_t*)heap_caps_malloc(n * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    buf2 = (lv_color_t*)heap_caps_malloc(n * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  }
#else
  buf1 = (lv_color_t*)heap_caps_malloc(n * sizeof(lv_color_t),
                                       MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (!buf1)
    buf1 = (lv_color_t*)heap_caps_malloc(n * sizeof(lv_color_t),
                                         MALLOC_CAP_SPIRAM);
  buf2 = (lv_color_t*)heap_caps_malloc(n * sizeof(lv_color_t),
                                       MALLOC_CAP_SPIRAM);
#endif
  if (!buf1) buf1 = (lv_color_t*)malloc(n * sizeof(lv_color_t));
  lv_disp_draw_buf_init(&draw_buf, buf1, buf2, n);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = w_;
  disp_drv.ver_res = h_;
  disp_drv.flush_cb = flush_cb;
  disp_drv.draw_buf = &draw_buf;
#if !MACRO_PAD_HEADLESS && defined(MACRO_PAD_USE_ESP_PANEL)
  disp_drv.full_refresh = doubleBuffered ? 1 : 0;
#endif
  lv_disp_drv_register(&disp_drv);

#if !MACRO_PAD_HEADLESS
  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = touch_read;
  if (
#if defined(MACRO_PAD_USE_ESP_PANEL)
      board && board->getTouch()
#else
      true
#endif
  ) {
    lv_indev_drv_register(&indev_drv);
  }
#endif

  lv_obj_t* scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, Theme::bg(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(scr, 0, 0);
  lv_obj_set_style_outline_width(scr, 0, 0);
  lv_obj_set_style_pad_all(scr, 0, 0);
  lv_obj_set_style_radius(scr, 0, 0);

  Serial.printf("DisplayManager: %dx%d headless=%d touch=%d\n", w_, h_,
                MACRO_PAD_HEADLESS, !MACRO_PAD_HEADLESS);
}

void DisplayManager::loop() {
  if (!lvglPaused_) lv_timer_handler();
}
