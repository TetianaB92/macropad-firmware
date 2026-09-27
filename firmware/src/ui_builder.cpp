#include "ui_builder.h"
#include "board_config.h"
#include "device_state.h"
#include "display_manager.h"
#include "frame_buffer.h"
#include "geometry.h"
#include "media_pipeline.h"
#include "theme.h"


// LVGL 8.3 handles TJPG differently depending on build flags
#if defined(LV_USE_SJPG) && LV_USE_SJPG
#include <src/extra/libs/sjpg/lv_sjpg.h>
#include <src/extra/libs/sjpg/tjpgd.h>
#else
// Fallback if lv_sjpg is not used, try to include the standalone decoder
#include <tjpgd.h>
// Provide empty implementations if JPEG decoder is missing in current environment
#ifndef JDR_OK
#define JDR_OK 0
void lv_split_jpeg_init() {}
#endif
#endif

#include <esp_heap_caps.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <Arduino.h>

lv_obj_t* UIBuilder::root_ = nullptr;
lv_obj_t* UIBuilder::overlay_ = nullptr;
lv_obj_t* UIBuilder::screenImage_ = nullptr;
uint8_t* UIBuilder::screenImageBytes_ = nullptr;
lv_img_dsc_t UIBuilder::screenImageDsc_ = {};
uint32_t UIBuilder::lastLocalTick_ = 0;
SDUIEngine UIBuilder::sduiEngine_;

struct WidgetRef {
  char id[48];
  char type[24];
  lv_obj_t* body;
  lv_obj_t* bar;
  lv_obj_t* bar2;
  lv_obj_t* img;
  int timerSecLeft;
  int stretchEveryMin;
  uint32_t stretchLastMs;
};
static WidgetRef g_refs[64];
static int g_refCount = 0;

namespace {
struct JpegMemSource {
  const uint8_t* data;
  size_t len;
  size_t pos;
  uint16_t* rgb565;
  int width;
  int height;
};

constexpr size_t kTjpgdWorkbufSize = 4096;

size_t rendered_jpeg_input(JDEC* jd, uint8_t* buf, size_t len) {
  auto* src = static_cast<JpegMemSource*>(jd->device);
  if (!src) return 0;
  const size_t remaining = (src->pos < src->len) ? (src->len - src->pos) : 0;
  const size_t toRead = remaining < len ? remaining : len;
  if (buf && toRead > 0) {
    memcpy(buf, src->data + src->pos, toRead);
  }
  src->pos += toRead;
  return toRead;
}

int rendered_jpeg_output(JDEC* jd, void* bitmap, JRECT* rect) {
  auto* src = static_cast<JpegMemSource*>(jd->device);
  if (!src || !bitmap || !src->rgb565 || !rect) return 0;

  const int rectW = rect->right - rect->left + 1;
#if JD_FORMAT == 1
  auto* pixels = static_cast<const uint16_t*>(bitmap);
#else
  auto* pixels = static_cast<const uint8_t*>(bitmap);
#endif
  for (int y = rect->top; y <= rect->bottom; ++y) {
    if (y < 0 || y >= src->height) continue;
    const int row = y - rect->top;
    uint16_t* dst = src->rgb565 + y * src->width + rect->left;
#if JD_FORMAT == 1
    const uint16_t* src_pixels = pixels + row * rectW;
    memcpy(dst, src_pixels, (size_t)rectW * sizeof(uint16_t));
#else
    const uint8_t* src_pixels = pixels + ((size_t)row * rectW * 3);
    for (int x = 0; x < rectW; ++x) {
      const uint8_t r = src_pixels[x * 3 + 0];
      const uint8_t g = src_pixels[x * 3 + 1];
      const uint8_t b = src_pixels[x * 3 + 2];
      dst[x] = (uint16_t)(((r & 0xF8) << 8) |
                          ((g & 0xFC) << 3) |
                          (b >> 3));
    }
#endif
  }
  return 1;
}
}  // namespace

static void remember(WidgetRef r) {
  if (g_refCount >= 64) return;
  g_refs[g_refCount++] = r;
}

