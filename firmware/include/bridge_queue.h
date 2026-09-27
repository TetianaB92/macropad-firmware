#pragma once

#include <stddef.h>
#include <stdint.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

enum class UiEventType {
  ShowPairingSuccess,
  ApplyLayout,
  ApplyWidgetData,
  ApplyRenderedScreen,
  ApplySduiScene,
  ApplySduiPatch,
  ApplySduiChrome,
  PageLoadFailed,
  ConnectionStatus,
  ApplyPhoto,
  ApplySnapshot,
  DescribeChannels,
  EncoderInput,
  NavigatePage,
};

enum class RenderedScreenFormat : uint8_t {
  Jpeg,
  Rgb565,
};

struct UiEvent {
  UiEventType type;
  uint8_t* imageBytes;
  size_t imageLen;
  RenderedScreenFormat imageFormat;
  uint32_t generation = 0;
  char pageId[64] = {};
  int status = 0;
};

extern QueueHandle_t actionQueue;
extern QueueHandle_t uiQueue;

// Latest desired page replaces an older unprocessed swipe request.
struct PageRequest {
  uint32_t generation;
  bool chromeReady = false;
  uint32_t chromeRefreshMs=30000;
  uint32_t stateRefreshMs=30000;
  char pageId[64];
};
extern QueueHandle_t pageRequestQueue;

struct SduiActionRequest {
  char widgetId[64] = {};
  char nodeId[96] = {};
  char name[32] = {};
  char dataId[128] = {};
};
extern QueueHandle_t sduiActionQueue;

struct PhotoRequest { uint32_t ticket; uint16_t width, height; char assetId[65]; };
extern QueueHandle_t photoRequestQueue;

extern QueueHandle_t serialTxQueue;

struct SceneResult { uint32_t generation; bool applied; };
extern QueueHandle_t sceneResultQueue;

// Generic SDUI JPEG image requests; no camera credentials or API token.
struct SnapshotRequest { uint32_t ticket=0, epoch=0; uint16_t width=0, height=0; bool cover=false; char url[512]={}; };
extern QueueHandle_t snapshotRequestQueue;
