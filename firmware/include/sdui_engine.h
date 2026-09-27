#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include "json_memory.h"
#include <lvgl.h>

class SDUIEngine {
public:
  SDUIEngine();
  ~SDUIEngine();

  void init(lv_obj_t* parentScreen);
  /** Re-point the engine at a freshly-cleared host and drop all stale object
   * bindings. Call this whenever the LVGL screen is wiped (boot/pairing/etc.)
   * so tick()/refreshBindings() never touch freed objects. Preserves _state. */
  void attachHost(lv_obj_t* host);
  void sendChannels();
  bool parseScene(const char* jsonString, uint32_t generation = 0);
  void swipe(int step);
  bool applyPatch(const char* jsonPatch);
  void tick();
  void onButtonIndex(int index);
  void captureButtonIndex(int index);
  void openReminder();
  lv_obj_t* getRoot() { return _root; }
  lv_obj_t* chromeImage() { return _chrome; }
  /** Feed a full-screen RGB565 chrome bake. Device owns the bytes afterwards.
   *  LVGL uses the retained pixels as its display background. */
  void setChromeImage(uint8_t* rgb565, int w, int h, const char* pageId, uint32_t generation);
  /** Attach a pending chrome bake and invalidate the scene for composition. */
  void presentChrome();
  void pageLoadFailed(const char* pageId, uint32_t generation, int status);
  bool isMounted() const { return _mounted; }
  void setConnectionStatus(const char* status);

private:
  static const int kMaxBound = 1024;

  struct Bound {
    bool used;
    char id[48];
    char source[96];
    char colorSource[96];
    char commandPrefix[24];
    char format[24];
    char actionType[12];
    char actionName[32];
    char actionPath[48];
    char actionDataSource[96];
    char pressedData[128];
    bool captured;
    int actionValue;
    uint8_t kind;        // 0 none 1 text 2 button 3 progress
    uint8_t transition;  // 0 none 1 flip 2 fade 3 slide
    char lastText[40];
    uint32_t intervalSec;
    uint32_t intervalStartMs;
    lv_obj_t* obj;
    lv_obj_t* label;
  };

  lv_obj_t* _root;
  lv_obj_t* _host;
  lv_obj_t* _chrome;
  uint8_t* _chromeBytes;
  lv_img_dsc_t _chromeDsc;
  bool _chromeDirty;
  SceneDocument _state;
  SceneDocument _scene;
  uint64_t _sceneVersion = 0;
  uint32_t _generation = 0;
  int _pageIndex = 0;
  int _pendingStep = 0;
  int _loadingPage = -1;
  uint32_t _pageLoadingAt = 0;
  bool _mounted = false;
  lv_obj_t* _pageLabel = nullptr;
  lv_obj_t* _syncLabel = nullptr;
  int _pageError = 0;
  char _connectionStatus[96] = {};
  char _reminderText[96] = {};
  int _reminderPage = -1;
  uint32_t _overlaysHeldAt = 0;
  // Fingerprint of the pixels currently sitting in the scan-out framebuffer so
  // attachPageBackground() only re-blits when the chrome actually changed. The
  // cache buffers are immutable, so pointer identity is a safe change test.
  const uint8_t* _presentedChrome = nullptr;
  int _presentedW = 0;
  int _presentedH = 0;
  bool _presentedFill = false;
  bool _overlaysHidden = false;
  bool _chromeBgAttached = false;
  static constexpr int kPageCacheSize = 6;
  struct CachedPage {
    char id[64] = {};
    uint8_t* bytes = nullptr;
    int w = 0;
    int h = 0;
    uint32_t used = 0;
  };
  CachedPage _pageCache[kPageCacheSize];
  uint32_t _cacheClock = 0;
  const char* currentPageId();
  void showPage(int index, bool chromeReady = false);
  void clearPageCache();
  void attachPageBackground(bool overlaysRebuilt = false);
  void adoptSceneGeneration(uint32_t generation);
  void restoreChromeFromFlash();
  void updatePageLabel();
  void setLiveOverlaysHidden(bool hidden);
  Bound* _bound = nullptr;
  int _boundCapacity = 0;
  int _boundCount;
  uint32_t _lastTickMs;

  void clearBindings();
  lv_obj_t* createNode(JsonObject node, lv_obj_t* parent);
  lv_obj_t* createContainer(lv_obj_t* parent);
  lv_obj_t* createText(lv_obj_t* parent);
  lv_obj_t* createButton(JsonObject node, lv_obj_t* parent);
  lv_obj_t* createImage(lv_obj_t* parent);
  lv_obj_t* createProgress(lv_obj_t* parent);
  lv_obj_t* createRectangle(lv_obj_t* parent);
  void applyLayout(lv_obj_t* obj, JsonObject layout);
  void applyProps(lv_obj_t* obj, JsonObject props, const char* type);
  void remember(JsonObject node, lv_obj_t* obj, lv_obj_t* label, uint8_t kind);
  void refreshBindings();
  void formatBinding(const Bound& b, char* out, size_t outLen);
  JsonVariant stateAt(const char* path);
  void setStatePath(const char* path, JsonVariant value);
  void toggleCountdown(const char* widgetId);
  lv_color_t hexToColor(const char* hex);
  const lv_font_t* fontFromProp(const char* name);
  static uint8_t transitionFromProp(const char* name);
  static void applyTransition(lv_obj_t* obj, uint8_t transition);
  void reattachChrome();
};

extern SDUIEngine* g_sdui;
