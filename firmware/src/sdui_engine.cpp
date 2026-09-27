#include "remote_image.h"
#include "color_disc.h"
#include "sdui_engine.h"
#include "countdown_runtime.h"
#include "clock_runtime.h"
#include "image_sequence.h"
#include "inline_image.h"
#include "serial_bridge.h"
#include "date_runtime.h"
#include "interval_runtime.h"
#include "fonts/ui_fonts.h"
#include "fonts/geist_fonts.h"
#include "flip_text.h"
#include "board_config.h"
#include <esp_timer.h>
#include <sys/time.h>
#include "bridge_queue.h"
#include "display_manager.h"
#include "frame_buffer.h"
#include "media_pipeline.h"
#include "config_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <time.h>
#include <math.h>

SDUIEngine* g_sdui = nullptr;

static void stripLvglDefaults(lv_obj_t* obj) {
  if (!obj) return;
  lv_obj_set_style_border_width(obj, 0, 0);
  lv_obj_set_style_outline_width(obj, 0, 0);
  lv_obj_set_style_shadow_width(obj, 0, 0);
  lv_obj_set_style_pad_all(obj, 0, 0);
  lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
  lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static void sdui_btn_cb(lv_event_t* e) {
  if (!g_sdui) return;
  intptr_t idx = (intptr_t)lv_event_get_user_data(e);
  g_sdui->onButtonIndex((int)idx);
}

static void sdui_press_cb(lv_event_t* e) {
  if(g_sdui) g_sdui->captureButtonIndex((int)(intptr_t)lv_event_get_user_data(e));
}

static void reminder_cb(lv_event_t*) { if(g_sdui) g_sdui->openReminder(); }

static void sdui_gesture_cb(lv_event_t*) {
  if (!g_sdui) return;
  lv_indev_t* input = lv_indev_get_act();
  if (!input) return;
  const auto direction = lv_indev_get_gesture_dir(input);
  if (direction != LV_DIR_LEFT && direction != LV_DIR_RIGHT) return;
  g_sdui->swipe(direction == LV_DIR_LEFT ? 1 : -1);
  // A swipe that starts over START must not become a click on release.
  lv_indev_wait_release(input);
}

[[maybe_unused]] static void anim_opa_cb(void* obj, int32_t v) {
  lv_obj_set_style_opa((lv_obj_t*)obj, (lv_opa_t)v, 0);
}
[[maybe_unused]] static void anim_trans_y_cb(void* obj, int32_t v) {
  lv_obj_set_style_translate_y((lv_obj_t*)obj, v, 0);
}
[[maybe_unused]] static void anim_trans_x_cb(void* obj, int32_t v) {
  lv_obj_set_style_translate_x((lv_obj_t*)obj, v, 0);
}

SDUIEngine::SDUIEngine()
    : _root(nullptr),
      _host(nullptr),
      _chrome(nullptr),
      _chromeBytes(nullptr),
      _chromeDirty(false),
      _sceneVersion(0),
      _generation(0),
      _pageIndex(0),
      _mounted(false),
      _boundCount(0),
      _lastTickMs(0) {
  memset(&_chromeDsc, 0, sizeof(_chromeDsc));
}

SDUIEngine::~SDUIEngine() {
  if (g_sdui == this) g_sdui = nullptr;
  heap_caps_free(_bound);
}

void SDUIEngine::init(lv_obj_t* parentScreen) {
  _host = parentScreen;
  _root = parentScreen;
  _chrome = nullptr;
  _state.to<JsonObject>();
  g_sdui = this;
}

void SDUIEngine::clearBindings() {
  _boundCount = 0;
}

void SDUIEngine::attachHost(lv_obj_t* host) {
  // The caller (UIBuilder::clearUi) just ran lv_obj_clean(), so every object we
  // created — including the chrome image and all bound labels — is already
  // freed. Drop the dangling references and re-point at the new host. Keep
  // _state (timer countdown, etc.) and _chromeBytes (we still own that buffer;
  // reattachChrome() re-links it on the next parseScene()).
  _host = host;
  _root = host;
  _chrome = nullptr;
  clearBindings();
  _pageLabel = nullptr;
  _syncLabel = nullptr;
  _mounted = false;
  _pendingStep = 0;
  _loadingPage = -1;
  _chromeBgAttached = false;
  _overlaysHidden = false;
  // Keep _chromeDsc/_chromeBytes intact so reattachChrome() can re-link the
  // background on the next parseScene() without a re-fetch.
  g_sdui = this;
}

bool SDUIEngine::parseScene(const char* jsonString, uint32_t generation) {
  if (!_host || !jsonString) return false;
  SceneDocument doc;
  if (deserializeJson(doc, jsonString, DeserializationOption::NestingLimit(SCENE_NESTING_LIMIT)) || doc["protocol"] != "sdui" || doc["type"] != "scene") return false;
  JsonObject payload = doc["payload"].as<JsonObject>();
  JsonVariantConst incoming = payload["scene"];
  JsonArrayConst incomingPages = incoming["pages"].as<JsonArrayConst>();
  if (incomingPages.isNull() || incomingPages.size() == 0) return false;
  const uint64_t version = incoming["version"] | static_cast<uint64_t>(0);
  const char* timezone = incoming["timeZoneRules"] | ClockRuntime::kDefaultTimezone;
  if (strlen(timezone) < 96) { setenv("TZ", timezone, 1); tzset(); }
  for (JsonVariantConst page : incomingPages) {
    const char* id = page["id"] | "";
    if (!id[0] || strlen(id) >= sizeof(PageRequest::pageId)) return false;
  }

  if (_mounted && version != 0 && version == _sceneVersion) {
    // Same layout the device is already showing. Rebuilding the LVGL tree here
    // is what made the dashboard flicker through every status/Wi-Fi/sync pass.
    adoptSceneGeneration(generation);
    Serial.printf("SDUI scene unchanged version=%llu generation=%lu\n",
                  (unsigned long long)version, (unsigned long)generation);
    return true;
  }

  // Keep locally running/paused timers across layout syncs
  // when their widget ID and countdown duration have not changed.
  SceneDocument nextState;
  nextState.set(payload["state"]);
  for (JsonPair pair : nextState.as<JsonObject>()) {
    JsonObject old = _state[pair.key().c_str()].as<JsonObject>();
    JsonObject next = pair.value().as<JsonObject>();
    if (old.isNull() || next.isNull()) continue;
    if(next["interval"].is<JsonObject>() && old["interval"].is<JsonObject>() &&
       next["interval"]["configKey"]==old["interval"]["configKey"])
      next["interval"].set(old["interval"]);
    if (next["date"].is<JsonObject>() && old["date"].is<JsonObject>() &&
        next["date"]["initialDate"] == old["date"]["initialDate"])
      next["date"].set(old["date"]);
    if (next["value"].is<JsonObject>() && old["value"].is<JsonObject>() &&
        next["value"]["durationSec"] == old["value"]["durationSec"]) {
      next["value"].set(old["value"]);
      next["btn_label"].set(old["btn_label"]);
      next["progress"].set(old["progress"]);
    }
  }
  ImageSequence::beginScene();
  _state.set(nextState);
  const int resW = incoming["resolution"]["width"] | PANEL_WIDTH;
  const int resH = incoming["resolution"]["height"] | PANEL_HEIGHT;
  char preferredPage[64] = {};
  if (_mounted) snprintf(preferredPage, sizeof(preferredPage), "%s", currentPageId());
  else if (generation == 0) ConfigStore::cachedChromePage(preferredPage, sizeof(preferredPage), resW, resH);
  _scene.set(incoming);
  JsonArray pages = _scene["pages"].as<JsonArray>();
  if (pages.isNull() || pages.size() == 0) return false;
  // Keep still-valid page bakes. Clearing them made chrome-only widgets
  // (weather/fit/markets) vanish until the next 1.2 MB fetch finished, and
  // left stretch/media as unstyled live overlays.
  for (auto& page : _pageCache) {
    if (!page.bytes) continue;
    bool known = false;
    for (JsonObject p : pages)
      if (strcmp(p["id"] | "", page.id) == 0) known = true;
    if (!known || page.w != resW || page.h != resH) {
      if (_chromeBytes == page.bytes) _chromeBytes = nullptr;
      if (_presentedChrome == page.bytes) { _presentedChrome = nullptr; _presentedW = _presentedH = 0; }
      releaseFrameBuffer(page.bytes);
      page = CachedPage{};
    }
  }
  if (!_chromeBytes) {
    lv_img_cache_invalidate_src(&_chromeDsc);
    memset(&_chromeDsc, 0, sizeof(_chromeDsc));
  }
  _sceneVersion = version;
  _generation = generation;
  _pendingStep = 0;
  _mounted = true;
  int selected = 0;
  const char* active = incoming["activePageId"] | "";
  for (unsigned i = 0; i < pages.size(); ++i)
    if (strcmp(pages[i]["id"] | "", active) == 0) selected = i;
  for (unsigned i = 0; i < pages.size(); ++i)
    if (preferredPage[0] && strcmp(pages[i]["id"] | "", preferredPage) == 0) selected = i;
  // A scene replacement is not a swipe from the old page index.
  _pageIndex = selected;
  showPage(selected);
  sendChannels();
  Serial.printf("SDUI scene pages=%u generation=%lu\n", unsigned(pages.size()), (unsigned long)_generation);
  return true;
}

const char* SDUIEngine::currentPageId() {
  return _scene["pages"][_pageIndex]["id"] | "";
}

void SDUIEngine::swipe(int step) {
  if (_mounted) _pendingStep = step;
}

void SDUIEngine::showPage(int index, bool chromeReady) {
  JsonArray pages = _scene["pages"].as<JsonArray>();
  if (!_mounted || !_host || index < 0 || index >= int(pages.size())) return;
  bool cached = false;
  const char* target = pages[index]["id"] | "";
  for (auto& page : _pageCache) if (page.bytes && !strcmp(page.id, target)) cached = true;
  if (index != _pageIndex && _chromeBytes && !cached) {
    _loadingPage = index;
    _pageLoadingAt = millis();
    PageRequest request{}; request.generation = _generation;
    request.chromeRefreshMs = pages[index]["chromeRefreshMs"] | 30000U;
    request.stateRefreshMs = pages[index]["stateRefreshMs"] | 30000U;
    snprintf(request.pageId, sizeof(request.pageId), "%s", target);
    if (pageRequestQueue) xQueueOverwrite(pageRequestQueue, &request);
    if (_syncLabel) {
      lv_label_set_text(_syncLabel, "Loading\xE2\x80\xA6");
      lv_obj_align(_syncLabel, LV_ALIGN_BOTTOM_MID, 0, -8);
      lv_obj_add_flag(_syncLabel, LV_OBJ_FLAG_HIDDEN);
    }
    return;
  }
  _loadingPage = -1;
  _pageIndex = index;
  _pageError = 0;
  RemoteImage::beginPage();
  MediaPipeline::detachAll();
  clearBindings();
  _pageLabel = nullptr;
  _syncLabel = nullptr;
  lv_obj_clean(_host);
  stripLvglDefaults(_host);
  lv_obj_set_size(_host, _scene["resolution"]["width"] | PANEL_WIDTH,
                        _scene["resolution"]["height"] | PANEL_HEIGHT);
  lv_obj_set_pos(_host, 0, 0);
  lv_obj_set_style_bg_opa(_host, LV_OPA_TRANSP, 0);
  lv_obj_add_flag(_host, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(_host, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_remove_event_cb(_host, sdui_gesture_cb);
  lv_obj_add_event_cb(_host, sdui_gesture_cb, LV_EVENT_GESTURE, nullptr);
  for (JsonObject n : pages[index]["nodes"].as<JsonArray>()) createNode(n, _host);
  _root = _host;
  // Centered loading state until this page's bake arrives. Keep it from being the
  // last child: tests and reminders treat the trailing label as the page label.
  _syncLabel = lv_label_create(_host);
  lv_obj_set_style_text_font(_syncLabel, &ui_font_12, 0);
  lv_obj_set_style_text_color(_syncLabel, lv_color_hex(0x6b6b74), 0);
  lv_label_set_text(_syncLabel, "Loading\xE2\x80\xA6");
  lv_obj_add_flag(_syncLabel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(_syncLabel, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_align(_syncLabel, LV_ALIGN_CENTER, 0, 0);
  _pageLabel = lv_label_create(_host);
  lv_obj_set_style_text_font(_pageLabel, &ui_font_12, 0);
  lv_obj_set_style_text_color(_pageLabel, lv_color_hex(0xa1a1aa), 0);
  lv_obj_add_flag(_pageLabel, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(_pageLabel,12);
  lv_obj_add_event_cb(_pageLabel,reminder_cb,LV_EVENT_CLICKED,nullptr);
  lv_obj_add_flag(_pageLabel, LV_OBJ_FLAG_GESTURE_BUBBLE);
  attachPageBackground(true);
  refreshBindings();
  PageRequest request {};
  request.generation = _generation;
  request.chromeReady = chromeReady;
  request.chromeRefreshMs=pages[index]["chromeRefreshMs"] | 30000U;
  request.stateRefreshMs=pages[index]["stateRefreshMs"] | 30000U;
  snprintf(request.pageId, sizeof(request.pageId), "%s", currentPageId());
  if (pageRequestQueue) xQueueOverwrite(pageRequestQueue, &request);
  Serial.printf("SDUI page=%s index=%d bindings=%d\n", currentPageId(), index, _boundCount);
}

void SDUIEngine::updatePageLabel() {
  if (!_pageLabel) return;
  if(_reminderPage>=0) {
    lv_obj_set_style_text_color(_pageLabel,lv_color_hex(0xffb566),0);
    lv_label_set_text_fmt(_pageLabel,"Reminder: %s  [Tap to open]",_reminderText);
    lv_obj_align(_pageLabel,LV_ALIGN_BOTTOM_MID,0,0);
    return;
  }
  // Keep device diagnostics on serial; only actionable reminders belong on the display.
  lv_label_set_text(_pageLabel, "");
}

void SDUIEngine::openReminder() {
  if(_reminderPage>=0) swipe(_reminderPage-_pageIndex);
}

void SDUIEngine::setConnectionStatus(const char* status) {
  if (!status || !strcmp(status,_connectionStatus)) return;
  snprintf(_connectionStatus,sizeof(_connectionStatus),"%s",status);
  updatePageLabel();
}

void SDUIEngine::pageLoadFailed(const char* pageId, uint32_t generation, int status) {
  if (_mounted && generation == _generation && _loadingPage >= 0 &&
      !strcmp(pageId, _scene["pages"][_loadingPage]["id"] | "")) {
    Serial.printf("SDUI pending page=%s status=%d; retrying in background\n", pageId, status);
    return;
  }
  if (!_mounted || generation != _generation || strcmp(pageId, currentPageId())) return;
  _pageError = status ? status : -1;
  Serial.printf("SDUI page=%s status=%d; retrying in background\n", pageId, status);
  updatePageLabel();
}

void SDUIEngine::clearPageCache() {
  lv_disp_set_bg_image(lv_disp_get_default(), nullptr);
  lv_img_cache_invalidate_src(&_chromeDsc);
  _chromeBytes = nullptr;
  _chromeDirty = false;
  _chromeBgAttached = false;
  memset(&_chromeDsc, 0, sizeof(_chromeDsc));
  // The scan-out buffer no longer matches any cache entry; force the next
  // attach to re-present instead of trusting a dangling pointer fingerprint.
  _presentedChrome = nullptr;
  _presentedW = _presentedH = 0;
  _presentedFill = false;
  _overlaysHidden = false;
  _chromeBgAttached = false;
  _sceneVersion = 0;
  for (auto& page : _pageCache) {
    releaseFrameBuffer(page.bytes);
    page = CachedPage{};
  }
}

void SDUIEngine::adoptSceneGeneration(uint32_t generation) {
  _generation = generation;
  PageRequest request {};
  request.generation = _generation;
  JsonArray pages = _scene["pages"].as<JsonArray>();
  const int target = _loadingPage >= 0 ? _loadingPage : _pageIndex;
  if (target >= 0 && target < int(pages.size())) {
    request.chromeRefreshMs = pages[target]["chromeRefreshMs"] | 30000U;
    request.stateRefreshMs = pages[target]["stateRefreshMs"] | 30000U;
  }
  snprintf(request.pageId, sizeof(request.pageId), "%s", pages[target]["id"] | "");
  if (pageRequestQueue) xQueueOverwrite(pageRequestQueue, &request);
  sendChannels();
}

void SDUIEngine::attachPageBackground(bool overlaysRebuilt) {
  _chromeBytes = nullptr;
  for (auto& page : _pageCache) {
    if (page.bytes && strcmp(page.id, currentPageId()) == 0) {
      _chromeBytes = page.bytes;
      page.used = ++_cacheClock;
      if (_chromeDsc.data != page.bytes || _chromeDsc.header.w != page.w ||
          _chromeDsc.header.h != page.h) {
        lv_img_cache_invalidate_src(&_chromeDsc);
        memset(&_chromeDsc, 0, sizeof(_chromeDsc));
        _chromeDsc.header.cf = LV_IMG_CF_TRUE_COLOR;
        _chromeDsc.header.w = page.w;
        _chromeDsc.header.h = page.h;
        _chromeDsc.data_size = size_t(page.w) * page.h * 2;
        _chromeDsc.data = page.bytes;
      }
      break;
    }
  }
  if (!_chromeBytes) restoreChromeFromFlash();
  const bool hasChrome = _chromeBytes != nullptr;
  if (hasChrome) {
    if (!_chromeBgAttached) {
      lv_disp_set_bg_image(lv_disp_get_default(), &_chromeDsc);
      _chromeBgAttached = true;
    }
  } else if (_chromeBgAttached) {
    lv_disp_set_bg_image(lv_disp_get_default(), nullptr);
    _chromeBgAttached = false;
  }
  lv_disp_set_bg_color(lv_disp_get_default(), lv_color_hex(0x09090b));
  lv_obj_set_style_bg_opa(_host, LV_OPA_TRANSP, 0);
  lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_TRANSP, 0);
  const bool sameChrome = hasChrome && _presentedChrome == _chromeBytes &&
                          _presentedW == _chromeDsc.header.w &&
                          _presentedH == _chromeDsc.header.h;
  if (hasChrome && _chromeDsc.header.w > 0 && _chromeDsc.header.h > 0 && !sameChrome) {
    // Compose backgrounds and controls together; direct writes erased live
    // overlays until a later LVGL refresh happened to repaint them.
    lv_obj_invalidate(lv_scr_act());
    _presentedChrome = _chromeBytes;
    _presentedW = _chromeDsc.header.w;
    _presentedH = _chromeDsc.header.h;
    _presentedFill = false;
  } else if (!hasChrome && !_presentedFill) {
    lv_obj_invalidate(lv_scr_act());
    _presentedFill = true;
    _presentedChrome = nullptr;
    _presentedW = _presentedH = 0;
  }
  const bool hideOverlays = !hasChrome;
  if (hideOverlays != _overlaysHidden || overlaysRebuilt) {
    setLiveOverlaysHidden(hideOverlays);
    _overlaysHidden = hideOverlays;
  }
  if (_syncLabel) {
    if (!hasChrome) lv_obj_clear_flag(_syncLabel, LV_OBJ_FLAG_HIDDEN);
    else if (_loadingPage < 0) lv_obj_add_flag(_syncLabel, LV_OBJ_FLAG_HIDDEN);
    // Otherwise preserve the pending navigation indicator.
  }
  _overlaysHeldAt = hasChrome ? 0 : millis();

  updatePageLabel();
  if (overlaysRebuilt) {
    if (_host) {
      uint32_t n = lv_obj_get_child_cnt(_host);
      for (uint32_t i = 0; i < n; i++) {
        lv_obj_t* child = lv_obj_get_child(_host, i);
        if (child) lv_obj_invalidate(child);
      }
    }
  }
}

void SDUIEngine::setLiveOverlaysHidden(bool hidden) {
  if (!_host) return;
  uint32_t n = lv_obj_get_child_cnt(_host);
  for (uint32_t i = 0; i < n; i++) {
    lv_obj_t* child = lv_obj_get_child(_host, i);
    if (!child || child == _pageLabel || child == _syncLabel || lv_obj_check_type(child, &lv_dropdownlist_class)) continue;
    if (hidden) lv_obj_add_flag(child, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(child, LV_OBJ_FLAG_HIDDEN);
  }
}

void SDUIEngine::restoreChromeFromFlash() {
  const char* pageId = currentPageId();
  const int w = _scene["resolution"]["width"] | PANEL_WIDTH;
  const int h = _scene["resolution"]["height"] | PANEL_HEIGHT;
  uint8_t* bytes = nullptr;
  size_t length = 0;
  if (!ConfigStore::loadChrome(pageId, w, h, &bytes, &length)) return;
  CachedPage* slot = nullptr;
  for (auto& page : _pageCache) if (!page.bytes) { slot = &page; break; }
  if (!slot) for (auto& page : _pageCache) {
    if (strcmp(page.id, pageId) != 0 && (!slot || page.used < slot->used)) slot = &page;
  }
  if (!slot) {
    releaseFrameBuffer(bytes);
    return;
  }
  uint8_t* previous = slot->bytes;
  slot->bytes = bytes;
  slot->w = w;
  slot->h = h;
  slot->used = ++_cacheClock;
  snprintf(slot->id, sizeof(slot->id), "%s", pageId);
  _chromeBytes = bytes;
  memset(&_chromeDsc, 0, sizeof(_chromeDsc));
  _chromeDsc.header.cf = LV_IMG_CF_TRUE_COLOR;
  _chromeDsc.header.w = w;
  _chromeDsc.header.h = h;
  _chromeDsc.data_size = length;
  _chromeDsc.data = bytes;
  if (previous == _presentedChrome) { _presentedChrome = nullptr; _presentedW = _presentedH = 0; }
  releaseFrameBuffer(previous);
  Serial.printf("Chrome restored from flash page=%s bytes=%u\n", pageId, unsigned(length));
}

void SDUIEngine::reattachChrome() { if (_mounted) attachPageBackground(); }
void SDUIEngine::presentChrome() {
  if (_chromeDirty && _mounted) attachPageBackground();
  _chromeDirty = false;
}

void SDUIEngine::setChromeImage(uint8_t* rgb565, int w, int h,
                                const char* pageId, uint32_t generation) {
  if (!rgb565) return;
  if (!_mounted || generation != _generation || !pageId || !pageId[0] ||
      w != (_scene["resolution"]["width"] | PANEL_WIDTH) ||
      h != (_scene["resolution"]["height"] | PANEL_HEIGHT)) {
    if (pageId) pageLoadFailed(pageId,generation,-5);
    releaseFrameBuffer(rgb565);
    return;
  }
  bool known = false;
  for (JsonObject p : _scene["pages"].as<JsonArray>())
    if (strcmp(p["id"] | "", pageId) == 0) known = true;
  if (!known) { releaseFrameBuffer(rgb565); return; }
  if (strcmp(pageId, currentPageId()) == 0) { _pageError = 0; updatePageLabel(); }
  const size_t bytes = size_t(w) * h * 2;
  psramInvalidate(rgb565, bytes);
  CachedPage* slot = nullptr;
  for (auto& p : _pageCache) if (p.bytes && strcmp(p.id, pageId) == 0) slot = &p;
  if (slot && slot->w == w && slot->h == h && memcmp(slot->bytes, rgb565, bytes) == 0) {
    releaseFrameBuffer(rgb565);
    return;
  }
  if (!slot) for (auto& p : _pageCache) if (!p.bytes) { slot = &p; break; }
  if (!slot) for (auto& p : _pageCache) {
    if (strcmp(p.id, currentPageId()) != 0 && (!slot || p.used < slot->used)) slot = &p;
  }
  if (!slot) { releaseFrameBuffer(rgb565); return; }
  uint8_t* previous = slot->bytes;
  slot->bytes = rgb565; slot->w = w; slot->h = h; slot->used = ++_cacheClock;
  snprintf(slot->id, sizeof(slot->id), "%s", pageId);
  if (strcmp(pageId, currentPageId()) == 0) attachPageBackground();
  // Never let the presented fingerprint alias a buffer we are about to free;
  // a recycled allocation could otherwise masquerade as "unchanged".
  if (previous == _presentedChrome) { _presentedChrome = nullptr; _presentedW = _presentedH = 0; }
  releaseFrameBuffer(previous);
  Serial.printf("Chrome cached page=%s bytes=%u\n", pageId, unsigned(bytes));
  if (_loadingPage >= 0 && !strcmp(pageId, _scene["pages"][_loadingPage]["id"] | "")) {
    const int target = _loadingPage; _loadingPage = -1; showPage(target, true);
  }
}

lv_obj_t* SDUIEngine::createNode(JsonObject node, lv_obj_t* parent) {
  const char* type = node["type"] | "";
  if (!type[0]) return nullptr;

  // Page backgrounds are managed by the RGB565 cache. General image nodes
  // are not yet supported by the device runtime.
  if (strcmp(type, "image") == 0 && node["props"]["asset_ids"].isNull() && node["binding"]["format"] != "rgb565_hex" && node["props"]["source_url"].isNull()) return nullptr;
  JsonArray children = node["children"].as<JsonArray>();
  if (strcmp(type, "container") == 0 &&
      (children.isNull() || children.size() == 0)) {
    return nullptr;
  }

  lv_obj_t* obj = nullptr;
  lv_obj_t* extraLabel = nullptr;
  uint8_t kind = 0;

  if (strcmp(type, "container") == 0) {
    obj = createContainer(parent);
  } else if (strcmp(type, "text") == 0) {
    const bool flip = node["props"]["transition"] == "flip";
    obj = flip ? FlipText::create(parent) : createText(parent);
    kind = flip ? 4 : 1;
  } else if (strcmp(type, "button") == 0) {
    obj = createButton(node, parent);
    extraLabel = lv_obj_get_child(obj, 0);
    kind = 2;
  } else if (strcmp(type, "select") == 0) {
    obj = lv_dropdown_create(parent);
    lv_dropdown_set_options(obj, "Unavailable");
    stripLvglDefaults(obj);
    auto* list = lv_dropdown_get_list(obj);
    stripLvglDefaults(list);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x171719), 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(list, lv_color_hex(0xf4f4f5), 0);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x38302a), LV_PART_SELECTED);
    lv_obj_set_style_text_color(list, lv_color_hex(0xffb86b), LV_PART_SELECTED);
    lv_obj_set_style_radius(list, 10, 0);
    lv_obj_set_style_pad_all(obj, 8, 0);
    lv_obj_set_style_text_font(list, fontFromProp("font_14"), 0);
    lv_obj_set_style_max_height(lv_dropdown_get_list(obj), 260, 0);
    kind = 6;
  } else if (strcmp(type, "color_picker") == 0) {
    if (node["props"]["appearance"] == "disc") {
      obj = createContainer(parent);
      lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
      kind = 8; // Transparent hit surface over server-rendered color disc.
    } else {
    obj = lv_colorwheel_create(parent, true);
    lv_colorwheel_set_mode(obj, LV_COLORWHEEL_MODE_HUE);
    lv_colorwheel_set_mode_fixed(obj, true);
    kind = 7;
    }
  } else if (strcmp(type, "image") == 0) {
    obj = createImage(parent);
    if(node["binding"]["format"] == "rgb565_hex") kind=5;
  } else if (strcmp(type, "progress") == 0) {
    obj = createProgress(parent);
    kind = 3;
  } else if (strcmp(type, "rectangle") == 0) {
    obj = createRectangle(parent);
  } else if (strcmp(type, "line") == 0 || strcmp(type, "circle") == 0 ||
             strcmp(type, "arc") == 0) {
    obj = createRectangle(parent);
  } else {
    Serial.printf("SDUI unknown primitive: %s\n", type);
    obj = createContainer(parent);
  }

  if (!obj) return nullptr;
  if (kind != 6 && kind != 7 && kind != 8) lv_obj_add_flag(obj, LV_OBJ_FLAG_GESTURE_BUBBLE);
  else lv_obj_clear_flag(obj, LV_OBJ_FLAG_GESTURE_BUBBLE);
  applyLayout(obj, node["layout"].as<JsonObject>());
  applyProps(obj, node["props"].as<JsonObject>(), type);
  if(kind==5) InlineImage::attach(obj,node["layout"]["width"].as<unsigned>());
  else if (strcmp(type,"image")==0 && node["props"]["source_url"].is<const char*>())
    RemoteImage::attach(obj,node["props"]["source_url"],node["layout"]["width"],node["layout"]["height"],node["props"]["refresh_ms"] | 1000U,node["props"]["fit"] == "cover");
  else if (strcmp(type,"image")==0) ImageSequence::attach(obj,node["props"]["asset_ids"].as<JsonArray>(),node["layout"]["width"].as<unsigned>(),node["layout"]["height"].as<unsigned>(),node["props"]["rotate_ms"] | 30000U,node["id"] | "");
  remember(node, obj, extraLabel, kind);

  if (!children.isNull()) {
    for (JsonObject child : children) createNode(child, obj);
  }
  return obj;
}

void SDUIEngine::remember(JsonObject node, lv_obj_t* obj, lv_obj_t* label,
                          uint8_t kind) {
  if (_boundCount >= kMaxBound) { Serial.println("SDUI binding capacity exceeded"); return; }
  JsonObject binding = node["binding"].as<JsonObject>();
  JsonObject action = node["action"].as<JsonObject>();
  const char* src = binding["source"] | "";
  const char* actType = action["type"] | "";
  if (!src[0] && !actType[0]) return;

  if (_boundCount == _boundCapacity) {
    const int capacity=_boundCapacity ? _boundCapacity*2 : 64;
    auto* bindings=static_cast<Bound*>(heap_caps_calloc(capacity,sizeof(Bound),MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if(!bindings) { Serial.println("SDUI bindings: PSRAM allocation failed");return; }
    if(_boundCount) memcpy(bindings,_bound,sizeof(Bound)*_boundCount);
    heap_caps_free(_bound);_bound=bindings;_boundCapacity=capacity;
  }

  Bound& b = _bound[_boundCount];
  memset(&b, 0, sizeof(Bound));
  b.used = true;
  strncpy(b.id, node["id"] | "", sizeof(b.id) - 1);
  strncpy(b.source, src, sizeof(b.source) - 1);
  strncpy(b.colorSource, node["props"]["color_source"] | "", sizeof(b.colorSource)-1);
  strncpy(b.commandPrefix, node["props"]["command_prefix"] | "", sizeof(b.commandPrefix)-1);
  strncpy(b.format, binding["format"] | "", sizeof(b.format) - 1);
  strncpy(b.actionType, actType, sizeof(b.actionType) - 1);
  strncpy(b.actionName, action["name"] | "", sizeof(b.actionName) - 1);
  strncpy(b.actionPath, action["path"] | "", sizeof(b.actionPath) - 1);
  strncpy(b.actionDataSource, action["dataSource"] | "", sizeof(b.actionDataSource) - 1);
  b.actionValue = action["value"] | 0;
  b.kind = kind;
  b.transition = transitionFromProp(node["props"]["transition"] | "");
  b.lastText[0] = 0;
  b.obj = obj;
  b.label = label;
  b.intervalSec = node["props"]["interval_sec"] | 0;
  b.intervalStartMs = millis();

  if (kind == 2) {
    lv_obj_add_event_cb(obj, sdui_press_cb, LV_EVENT_PRESSED,(void*)(intptr_t)_boundCount);
    lv_obj_add_event_cb(obj, sdui_btn_cb, LV_EVENT_CLICKED,
                        (void*)(intptr_t)_boundCount);
  }
  if (kind == 6 || kind == 7 || kind == 8) {
    lv_obj_add_event_cb(obj, sdui_btn_cb, kind == 6 ? LV_EVENT_VALUE_CHANGED : LV_EVENT_RELEASED,
                       (void*)(intptr_t)_boundCount);
  }
  _boundCount++;
}

void SDUIEngine::applyLayout(lv_obj_t* obj, JsonObject layout) {
  if (layout.isNull()) return;
  // JSON coordinates can be fractional after dividing a calendar into 7 columns.
  // ArduinoJson's `value | 0` rejects floats and silently placed them at (0, 0).
  auto coordinate = [](JsonVariant value, int fallback) -> lv_coord_t {
    const double n = value.is<double>() ? value.as<double>() : fallback;
    return isfinite(n) ? lround(fmax(-LV_COORD_MAX, fmin(LV_COORD_MAX, n))) : fallback;
  };
  if (!layout["x"].isNull() && !layout["y"].isNull()) {
    lv_obj_set_pos(obj, coordinate(layout["x"],0), coordinate(layout["y"],0));
  }
  if (!layout["width"].isNull() && !layout["height"].isNull()) {
    lv_obj_set_size(obj, coordinate(layout["width"],8), coordinate(layout["height"],8));
  }
}

void SDUIEngine::applyProps(lv_obj_t* obj, JsonObject props, const char* type) {
  if (props.isNull()) return;

  if (props["bg_color"].is<const char*>()) {
    lv_obj_set_style_bg_color(obj, hexToColor(props["bg_color"]), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
  }
  if (props["bg_grad_color"].is<const char*>()) {
    lv_obj_set_style_bg_grad_color(obj, hexToColor(props["bg_grad_color"]), 0);
    lv_obj_set_style_bg_grad_dir(obj, LV_GRAD_DIR_VER, 0);
  }
  if (!props["bg_opa"].isNull()) {
    lv_obj_set_style_bg_opa(obj, props["bg_opa"] | 0, 0);
  }
  if (!props["radius"].isNull()) {
    lv_obj_set_style_radius(obj, props["radius"] | 0, 0);
  }
  if (!props["border_width"].isNull()) {
    lv_obj_set_style_border_width(obj, props["border_width"] | 0, 0);
  }
  if (props["border_color"].is<const char*>()) {
    lv_obj_set_style_border_color(obj, hexToColor(props["border_color"]), 0);
  }
  if (!props["shadow_width"].isNull())
    lv_obj_set_style_shadow_width(obj, props["shadow_width"] | 0, 0);
  if (props["text_color"].is<const char*>())
    lv_obj_set_style_text_color(obj, hexToColor(props["text_color"]), 0);
  if (props["color"].is<const char*>() &&
      (strcmp(type, "text") == 0 || strcmp(type, "progress") == 0)) {
    if (strcmp(type, "progress") == 0) {
      lv_obj_set_style_bg_color(obj, hexToColor(props["color"]),
                                LV_PART_INDICATOR);
    } else {
      lv_obj_set_style_text_color(obj, hexToColor(props["color"]), 0);
    }
  }
  if (props["font"].is<const char*>() && (strcmp(type, "text") == 0 || strcmp(type, "button") == 0)) {
    lv_obj_set_style_text_font(obj, fontFromProp(props["font"]), 0);
    if (strcmp(type, "button") == 0) lv_obj_set_style_text_font(lv_obj_get_child(obj, 0), fontFromProp(props["font"]), 0);
  }
  if (strcmp(type, "text") == 0 && props["transition"] != "flip") {
    if (!props["pad_top"].isNull()) lv_obj_set_style_pad_top(obj, props["pad_top"] | 0, 0);
    if (props["long_mode"] == "clip") lv_label_set_long_mode(obj, LV_LABEL_LONG_CLIP);
  }
  if (props["clip_corner"].is<bool>()) lv_obj_set_style_clip_corner(obj, props["clip_corner"].as<bool>(), 0);
  if (props["align"].is<const char*>() && strcmp(type, "text") == 0) {
    const char* align = props["align"];
    if (strcmp(align, "center") == 0)
      lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, 0);
    else if (strcmp(align, "right") == 0)
      lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_RIGHT, 0);
    else
      lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, 0);
  }
  if (props["fallback_color"].is<const char*>()) {
    lv_obj_set_style_bg_color(obj, hexToColor(props["fallback_color"]), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
  }
  if (strcmp(type, "text") == 0 && props["transition"] != "flip" && props["text"].is<const char*>())
    lv_label_set_text(obj, props["text"]);
  if (props["transition"] == "flip") {
    FlipText::setPixelFont(obj,props["font_family"] != "sans");
    FlipText::setFontSize(obj,props["font_size"] | 0);
    lv_obj_set_style_clip_corner(obj, true, 0);
    if (props["text"].is<const char*>()) FlipText::set(obj,props["text"],false);
  }
  if (props["text_color"].is<const char*>() && strcmp(type, "button") == 0) {
    lv_obj_t* lab = lv_obj_get_child(obj, 0);
    if (lab)
      lv_obj_set_style_text_color(lab, hexToColor(props["text_color"]), 0);
  }
}

const lv_font_t* SDUIEngine::fontFromProp(const char* name) {
  if (!name) return &ui_font_14;
  if (!strcmp(name, "icons_20")) return &icons_20;
  if (!strcmp(name, "geist_10_400")) return &geist_10_400;
  if (!strcmp(name, "geist_12_400")) return &geist_12_400;
  if (!strcmp(name, "geist_12_600")) return &geist_12_600;
  if (!strcmp(name, "geist_14_400")) return &geist_14_400;
  if (!strcmp(name, "geist_18_600")) return &geist_18_600;
  if (!strcmp(name, "geist_28_700")) return &geist_28_700;
  if (strcmp(name, "font_48") == 0) return &lv_font_montserrat_48;
  if (strstr(name, "28") || strstr(name, "64") || strstr(name, "32"))
    return &ui_font_28;
  if (strstr(name, "12") || strstr(name, "10")) return &ui_font_12;
  return &ui_font_14;
}

lv_obj_t* SDUIEngine::createContainer(lv_obj_t* parent) {
  lv_obj_t* obj = lv_obj_create(parent);
  stripLvglDefaults(obj);
  lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
  return obj;
}

lv_obj_t* SDUIEngine::createText(lv_obj_t* parent) {
  lv_obj_t* obj = lv_label_create(parent);
  lv_label_set_text(obj, "");
  lv_label_set_long_mode(obj, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_color(obj, lv_color_hex(0xf4f4f5), 0);
  lv_obj_set_style_text_font(obj, &lv_font_montserrat_14, 0);
  return obj;
}

lv_obj_t* SDUIEngine::createButton(JsonObject node, lv_obj_t* parent) {
  lv_obj_t* obj = lv_btn_create(parent);
  stripLvglDefaults(obj);
  lv_obj_t* label = lv_label_create(obj);
  lv_label_set_text(label, node["props"]["text"] | "");
  lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
  lv_obj_center(label);
  return obj;
}

lv_obj_t* SDUIEngine::createImage(lv_obj_t* parent) {
  lv_obj_t* obj = lv_img_create(parent);
  stripLvglDefaults(obj);
  lv_obj_set_style_bg_color(obj, lv_color_hex(0x09090b), 0);
  lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
  lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
  return obj;
}

lv_obj_t* SDUIEngine::createProgress(lv_obj_t* parent) {
  lv_obj_t* obj = lv_bar_create(parent);
  lv_bar_set_range(obj, 0, 100);
  lv_bar_set_value(obj, 0, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(obj, lv_color_hex(0x2a2a30), LV_PART_MAIN);
  lv_obj_set_style_bg_color(obj, lv_color_hex(0xd97736), LV_PART_INDICATOR);
  lv_obj_set_style_radius(obj, 6, LV_PART_MAIN);
  lv_obj_set_style_radius(obj, 6, LV_PART_INDICATOR);
  return obj;
}

lv_obj_t* SDUIEngine::createRectangle(lv_obj_t* parent) {
  lv_obj_t* obj = lv_obj_create(parent);
  stripLvglDefaults(obj);
  return obj;
}

JsonVariant SDUIEngine::stateAt(const char* path) {
  JsonVariant v = _state.as<JsonVariant>();
  if (!path || !path[0]) return v;
  char buf[96];
  strncpy(buf, path, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = 0;
  char* save = nullptr;
  char* tok = strtok_r(buf, ".", &save);
  while (tok && !v.isNull()) {
    v = v[tok];
    tok = strtok_r(nullptr, ".", &save);
  }
  return v;
}

void SDUIEngine::setStatePath(const char* path, JsonVariant value) {
  if (!path) return;
  char buf[96];
  strncpy(buf, path, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = 0;
  JsonVariant cur = _state.as<JsonVariant>();
  char* save = nullptr;
  char* tok = strtok_r(buf, ".", &save);
  char* next = strtok_r(nullptr, ".", &save);
  while (tok && next) {
    if (!cur[tok].is<JsonObject>()) cur[tok].to<JsonObject>();
    cur = cur[tok];
    tok = next;
    next = strtok_r(nullptr, ".", &save);
  }
  if (tok) cur[tok].set(value);
}

static uint64_t monotonicMs() { return esp_timer_get_time() / 1000ULL; }
static uint64_t unixMs() {
  struct timeval tv {};
  gettimeofday(&tv, nullptr);
  return tv.tv_sec > 100000 ? uint64_t(tv.tv_sec) * 1000ULL + tv.tv_usec / 1000 : monotonicMs();
}

void SDUIEngine::formatBinding(const Bound& b, char* out, size_t outLen) {
  out[0] = 0;
  if (!b.source[0]) return;

  if (strcmp(b.source, "local.clock") == 0) {
    time_t t = time(nullptr);
    struct tm tmNow {};
    const char* fmt = b.format[0] ? b.format : "HH:mm";
    if(!strcmp(fmt,"sync_status")) { if(!ClockRuntime::valid(uint64_t(t)*1000)) snprintf(out,outLen,"Synchronizing time...");return; }
    if(!ClockRuntime::valid(uint64_t(t)*1000)) return;
    localtime_r(&t, &tmNow);
    if (strcmp(fmt, "a") == 0) { snprintf(out, outLen, "%s", tmNow.tm_hour >= 12 ? "PM" : "AM"); return; }
    if (strlen(fmt) == 4 && fmt[2] == '.' && (fmt[3] == '0' || fmt[3] == '1')) {
      const int part = strncmp(fmt,"ss",2)==0 ? tmNow.tm_sec : strncmp(fmt,"mm",2)==0 ? tmNow.tm_min :
        strncmp(fmt,"hh",2)==0 ? (tmNow.tm_hour%12 ? tmNow.tm_hour%12 : 12) : tmNow.tm_hour;
      snprintf(out,outLen,"%d",fmt[3]=='0' ? part/10 : part%10); return;
    }
    bool twelve = strstr(fmt, "hh") != nullptr;
    bool sec = strstr(fmt, "ss") != nullptr;
    int h = tmNow.tm_hour;
    if (twelve) {
      int h12 = h % 12;
      if (h12 == 0) h12 = 12;
      if (sec)
        snprintf(out, outLen, "%02d:%02d:%02d", h12, tmNow.tm_min, tmNow.tm_sec);
      else
        snprintf(out, outLen, "%02d:%02d", h12, tmNow.tm_min);
    } else if (sec) {
      snprintf(out, outLen, "%02d:%02d:%02d", h, tmNow.tm_min, tmNow.tm_sec);
    } else {
      snprintf(out, outLen, "%02d:%02d", h, tmNow.tm_min);
    }
    return;
  }

  if (strncmp(b.source, "local.elapsed_pct:", 18) == 0) {
    uint32_t every = b.intervalSec > 0 ? b.intervalSec : 1800;
    uint32_t elapsed = (millis() - b.intervalStartMs) / 1000;
    int pct = (int)((elapsed % every) * 100 / every);
    snprintf(out, outLen, "%d", pct);
    return;
  }

  const char* path = b.source;
  if (strncmp(path, "state.", 6) == 0) path += 6;
  JsonVariant v = stateAt(path);
  if (v.isNull()) return;
  if(v.is<JsonObject>() && strcmp(b.format,"checkable")==0) {
    snprintf(out,outLen,"%s",v["label"] | "");return;
  }
  if(v.is<JsonObject>() && strncmp(b.format,"interval.",9)==0) {
    IntervalRuntime::format(v.as<JsonObject>(),b.format,out,outLen);
    return;
  }
  if (v.is<JsonObject>() && strncmp(b.format,"date.",5)==0) {
    auto date=v.as<JsonObject>();
    if(strcmp(b.format,"date.title")==0) DateRuntime::title(date,out,outLen);
    else if(strncmp(b.format,"date.cell.",10)==0) {
      auto day=DateRuntime::cell(date,atoi(b.format+10));
      snprintf(out,outLen,"%d",day.day);
    }
    return;
  }

  if (v.is<JsonObject>() && strcmp(b.format, "mm:ss") == 0) {
    int remaining = CountdownRuntime::remainingSeconds(v.as<JsonObject>(), unixMs(), monotonicMs());
    snprintf(out, outLen, "%02d:%02d", remaining / 60, remaining % 60);
    return;
  }

  if (!strcmp(b.format,"swatch")) {
    snprintf(out,outLen," ");return;
  }
  if (v.is<const char*>()) {
    const char* text=v.as<const char*>();
    size_t length=strlen(text);
    if(length>=outLen) {
      length=outLen-1;
      while(length && (static_cast<unsigned char>(text[length])&0xc0)==0x80) --length;
    }
    memcpy(out,text,length);out[length]=0;
    return;
  }
  if (v.is<int>() || v.is<float>() || v.is<double>()) {
    snprintf(out, outLen, "%d", (int)v.as<int>());
  }
}

void SDUIEngine::refreshBindings() {
  char buf[192];
  for (int i = 0; i < _boundCount; i++) {
    Bound& b = _bound[i];
    if (!b.used || !b.obj) continue;
    if (b.kind == 3 && strncmp(b.source, "local.elapsed_pct:", 18) == 0) {
      formatBinding(b, buf, sizeof(buf));
      lv_bar_set_value(b.obj, atoi(buf), LV_ANIM_OFF);
      continue;
    }
    if (b.colorSource[0]) {
      const char* path = strncmp(b.colorSource,"state.",6)==0 ? b.colorSource+6 : b.colorSource;
      const char* color = stateAt(path) | "#71717a";
      auto* target = b.label ? b.label : b.obj;
      const auto nextColor = hexToColor(color);
      if (lv_color_to32(lv_obj_get_style_text_color(target, 0)) != lv_color_to32(nextColor))
        lv_obj_set_style_text_color(target, nextColor, 0);
    }
    if (!b.source[0]) continue;
    if (b.kind == 6 || b.kind == 7 || b.kind == 8) {
      const char* path = strncmp(b.source,"state.",6)==0 ? b.source+6 : b.source;
      auto control = stateAt(path);
      const bool enabled = control["enabled"] | false;
      if (enabled) lv_obj_clear_state(b.obj, LV_STATE_DISABLED);
      else lv_obj_add_state(b.obj, LV_STATE_DISABLED);
      if (b.kind == 6 && !lv_dropdown_is_open(b.obj)) {
        std::string options;
        for (JsonObject option : control["options"].as<JsonArray>()) {
          if (!options.empty()) options += '\n';
          options += option["label"] | "";
        }
        if (options.empty()) { options = "No modes available"; lv_obj_add_state(b.obj, LV_STATE_DISABLED); }
        if (options != lv_dropdown_get_options(b.obj)) lv_dropdown_set_options(b.obj, options.c_str());
        lv_dropdown_set_selected(b.obj, control["selected"] | 0);
      } else if (b.kind == 7 && !lv_obj_has_state(b.obj, LV_STATE_PRESSED)) {
        const auto nextColor = hexToColor(control["color"] | "#ffb86b");
        if (lv_color_to32(lv_colorwheel_get_rgb(b.obj)) != lv_color_to32(nextColor))
          lv_colorwheel_set_rgb(b.obj, nextColor);
      }
      continue;
    }
    if(!strcmp(b.source,"local.clock") && strcmp(b.format,"sync_status")) {
      if(ClockRuntime::valid(uint64_t(time(nullptr))*1000)) lv_obj_clear_flag(b.obj,LV_OBJ_FLAG_HIDDEN);
      else lv_obj_add_flag(b.obj,LV_OBJ_FLAG_HIDDEN);
    }
    if(b.kind==5) {
      const char* path=strncmp(b.source,"state.",6)==0?b.source+6:b.source;
      InlineImage::update(b.obj,stateAt(path).as<const char*>());continue;
    }
    formatBinding(b, buf, sizeof(buf));

    if (b.kind == 4) {
      if (strcmp(buf,b.lastText) != 0) {
        if (FlipText::set(b.obj,buf,b.lastText[0] != 0))
          snprintf(b.lastText,sizeof(b.lastText),"%s",buf);
      }
    } else if (b.kind == 1 && b.obj) {
      const bool changed = strcmp(buf, lv_label_get_text(b.obj)) != 0;
      if (!changed) continue;
      lv_label_set_text(b.obj, buf);
      if (b.transition && changed && b.lastText[0])
        applyTransition(b.obj, b.transition);
      strncpy(b.lastText, buf, sizeof(b.lastText) - 1);
      b.lastText[sizeof(b.lastText) - 1] = 0;
    } else if (b.kind == 2 && b.label) {
      if (b.actionDataSource[0]) {
        const char* command = stateAt(b.actionDataSource) | "";
        if (command[0]) lv_obj_clear_state(b.obj, LV_STATE_DISABLED);
        else lv_obj_add_state(b.obj, LV_STATE_DISABLED);
      }
      if(strcmp(b.format,"checkable")==0) {
        const char* path=strncmp(b.source,"state.",6)==0?b.source+6:b.source;
        const bool completed=stateAt(path)["completed"] | false;
        lv_obj_set_style_text_decor(b.label,completed?LV_TEXT_DECOR_STRIKETHROUGH:LV_TEXT_DECOR_NONE,0);
        lv_obj_set_style_text_opa(b.label,completed?LV_OPA_50:LV_OPA_COVER,0);
      }
      if(strcmp(b.format,"optional")==0 || strcmp(b.format,"checkable")==0 || strcmp(b.format,"swatch")==0) {
        if(buf[0]) lv_obj_clear_flag(b.obj,LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(b.obj,LV_OBJ_FLAG_HIDDEN);
      }
      if (strncmp(b.format,"date.cell.",10)==0) {
        const char* path=strncmp(b.source,"state.",6)==0 ? b.source+6 : b.source;
        auto date=stateAt(path).as<JsonObject>();
        auto day=DateRuntime::cell(date,atoi(b.format+10));char iso[16];DateRuntime::iso(day,iso,sizeof(iso));
        const bool selected=strcmp(date["selectedDate"] | "",iso)==0;
        const bool outside=day.month != date["month"].as<int>();
        const int style=selected?2:outside?1:0;
        // Avoid style invalidation every 250 ms when the date did not change.
        if (b.lastText[0] != '0'+style) {
          b.lastText[0]='0'+style;
          lv_obj_set_style_bg_opa(b.obj,selected?LV_OPA_COVER:LV_OPA_TRANSP,0);
          lv_obj_set_style_bg_color(b.obj,lv_color_hex(0x54311c),0);
          lv_obj_set_style_text_color(b.label,lv_color_hex(selected?0xd97736:outside?0x71717a:0xf4f4f5),0);
        }
      }
      const char* text = buf[0] ? buf : " ";
      if (strcmp(text, lv_label_get_text(b.label)) != 0)
        lv_label_set_text(b.label, text);
    } else if (b.kind == 3 && b.obj) {
      const char* path = b.source;
      if (strncmp(path, "state.", 6) == 0) path += 6;
      JsonVariant v = stateAt(path);
      int pct = 0;
      if (v.is<int>() || v.is<float>()) pct = (int)v.as<float>();
      else if (buf[0]) pct = atoi(buf);
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
      lv_bar_set_value(b.obj, pct, LV_ANIM_OFF);
    }
  }
}

void SDUIEngine::toggleCountdown(const char* widgetId) {
  if (!widgetId) return;
  JsonVariant w = _state[widgetId];
  if (w.isNull()) return;
  if (!w["value"].is<JsonObject>()) return;
  JsonObject val = w["value"].as<JsonObject>();
  CountdownRuntime::toggle(val, unixMs(), monotonicMs());
  w["btn_label"] = val["mode"] == "countdown" ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY;
}

void SDUIEngine::captureButtonIndex(int index) {
  if(index<0||index>=_boundCount) return;
  auto& b=_bound[index];
  b.captured=true;
  const char* value=stateAt(b.actionDataSource) | "";
  // Reject an oversized ID instead of completing a different, truncated ID.
  snprintf(b.pressedData,sizeof(b.pressedData),"%s",strlen(value)<sizeof(b.pressedData)?value:"");
}

void SDUIEngine::onButtonIndex(int index) {
  if (index < 0 || index >= _boundCount) return;
  Bound& b = _bound[index];
  if (b.kind == 6 || b.kind == 7 || b.kind == 8) {
    const char* path = strncmp(b.source,"state.",6)==0 ? b.source+6 : b.source;
    auto control = stateAt(path);
    if (!(control["enabled"] | false)) return;
    SduiActionRequest request{};
    snprintf(request.widgetId,sizeof(request.widgetId),"%s",b.actionPath);
    snprintf(request.nodeId,sizeof(request.nodeId),"%s",b.id);
    snprintf(request.name,sizeof(request.name),"%s",b.actionName);
    if (b.kind == 6) {
      const unsigned selected = lv_dropdown_get_selected(b.obj);
      const char* value = control["options"][selected]["value"] | "";
      if (!value[0] || strlen(value) >= sizeof(request.dataId)) return;
      snprintf(request.dataId,sizeof(request.dataId),"%s",value);
      control["selected"] = selected;
    } else if (b.kind == 8) {
      auto* input = lv_indev_get_act();
      if (!input) return;
      lv_point_t point; lv_indev_get_point(input, &point);
      lv_area_t bounds; lv_obj_get_coords(b.obj, &bounds);
      const float radiusX = lv_obj_get_width(b.obj) / 2.0f;
      const float radiusY = lv_obj_get_height(b.obj) / 2.0f;
      if (radiusX <= 0 || radiusY <= 0) return;
      uint32_t color;
      if (!ColorDisc::pick((point.x - bounds.x1 - radiusX) / radiusX,
                           (point.y - bounds.y1 - radiusY) / radiusY, color)) return;
      snprintf(request.dataId, sizeof(request.dataId), "%s%06lx", b.commandPrefix, (unsigned long)color);
    } else {
      auto hsv = lv_colorwheel_get_hsv(b.obj);
      hsv.s = 100; hsv.v = 100;
      const auto color = lv_color_to32(lv_color_hsv_to_rgb(hsv.h, hsv.s, hsv.v));
      snprintf(request.dataId,sizeof(request.dataId),"%s%02x%02x%02x",b.commandPrefix,
        unsigned((color >> 16) & 255),unsigned((color >> 8) & 255),unsigned(color & 255));
    }
    const bool queued = sduiActionQueue && xQueueSend(sduiActionQueue,&request,0)==pdTRUE;
    _state[b.actionPath]["status"] = queued ? "Saving..." : "Busy. Tap again.";
    return;
  }
  if(strcmp(b.actionType,"local")==0 && strncmp(b.actionName,"interval.",9)==0) {
    auto interval=stateAt(b.actionPath).as<JsonObject>();
    if(!interval.isNull()) IntervalRuntime::action(interval,monotonicMs(),b.actionName);
    _lastTickMs=millis()-250;tick();return;
  }
  if(strcmp(b.actionType,"event")==0 && b.actionDataSource[0]) {
    if(!b.captured) captureButtonIndex(index);
    b.captured=false;
    if(!b.pressedData[0]) return;
    SduiActionRequest request;
    snprintf(request.widgetId,sizeof(request.widgetId),"%s",b.actionPath);
    snprintf(request.nodeId,sizeof(request.nodeId),"%s",b.id);
    snprintf(request.name,sizeof(request.name),"%s",b.actionName);
    snprintf(request.dataId,sizeof(request.dataId),"%s",b.pressedData);
    const bool queued=sduiActionQueue && xQueueSend(sduiActionQueue,&request,0)==pdTRUE;
    _state[b.actionPath]["status"]=queued?"Saving...":"Busy. Tap again.";
    refreshBindings();return;
  }
  if (strcmp(b.actionType,"local")==0 && strncmp(b.actionName,"date.",5)==0) {
    auto date=stateAt(b.actionPath).as<JsonObject>();
    if(date.isNull()) return;
    if(strcmp(b.actionName,"date.shift")==0) DateRuntime::move(date,b.actionValue);
    else if(strcmp(b.actionName,"date.select")==0) DateRuntime::select(date,b.actionValue);
    refreshBindings();
    return; // Local date browsing does not invoke a remote widget action.
  }
  if (strcmp(b.actionType, "local") == 0 &&
      strcmp(b.actionName, "toggle_countdown") == 0) {
    toggleCountdown(b.actionPath[0] ? b.actionPath : b.id);
    refreshBindings();
    return;
  }
  if (strcmp(b.actionType, "local") == 0 && strcmp(b.actionName, "reset_countdown") == 0) {
    JsonVariant w = _state[b.actionPath];
    if (w["value"].is<JsonObject>()) {
      CountdownRuntime::reset(w["value"].as<JsonObject>());
      w["btn_label"] = LV_SYMBOL_PLAY;
      w["progress"] = 100;
      refreshBindings();
    }
    return;
  }
  if (strncmp(b.actionName, "media.", 6) == 0) {
    JsonDocument event;
    event["protocol"] = "macropad.media";
    event["action"] = b.actionName + 6;
    SerialBridge::send(event);
    return;
  }
  if (strncmp(b.actionName, "obs.", 4) == 0) {
    JsonDocument event;
    event["protocol"] = "macropad.obs";
    event["action"] = b.actionName + 4;
    event["index"] = b.actionValue;
    SerialBridge::send(event);
    return;
  }
  if (strcmp(b.actionName, "hotkey") == 0) {
    JsonDocument event;
    event["protocol"] = "macropad.hotkey";
    event["code"] = b.actionValue;
    SerialBridge::send(event);
    return;
  }
  Serial.printf("SDUI event widget=%s node=%s\n", b.actionPath, b.id);
  if (actionQueue) {
    char action[64];
    snprintf(action, sizeof(action), "SDUI_EVENT:%s",
             b.actionPath[0] ? b.actionPath : b.id);
    xQueueSend(actionQueue, &action, 0);
  }
}

bool SDUIEngine::applyPatch(const char* jsonPatch) {
  if (!jsonPatch) return false;
  SceneDocument doc;
  if (deserializeJson(doc, jsonPatch, DeserializationOption::NestingLimit(SCENE_NESTING_LIMIT))) return false;
  if (doc["type"] != "patch") return false;
  JsonArray ops = doc["payload"]["operations"].as<JsonArray>();
  if (ops.isNull()) return false;
  for (JsonObject op : ops) {
    const char* target = op["target"] | "";
    const char* path = op["path"] | "";
    if (strcmp(target, "state") != 0 || !path[0]) continue;
    setStatePath(path, op["value"].as<JsonVariant>());
  }
  refreshBindings();
  SerialBridge::applied++;
  return true;
}

void SDUIEngine::tick() {
  ImageSequence::tick();
  RemoteImage::tick();
  if (_loadingPage >= 0 && _syncLabel) {
    const uint32_t elapsed = millis() - _pageLoadingAt;
    if (elapsed >= 700) {
      const char* text = elapsed >= 15000 ? "Still loading. Swipe to choose another screen." : "Loading\xE2\x80\xA6";
      if (strcmp(lv_label_get_text(_syncLabel), text)) lv_label_set_text(_syncLabel, text);
      lv_obj_clear_flag(_syncLabel, LV_OBJ_FLAG_HIDDEN);
    }
  }
  if (_overlaysHeldAt && millis() - _overlaysHeldAt > 15000) {
    // Live widgets (WLED, media, …) are not in the chrome bake. If the background
    // never arrives, still show their SDUI controls instead of an empty tile.
    if (_overlaysHidden) {
      setLiveOverlaysHidden(false);
      _overlaysHidden = false;
    }
    if (_syncLabel) lv_label_set_text(_syncLabel, "Waiting for connection. Swipe to another screen.");
    _overlaysHeldAt = 0;
  }
  // Rebuild outside LVGL's input callback, after suppressing click-on-release.
  if (_pendingStep) {
    const int step = _pendingStep;
    _pendingStep = 0;
    showPage((_loadingPage >= 0 ? _loadingPage : _pageIndex) + step);
  }
  uint32_t now = millis();
  if (now - _lastTickMs < 250) return;
  _lastTickMs = now;

  JsonObject root = _state.as<JsonObject>();
  int reminderPage=-1;char reminderText[96]={};
  if (!root.isNull()) {
    for (JsonPair kv : root) {
      JsonVariant w = kv.value();
      if (!w.is<JsonObject>()) continue;
      if(w["interval"].is<JsonObject>()) {
        auto interval=w["interval"].as<JsonObject>();
        time_t nowTime=time(nullptr);struct tm current {};
        const bool hasTime=nowTime>100000 && localtime_r(&nowTime,&current);
        IntervalRuntime::tick(interval,monotonicMs(),hasTime?current.tm_hour:-1);
        w["progress"]=IntervalRuntime::progress(interval);
        if((interval["alerting"] | false) && !(interval["quiet"] | false) && reminderPage<0) {
          auto pages=_scene["pages"].as<JsonArray>();
          for(unsigned p=0;p<pages.size();++p) for(JsonObject n:pages[p]["nodes"].as<JsonArray>())
            if(n["id"]==kv.key().c_str()) reminderPage=p;
          snprintf(reminderText,sizeof(reminderText),"%s",(interval["active"] | false)?"Session complete":(w["value"] | "Time to stretch"));
        }
      }
      JsonVariant val = w["value"];
      if (!val.is<JsonObject>()) continue;
      const char* mode = val["mode"] | "";
      if (strcmp(mode, "countdown") != 0) continue;
      CountdownRuntime::tick(val.as<JsonObject>(), unixMs(), monotonicMs());
      const int left = val["remainingSec"] | 0;
      const int duration = val["durationSec"] | 1;
      w["progress"] = duration > 0 ? int(int64_t(left) * 100 / duration) : 0;
      if (!left) w["btn_label"] = LV_SYMBOL_PLAY;
    }
  }
  if(reminderPage!=_reminderPage || strcmp(reminderText,_reminderText)) {
    _reminderPage=reminderPage;snprintf(_reminderText,sizeof(_reminderText),"%s",reminderText);
    updatePageLabel();
  }
  refreshBindings();
}

uint8_t SDUIEngine::transitionFromProp(const char* name) {
  if (!name || !name[0]) return 0;
  if (strcmp(name, "flip") == 0) return 1;
  if (strcmp(name, "fade") == 0) return 2;
  if (strcmp(name, "slide") == 0) return 3;
  return 0;
}

void SDUIEngine::applyTransition(lv_obj_t* obj, uint8_t transition) {
  if (!obj || !transition) return;
  // Cancel any in-flight animation on this object before starting a new one so
  // overlapping value changes never leave a stale anim pointing at it.
  lv_anim_del(obj, anim_opa_cb);
  lv_anim_del(obj, anim_trans_y_cb);
  lv_anim_del(obj, anim_trans_x_cb);

  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, obj);
  lv_anim_set_time(&a, 200);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  if (transition == 2) {  // fade
    lv_anim_set_values(&a, LV_OPA_40, LV_OPA_COVER);
    lv_anim_set_exec_cb(&a, anim_opa_cb);
    lv_anim_start(&a);

  } else if (transition == 3) {  // slide from the right
    lv_anim_set_values(&a, 14, 0);
    lv_anim_set_exec_cb(&a, anim_trans_x_cb);
    lv_anim_start(&a);
  }
}

lv_color_t SDUIEngine::hexToColor(const char* hex) {
  if (!hex || strlen(hex) < 7 || hex[0] != '#') return lv_color_black();
  uint32_t val = strtoul(hex + 1, nullptr, 16);
  return lv_color_hex(val);
}

void SDUIEngine::sendChannels() {
  JsonDocument message;message["protocol"]="sdui";message["type"]="channels";
  message["payload"].set(_scene["channels"]);
  message["payload"]["scene_generation"]=_generation;
  message["payload"]["usb_patches"]=SerialBridge::patches.load();
  message["payload"]["usb_invalid"]=SerialBridge::invalid.load();
  message["payload"]["usb_dropped"]=SerialBridge::dropped.load();
  message["payload"]["patches_applied"]=SerialBridge::applied.load();
  unsigned ready=0, artwork=0;
  for(JsonVariant id : _scene["channels"]["system_media"].as<JsonArray>()) {
    auto state=_state[id.as<const char*>()];
    const char* title=state["title"] | "";
    if(title[0] && strcmp(title,"Not playing")) ready++;
    if(strlen(state["art"] | "")==6400) artwork++;
  }
  message["payload"]["media_metadata_ready"]=ready;
  message["payload"]["media_artwork_ready"]=artwork;
  SerialBridge::send(message);
}