void UIBuilder::init() {
  lv_split_jpeg_init();
  if (root_) return;

  root_ = lv_obj_create(lv_scr_act());
  lv_obj_set_size(root_, DisplayManager::width(), DisplayManager::height());
  lv_obj_set_pos(root_, 0, 0);
  lv_obj_set_style_bg_color(root_, lv_color_hex(0x0c0c0e), 0);
  lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(root_, 0, 0);
  lv_obj_set_style_outline_width(root_, 0, 0);
  lv_obj_set_style_shadow_width(root_, 0, 0);
  lv_obj_set_style_pad_all(root_, 0, 0);
  lv_obj_set_style_radius(root_, 0, 0);
  lv_obj_set_scrollbar_mode(root_, LV_SCROLLBAR_MODE_OFF);
  lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

  sduiEngine_.init(root_);
}

bool UIBuilder::loadSDUIScene(const char* jsonScene, uint32_t generation) {
  // Hybrid rendering: LVGL owns the frame and composites the chrome image with
  // the live overlays, so it must not be paused (that mode is only for the
  // legacy full-screen bake path).
  DisplayManager::setLvglPaused(false);
  return sduiEngine_.parseScene(jsonScene, generation);
}

SDUIEngine& UIBuilder::getSDUIEngine() {
  return sduiEngine_;
}

void UIBuilder::clearUi() {
  MediaPipeline::detachAll();
  g_refCount = 0;
  if (overlay_) {
    lv_obj_del(overlay_);
    overlay_ = nullptr;
  }
  lv_obj_clean(lv_scr_act());
  if (screenImageBytes_) {
    lv_img_cache_invalidate_src(&screenImageDsc_);
    releaseFrameBuffer(screenImageBytes_);
    screenImageBytes_ = nullptr;
  }
  memset(&screenImageDsc_, 0, sizeof(screenImageDsc_));
  screenImage_ = nullptr;
  root_ = lv_scr_act();
  lv_disp_set_bg_image(lv_disp_get_default(), nullptr);
  lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(root_, Theme::bg(), 0);
  // lv_obj_clean() above freed every object the SDUI engine created. Drop its
  // stale bindings and re-point it at the new root so tick()/parseScene() never
  // dereference freed memory (this was the P4 reboot loop after "Connecting…").
  sduiEngine_.attachHost(root_);
}

void UIBuilder::styleTile(lv_obj_t* tile) {
  lv_obj_set_style_bg_color(tile, Theme::panel(), 0);
  lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(tile, 16, 0);
  lv_obj_set_style_border_width(tile, 1, 0);
  lv_obj_set_style_border_color(tile, Theme::border(), 0);
  lv_obj_set_style_pad_all(tile, 10, 0);
  lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t* UIBuilder::makeTile(lv_obj_t* parent, int px, int py, int pw,
                              int ph) {
  lv_obj_t* tile = lv_obj_create(parent);
  lv_obj_set_pos(tile, px, py);
  lv_obj_set_size(tile, pw, ph);
  styleTile(tile);
  return tile;
}

lv_obj_t* UIBuilder::addTitle(lv_obj_t* tile, const char* title) {
  lv_obj_t* lab = lv_label_create(tile);
  lv_label_set_text(lab, title);
  lv_obj_set_style_text_color(lab, Theme::accent(), 0);
  lv_obj_set_style_text_font(lab, &lv_font_montserrat_12, 0);
  lv_obj_align(lab, LV_ALIGN_TOP_LEFT, 0, 0);
  return lab;
}

lv_obj_t* UIBuilder::addBody(lv_obj_t* tile, const char* text) {
  lv_obj_t* lab = lv_label_create(tile);
  lv_label_set_text(lab, text);
  lv_label_set_long_mode(lab, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(lab, lv_pct(100));
  lv_obj_set_style_text_color(lab, Theme::text(), 0);
  lv_obj_set_style_text_font(lab, &lv_font_montserrat_14, 0);
  lv_obj_align(lab, LV_ALIGN_TOP_LEFT, 0, 18);
  return lab;
}

lv_obj_t* UIBuilder::addBar(lv_obj_t* tile, int y) {
  lv_obj_t* bar = lv_bar_create(tile);
  lv_obj_set_size(bar, lv_pct(100), 10);
  lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, y);
  lv_bar_set_range(bar, 0, 100);
  lv_bar_set_value(bar, 0, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(bar, Theme::border(), LV_PART_MAIN);
  lv_obj_set_style_bg_color(bar, Theme::accent(), LV_PART_INDICATOR);
  lv_obj_set_style_radius(bar, 6, LV_PART_MAIN);
  lv_obj_set_style_radius(bar, 6, LV_PART_INDICATOR);
  return bar;
}

void UIBuilder::showBoot(const char* msg) {
  DisplayManager::setLvglPaused(false);
  clearUi();
  lv_obj_t* lab = lv_label_create(root_);
  lv_label_set_text(lab, msg ? msg : "Boot");
  lv_obj_set_style_text_color(lab, Theme::muted(), 0);
  lv_obj_center(lab);
}

void UIBuilder::showProvisioning(const char* apName) {
  DisplayManager::setLvglPaused(false);
  clearUi();
  lv_obj_t* t1 = lv_label_create(root_);
  lv_label_set_text(t1, "Setup Wi-Fi");
  lv_obj_set_style_text_color(t1, Theme::accent(), 0);
  lv_obj_set_style_text_font(t1, &lv_font_montserrat_28, 0);
  lv_obj_align(t1, LV_ALIGN_CENTER, 0, -70);

  lv_obj_t* t2 = lv_label_create(root_);
  lv_label_set_text(t2, "On your phone connect to:");
  lv_obj_set_style_text_color(t2, Theme::muted(), 0);
  lv_obj_align(t2, LV_ALIGN_CENTER, 0, -20);

  lv_obj_t* ap = lv_label_create(root_);
  lv_label_set_text(ap, apName && apName[0] ? apName : "Macropad-Setup");
  lv_obj_set_style_text_color(ap, Theme::text(), 0);
  lv_obj_set_style_text_font(ap, &lv_font_montserrat_14, 0);
  lv_obj_align(ap, LV_ALIGN_CENTER, 0, 10);

  lv_obj_t* t3 = lv_label_create(root_);
  lv_label_set_text(t3, "Open the captive page → enter home Wi-Fi");
  lv_obj_set_style_text_color(t3, Theme::muted(), 0);
  lv_obj_align(t3, LV_ALIGN_CENTER, 0, 50);
}

void UIBuilder::showPairing(const char* code) {
  DisplayManager::setLvglPaused(false);
  clearUi();
  lv_obj_t* hint = lv_label_create(root_);
  lv_label_set_text(hint, "Open /app/devices and enter code");
  lv_obj_set_style_text_color(hint, Theme::muted(), 0);
  lv_obj_align(hint, LV_ALIGN_CENTER, 0, -50);

  lv_obj_t* codeLab = lv_label_create(root_);
  char buf[32];
  snprintf(buf, sizeof(buf), "%s", code && code[0] ? code : "------");
  lv_label_set_text(codeLab, buf);
  lv_obj_set_style_text_color(codeLab, Theme::accent(), 0);
  lv_obj_set_style_text_font(codeLab, &lv_font_montserrat_28, 0);
  lv_obj_align(codeLab, LV_ALIGN_CENTER, 0, 0);

  lv_obj_t* mac = lv_label_create(root_);
  char macBuf[48];
  snprintf(macBuf, sizeof(macBuf), "MAC %s", DeviceState::mac());
  lv_label_set_text(mac, macBuf);
  lv_obj_set_style_text_color(mac, Theme::muted(), 0);
  lv_obj_align(mac, LV_ALIGN_CENTER, 0, 50);
}

void UIBuilder::showPairedSuccess() {
  DisplayManager::setLvglPaused(false);
  clearUi();

  lv_obj_t* title = lv_label_create(root_);
  lv_label_set_text(title, "Successfully connected!");
  lv_obj_set_style_text_color(title, Theme::accent(), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
  lv_obj_align(title, LV_ALIGN_CENTER, 0, -18);

  lv_obj_t* hint = lv_label_create(root_);
  lv_label_set_text(hint, "Loading layout from your account…");
  lv_obj_set_style_text_color(hint, Theme::muted(), 0);
  lv_obj_align(hint, LV_ALIGN_CENTER, 0, 24);
}

bool UIBuilder::showRenderedScreen(const uint8_t* imageBytes, size_t imageLen) {
  if (!imageBytes || imageLen < 100) return false;

  // #region debug-point A:jpeg-magic
  Serial.printf(
      "[DEBUG] rendered-screen: len=%u magic=%02X %02X %02X %02X %02X %02X %02X %02X\n",
      (unsigned)imageLen, imageBytes[0], imageBytes[1], imageBytes[2],
      imageBytes[3], imageBytes[4], imageBytes[5], imageBytes[6], imageBytes[7]);
  // #endregion

  uint8_t* workbuf =
      static_cast<uint8_t*>(malloc(kTjpgdWorkbufSize));
  JDEC decoder {};
  JpegMemSource source {
      .data = imageBytes,
      .len = imageLen,
      .pos = 0,
      .rgb565 = nullptr,
      .width = PANEL_WIDTH,
      .height = PANEL_HEIGHT,
  };
  if (!workbuf) {
    Serial.println("Rendered screen workbuf alloc failed");
    return false;
  }
  JRESULT prep = jd_prepare(&decoder, rendered_jpeg_input, workbuf,
                            kTjpgdWorkbufSize, &source);
  if (prep != JDR_OK) {
    free(workbuf);
    // #region debug-point B:jd-prepare-failed
    Serial.printf("Rendered screen jd_prepare failed rc=%d\n", (int)prep);
    // #endregion
    return false;
  }

  source.width = decoder.width;
  source.height = decoder.height;
  const size_t rgbBytes =
      (size_t)source.width * source.height * sizeof(uint16_t);
  source.rgb565 = static_cast<uint16_t*>(heap_caps_malloc(
      rgbBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!source.rgb565) {
    source.rgb565 = static_cast<uint16_t*>(malloc(rgbBytes));
  }
  if (!source.rgb565) {
    free(workbuf);
    Serial.println("Rendered screen rgb alloc failed");
    return false;
  }
  memset(source.rgb565, 0, rgbBytes);

  JRESULT dec = jd_decomp(&decoder, rendered_jpeg_output, 0);
  if (dec != JDR_OK) {
    releaseFrameBuffer(reinterpret_cast<uint8_t*>(source.rgb565));
    free(workbuf);
    // #region debug-point C:jd-decomp-failed
    Serial.printf("Rendered screen jd_decomp failed rc=%d size=%dx%d\n",
                  (int)dec, source.width, source.height);
    // #endregion
    return false;
  }
  free(workbuf);

  clearUi();
  releaseFrameBuffer(const_cast<uint8_t*>(imageBytes));

  screenImageBytes_ = reinterpret_cast<uint8_t*>(source.rgb565);
  DisplayManager::drawRgb565(0, 0, source.width, source.height, source.rgb565);
  Serial.printf("Rendered screen drawn directly %dx%d bytes=%u\n",
                source.width, source.height, (unsigned)imageLen);
  return true;
}

bool UIBuilder::showRenderedScreenRgb565(uint8_t* imageBytes, size_t imageLen) {
  const size_t expectedBytes =
      (size_t)PANEL_WIDTH * PANEL_HEIGHT * sizeof(uint16_t);
  if (!imageBytes || imageLen != expectedBytes) {
    Serial.printf("Rendered RGB565 size mismatch got=%u expected=%u\n",
                  (unsigned)imageLen, (unsigned)expectedBytes);
    return false;
  }

  DisplayManager::setLvglPaused(true);
  clearUi();
  DisplayManager::drawRgb565(0, 0, PANEL_WIDTH, PANEL_HEIGHT,
                             reinterpret_cast<const uint16_t*>(imageBytes));
  releaseFrameBuffer(imageBytes);
  screenImageBytes_ = nullptr;
  Serial.printf("Rendered screen drawn raw RGB565 %dx%d bytes=%u\n",
                PANEL_WIDTH, PANEL_HEIGHT, (unsigned)imageLen);
  return true;
}

bool UIBuilder::applyChromeRgb565(uint8_t* imageBytes, size_t imageLen, const char* pageId, uint32_t generation) {
  const size_t expectedBytes =
      (size_t)PANEL_WIDTH * PANEL_HEIGHT * sizeof(uint16_t);
  if (!imageBytes || imageLen != expectedBytes) {
    Serial.printf("Chrome RGB565 size mismatch got=%u expected=%u\n",
                  (unsigned)imageLen, (unsigned)expectedBytes);
    return false;
  }
  // Keep LVGL running: the bake becomes the background image under the overlays.
  DisplayManager::setLvglPaused(false);
  sduiEngine_.setChromeImage(imageBytes, PANEL_WIDTH, PANEL_HEIGHT, pageId, generation);
  return true;
}

void UIBuilder::buildWidget(lv_obj_t* parent, JsonObjectConst widget,
                            int panelW, int panelH, int cols, int rows) {
  ScreenGeom g = makeGeom(panelW, panelH, cols, rows);
  CellRect r = cellToPx(g, widget["x"] | 0, widget["y"] | 0, widget["w"] | 1,
                        widget["h"] | 1);

  const char* type = widget["type"] | "unknown";
  const char* id = widget["id"] | "";
  JsonObjectConst cfg = widget["config"].as<JsonObjectConst>();

  lv_obj_t* tile = makeTile(parent, r.x, r.y, r.w, r.h);
  WidgetRef ref{};
  strncpy(ref.id, id, sizeof(ref.id) - 1);
  strncpy(ref.type, type, sizeof(ref.type) - 1);
  ref.timerSecLeft = -1;
  ref.stretchEveryMin = 0;
  ref.stretchLastMs = millis();

  if (strcmp(type, "weather") == 0) {
    addTitle(tile, "Weather");
    const char* city = cfg["city"] | "City";
    char buf[64];
    snprintf(buf, sizeof(buf), "%s\n— °C", city);
    ref.body = addBody(tile, buf);
  } else if (strcmp(type, "flipper_clock") == 0) {
    addTitle(tile, "Clock");
    ref.body = addBody(tile, "--:--:--");
    lv_obj_set_style_text_font(ref.body, &lv_font_montserrat_28, 0);
  } else if (strcmp(type, "timer") == 0) {
    addTitle(tile, "Timer");
    int mins = cfg["minutes"] | 5;
    ref.timerSecLeft = mins * 60;
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:00", mins);
    ref.body = addBody(tile, buf);
    ref.bar = addBar(tile, r.h > 80 ? 70 : 55);
    lv_bar_set_range(ref.bar, 0, ref.timerSecLeft > 0 ? ref.timerSecLeft : 1);
    lv_bar_set_value(ref.bar, ref.timerSecLeft, LV_ANIM_OFF);
  } else if (strcmp(type, "stretch_reminder") == 0) {
    addTitle(tile, "Stretch");
    ref.stretchEveryMin = cfg["intervalMinutes"] | 30;
    const char* msg = cfg["message"] | "Time to stretch";
    ref.body = addBody(tile, msg);
    ref.bar = addBar(tile, 70);
  } else if (strcmp(type, "month_calendar") == 0) {
    addTitle(tile, "Calendar");
    time_t t = time(nullptr);
    struct tm* tm = localtime(&t);
    char buf[48];
    if (tm && t > 100000)
      snprintf(buf, sizeof(buf), "%04d-%02d\nDay %d", tm->tm_year + 1900,
               tm->tm_mon + 1, tm->tm_mday);
    else
      snprintf(buf, sizeof(buf), "Month view");
    ref.body = addBody(tile, buf);
  } else if (strcmp(type, "calendar") == 0) {
    addTitle(tile, "Agenda");
    ref.body = addBody(tile, "Loading…");
  } else if (strcmp(type, "tasks") == 0) {
    addTitle(tile, "Tasks");
    ref.body = addBody(tile, "Loading…");
  } else if (strcmp(type, "finance") == 0) {
    addTitle(tile, "Finance");
    ref.body = addBody(tile, "USD / EUR");
    ref.bar = addBar(tile, 62);
    ref.bar2 = addBar(tile, 78);
  } else if (strcmp(type, "fit") == 0) {
    addTitle(tile, "Fit");
    ref.body = addBody(tile, "Steps…");
    ref.bar = addBar(tile, 70);
    lv_bar_set_range(ref.bar, 0, 10000);
  } else if (strcmp(type, "pc_monitor") == 0) {
    addTitle(tile, "PC");
    ref.body = addBody(tile, "CPU / RAM");
    ref.bar = addBar(tile, 58);
    ref.bar2 = addBar(tile, 74);
  } else if (strcmp(type, "photos_ambient") == 0) {
    addTitle(tile, "Photos");
    ref.img = lv_img_create(tile);
    lv_obj_set_size(ref.img, lv_pct(100), lv_pct(78));
    lv_obj_align(ref.img, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(ref.img, 10, 0);
    lv_obj_set_style_clip_corner(ref.img, true, 0);
    ref.body = addBody(tile, "Loading album…");

    const char* urls[8] = {};
    int n = 0;
    if (cfg["photoUrls"].is<JsonArrayConst>()) {
      JsonArrayConst arr = cfg["photoUrls"].as<JsonArrayConst>();
      for (JsonVariantConst v : arr) {
        if (n >= 8) break;
        urls[n++] = v.as<const char*>();
      }
    }
    uint32_t rot = (uint32_t)((cfg["rotateSeconds"] | 12) * 1000);
    if (n > 0) {
      MediaPipeline::attachAlbum(ref.img, urls, n, rot);
      lv_label_set_text(ref.body, "");
    }
  } else if (strcmp(type, "camera") == 0) {
    const char* title = cfg["title"] | "Camera";
    addTitle(tile, title);
    ref.img = lv_img_create(tile);
    lv_obj_set_size(ref.img, lv_pct(100), lv_pct(78));
    lv_obj_align(ref.img, LV_ALIGN_BOTTOM_MID, 0, 0);
    const char* url = cfg["url"] | "";
    ref.body = addBody(tile, url[0] ? "Streaming…" : "No URL");
    if (url[0]) MediaPipeline::attach(ref.img, url, true);
  } else if (strcmp(type, "media_control") == 0) {
    addTitle(tile, "Media");
    const char* t = cfg["title"] | "Now playing";
    const char* a = cfg["artist"] | "";
    char buf[96];
    snprintf(buf, sizeof(buf), "%s\n%s", t, a);
    ref.body = addBody(tile, buf);
    ref.bar = addBar(tile, 70);
    int dur = cfg["durationSec"] | 100;
    int pos = cfg["positionSec"] | 0;
    lv_bar_set_range(ref.bar, 0, dur > 0 ? dur : 1);
    lv_bar_set_value(ref.bar, pos, LV_ANIM_OFF);
  } else if (strcmp(type, "button") == 0) {
    addTitle(tile, "Button");
    lv_obj_t* btn = lv_btn_create(tile);
    lv_obj_set_size(btn, lv_pct(100), 40);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(btn, Theme::accent(), 0);
    lv_obj_t* lab = lv_label_create(btn);
    lv_label_set_text(lab, cfg["label"] | "Action");
    lv_obj_center(lab);
    ref.body = lab;
  } else {
    addTitle(tile, type);
    ref.body = addBody(tile, id);
  }

  remember(ref);
}

void UIBuilder::buildFromLayout(JsonDocument& layout) {
  clearUi();

  const char* preset = layout["screen"] | "7inch_wvga";
  int panelW = PANEL_WIDTH, panelH = PANEL_HEIGHT;
  resolvePresetSize(preset, &panelW, &panelH);
  DisplayManager::setLogicalSize(panelW, panelH);

  JsonArray screens = layout["screens"].as<JsonArray>();
  if (screens.isNull() || screens.size() == 0) {
    showBoot("Empty layout");
    return;
  }

  lv_obj_t* tv = lv_tileview_create(root_);
  lv_obj_set_size(tv, panelW, panelH);
  lv_obj_set_style_bg_color(tv, Theme::bg(), 0);

  int si = 0;
  for (JsonObjectConst screen : screens) {
    int cols = screen["grid"]["cols"] | 4;
    int rows = screen["grid"]["rows"] | 3;
    lv_obj_t* page =
        lv_tileview_add_tile(tv, si, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
    lv_obj_set_style_bg_color(page, Theme::bg(), 0);

    JsonArrayConst widgets = screen["widgets"].as<JsonArrayConst>();
    for (JsonObjectConst w : widgets) {
      buildWidget(page, w, panelW, panelH, cols, rows);
    }
    si++;
  }

  const char* activeId = layout["activeScreenId"] | "";
  if (activeId[0]) {
    int idx = 0;
    for (JsonObjectConst screen : screens) {
      if (strcmp(screen["id"] | "", activeId) == 0) {
        lv_obj_set_tile_id(tv, idx, 0, LV_ANIM_OFF);
        break;
      }
      idx++;
    }
  }

  Serial.printf("UIBuilder: %d screens, %d widgets\n", si, g_refCount);
}

void UIBuilder::updateLiveData(JsonDocument& data) {
  for (int i = 0; i < g_refCount; i++) {
    WidgetRef& r = g_refs[i];
    char buf[192];
    buf[0] = 0;

    if (strcmp(r.type, "finance") == 0 && !data["finance"].isNull()) {
      JsonObjectConst finance = data["finance"].as<JsonObjectConst>();
      double usd = finance["fx"]["usdUah"] | finance["usdUah"] | 0.0;
      double eur = finance["fx"]["eurUah"] | finance["eurUah"] | 0.0;
      double btc = finance["crypto"]["btcUsd"] | 0.0;
      snprintf(buf, sizeof(buf), "USD %.2f  EUR %.2f\nBTC $%.0f", usd, eur,
               btc);
      if (r.bar) lv_bar_set_value(r.bar, (int)constrain(usd, 0, 100), LV_ANIM_ON);
      if (r.bar2)
        lv_bar_set_value(r.bar2, (int)constrain(eur, 0, 100), LV_ANIM_ON);
    } else if (strcmp(r.type, "calendar") == 0 &&
               data["calendar"]["events"].is<JsonArray>()) {
      JsonArrayConst ev = data["calendar"]["events"].as<JsonArrayConst>();
      if (ev.size() == 0) {
        snprintf(buf, sizeof(buf), "No upcoming");
      } else {
        size_t n = min((size_t)3, ev.size());
        buf[0] = 0;
        for (size_t e = 0; e < n; e++) {
          JsonObjectConst it = ev[e].as<JsonObjectConst>();
          const char* title = it["title"] | "Event";
          int mins = it["startsInMin"] | 0;
          bool nowEv = it["happeningNow"] | false;
          char line[64];
          if (nowEv)
            snprintf(line, sizeof(line), "• Now  %.40s\n", title);
          else
            snprintf(line, sizeof(line), "• %dm  %.40s\n", mins, title);
          strncat(buf, line, sizeof(buf) - strlen(buf) - 1);
        }
      }
    } else if (strcmp(r.type, "tasks") == 0 &&
               data["tasks"]["tasks"].is<JsonArray>()) {
      JsonArrayConst tasks = data["tasks"]["tasks"].as<JsonArrayConst>();
      buf[0] = 0;
      size_t n = min((size_t)4, tasks.size());
      if (n == 0) snprintf(buf, sizeof(buf), "Inbox empty");
      for (size_t t = 0; t < n; t++) {
        const char* title = tasks[t]["title"] | "Task";
        char line[64];
        snprintf(line, sizeof(line), "☐ %.48s\n", title);
        strncat(buf, line, sizeof(buf) - strlen(buf) - 1);
      }
    } else if (strcmp(r.type, "fit") == 0 && !data["fit"].isNull()) {
      int steps = data["fit"]["steps"] | 0;
      int cal = data["fit"]["calories"] | 0;
      int act = data["fit"]["activeMinutes"] | 0;
      snprintf(buf, sizeof(buf), "%d steps\n%d kcal · %d min", steps, cal, act);
      if (r.bar) lv_bar_set_value(r.bar, constrain(steps, 0, 10000), LV_ANIM_ON);
    } else if (strcmp(r.type, "pc_monitor") == 0 &&
               !data["pc_monitor"].isNull()) {
      int cpu = data["pc_monitor"]["cpu"] | 0;
      int ram = data["pc_monitor"]["ram"] | 0;
      int gpu = data["pc_monitor"]["gpu"] | 0;
      snprintf(buf, sizeof(buf), "CPU %d%%  RAM %d%%\nGPU %d%%", cpu, ram, gpu);
      if (r.bar) lv_bar_set_value(r.bar, constrain(cpu, 0, 100), LV_ANIM_ON);
      if (r.bar2) lv_bar_set_value(r.bar2, constrain(ram, 0, 100), LV_ANIM_ON);
    } else if (strcmp(r.type, "weather") == 0 && !data["weather"].isNull()) {
      const char* city = data["weather"]["location"]["name"] | "City";
      double temp = data["weather"]["current"]["temp_c"] | 0.0;
      const char* cond =
          data["weather"]["current"]["condition"]["text"] | "";
      int hum = data["weather"]["current"]["humidity"] | 0;
      snprintf(buf, sizeof(buf), "%s\n%.0f°C  %s\nHumidity %d%%", city, temp,
               cond, hum);
    }

    if (buf[0] && r.body) lv_label_set_text(r.body, buf);
  }
}

void UIBuilder::tickLocalWidgets() {
  sduiEngine_.tick();
  uint32_t now = millis();
  if (now - lastLocalTick_ < 1000) return;
  lastLocalTick_ = now;

  time_t t = time(nullptr);
  struct tm* tm = localtime(&t);
  char clockBuf[16] = "--:--:--";
  if (tm && t > 100000) {
    snprintf(clockBuf, sizeof(clockBuf), "%02d:%02d:%02d", tm->tm_hour,
             tm->tm_min, tm->tm_sec);
  } else {
    uint32_t sec = now / 1000;
    snprintf(clockBuf, sizeof(clockBuf), "%02u:%02u:%02u",
             (unsigned)((sec / 3600) % 24), (unsigned)((sec / 60) % 60),
             (unsigned)(sec % 60));
  }

  for (int i = 0; i < g_refCount; i++) {
    WidgetRef& r = g_refs[i];
    if (strcmp(r.type, "flipper_clock") == 0 && r.body) {
      lv_label_set_text(r.body, clockBuf);
    }
    if (strcmp(r.type, "timer") == 0 && r.timerSecLeft >= 0) {
      if (r.timerSecLeft > 0) r.timerSecLeft--;
      char buf[16];
      snprintf(buf, sizeof(buf), "%02d:%02d", r.timerSecLeft / 60,
               r.timerSecLeft % 60);
      if (r.body) lv_label_set_text(r.body, buf);
      if (r.bar) lv_bar_set_value(r.bar, r.timerSecLeft, LV_ANIM_OFF);
    }
    if (strcmp(r.type, "stretch_reminder") == 0 && r.stretchEveryMin > 0) {
      uint32_t interval = (uint32_t)r.stretchEveryMin * 60UL * 1000UL;
      uint32_t elapsed = now - r.stretchLastMs;
      int pct = (int)((elapsed * 100UL) / interval);
      if (pct > 100) pct = 100;
      if (r.bar) lv_bar_set_value(r.bar, pct, LV_ANIM_OFF);
      if (elapsed >= interval) {
        r.stretchLastMs = now;
        if (r.body) lv_label_set_text(r.body, "Stretch now!");
      }
    }
  }
}
