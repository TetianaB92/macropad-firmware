#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <time.h>
#include <stdlib.h>

#include "board_config.h"
#include "bridge_queue.h"
#include "config_store.h"
#include "device_state.h"
#include "display_manager.h"
#include "encoder_manager.h"
#include "frame_buffer.h"
#include "hid_actions.h"
#include "media_pipeline.h"
#include "network_manager.h"
#include "provisioning.h"
#include "ui_builder.h"
#include "wifi_config.h"
#include "clock_runtime.h"
#include "image_sequence.h"
#include "remote_image.h"
#include "json_memory.h"
#include "serial_bridge.h"

QueueHandle_t actionQueue;
QueueHandle_t uiQueue;
QueueHandle_t pageRequestQueue;
QueueHandle_t sduiActionQueue;
QueueHandle_t photoRequestQueue;
QueueHandle_t snapshotRequestQueue;
QueueHandle_t serialTxQueue;
QueueHandle_t sceneResultQueue;
static void rememberSceneEnvelope(const char* json) {
  if (!json) return;
  SceneDocument meta;
  if (deserializeJson(meta, json, DeserializationOption::NestingLimit(SCENE_NESTING_LIMIT))) return;
  const char* updated = meta["updated_at"] | "";
  if (updated[0]) DeviceState::setLayoutUpdatedAt(updated);
  DeviceState::setHasLayout(true);
}

static void applyScene(const UiEvent& event) {
  const char* json = reinterpret_cast<const char*>(event.imageBytes);
  const bool applied=UIBuilder::loadSDUIScene(json,event.generation);
  if (applied) rememberSceneEnvelope(json);
  SceneResult result {event.generation,applied};if(sceneResultQueue) xQueueOverwrite(sceneResultQueue,&result);
}

void NetworkTask(void* pvParameters) {
  (void)pvParameters;
  DeviceNetworkManager::init();
  for (;;) {
    SduiActionRequest request;
    if (xQueueReceive(sduiActionQueue, &request, 0) == pdTRUE)
      DeviceNetworkManager::handleSduiAction(request);

    char action[64];
    if (xQueueReceive(actionQueue, &action, 0) == pdTRUE) {
      DeviceNetworkManager::handleAction(action);
    }
    DeviceNetworkManager::loop();
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void GuiAndInputTask(void* pvParameters) {
  (void)pvParameters;
  // #region debug-point A:gui-init-sequence
  Serial.println("[DEBUG] GuiAndInputTask: DisplayManager::init enter");
  // #endregion
  DisplayManager::init();
  // #region debug-point A:gui-init-sequence
  Serial.println("[DEBUG] GuiAndInputTask: DisplayManager::init done");
  // #endregion
  // #region debug-point B:gui-init-sequence
  Serial.println("[DEBUG] GuiAndInputTask: MediaPipeline::init enter");
  // #endregion
  MediaPipeline::init();
  // #region debug-point B:gui-init-sequence
  Serial.println("[DEBUG] GuiAndInputTask: MediaPipeline::init done");
  // #endregion
  // #region debug-point C:gui-init-sequence
  Serial.println("[DEBUG] GuiAndInputTask: UIBuilder::init enter");
  // #endregion
  UIBuilder::init();
  // #region debug-point C:gui-init-sequence
  Serial.println("[DEBUG] GuiAndInputTask: UIBuilder::init done");
  // #endregion
  // #region debug-point D:gui-init-sequence
  Serial.println("[DEBUG] GuiAndInputTask: EncoderManager::init enter");
  // #endregion
  EncoderManager::init();
  // #region debug-point D:gui-init-sequence
  Serial.println("[DEBUG] GuiAndInputTask: EncoderManager::init done");
  // #endregion
  // #region debug-point E:gui-init-sequence
  Serial.println("[DEBUG] GuiAndInputTask: HidActions::init enter");
  // #endregion
  HidActions::init();
  // #region debug-point E:gui-init-sequence
  Serial.println("[DEBUG] GuiAndInputTask: HidActions::init done");
  // #endregion

  // Show a visible connection state until the network supplies a matching
  // scene and background; cached scenes may contain no live nodes.
  UIBuilder::showBoot("Connecting Wi-Fi...");
  String cachedScene;
  if (ConfigStore::loadScene(cachedScene)) {
    // Local widgets can run while networking recovers; the fresh scene replaces this generation.
    UIBuilder::loadSDUIScene(cachedScene.c_str(),0);
    rememberSceneEnvelope(cachedScene.c_str());
    UIBuilder::getSDUIEngine().setConnectionStatus("Offline / reconnecting...");
  } else if (ConfigStore::loadFromFlash(DeviceState::layoutDoc())) {
    DeviceState::setHasLayout(true);
    EncoderManager::applyFromLayout(DeviceState::layoutDoc());
    UIBuilder::showBoot("Connecting Wi‑Fi…");
  } else {
    UIBuilder::showBoot("Connecting Wi‑Fi…");
  }

  for (;;) {
    EncoderManager::loop(actionQueue);
    MediaPipeline::loop();

    static bool deferLayoutApply = false;
    static bool deferWidgetApply = false;
    static UiEvent deferredScene {};
    static uint32_t successShownAt = 0;
    static const uint32_t kSuccessOverlayMs = 1500;
    static uint8_t* deferredImageBytes = nullptr;
    static size_t deferredImageLen = 0;
    static RenderedScreenFormat deferredImageFormat =
        RenderedScreenFormat::Jpeg;

    UiEvent uiEvent {};
    while (xQueueReceive(uiQueue, &uiEvent, 0) == pdTRUE) {
      if (uiEvent.type == UiEventType::ConnectionStatus) {
        const char* message=reinterpret_cast<const char*>(uiEvent.imageBytes);
        if(message) {
          auto& engine=UIBuilder::getSDUIEngine();
          if(engine.isMounted()) engine.setConnectionStatus(message);
          else if(message[0]) UIBuilder::showBoot(message);
          releaseFrameBuffer(uiEvent.imageBytes);
        }
      } else if (uiEvent.type == UiEventType::ShowPairingSuccess) {
        UIBuilder::showPairedSuccess();
        successShownAt = millis();
      } else if (uiEvent.type == UiEventType::ApplyLayout) {
        if (successShownAt != 0 &&
            millis() - successShownAt < kSuccessOverlayMs) {
          deferLayoutApply = true;
        } else {
          // SDUI PHASE 1: Disable old layout rendering so it doesn't overwrite our mock scene
          // UIBuilder::buildFromLayout(DeviceState::layoutDoc());
          EncoderManager::applyFromLayout(DeviceState::layoutDoc());
        }
      } else if (uiEvent.type == UiEventType::ApplyWidgetData) {
        if (successShownAt != 0 &&
            millis() - successShownAt < kSuccessOverlayMs) {
          deferWidgetApply = true;
        } else {
          // SDUI PHASE 1: Disable old widget data updating
          // UIBuilder::updateLiveData(DeviceState::widgetDataDoc());
        }
      } else if (uiEvent.type == UiEventType::ApplySduiScene) {
        if (successShownAt != 0 && millis() - successShownAt < kSuccessOverlayMs) {
          releaseFrameBuffer(deferredScene.imageBytes);
          deferredScene = uiEvent;
        } else if (uiEvent.imageBytes) {
          applyScene(uiEvent);
          releaseFrameBuffer(uiEvent.imageBytes);
        }
      } else if (uiEvent.type == UiEventType::ApplySduiPatch) {
        if (uiEvent.imageBytes) {
          UIBuilder::getSDUIEngine().applyPatch(
              reinterpret_cast<const char*>(uiEvent.imageBytes));
          releaseFrameBuffer(uiEvent.imageBytes);
        }
      } else if (uiEvent.type == UiEventType::NavigatePage) {
        UIBuilder::getSDUIEngine().swipe(uiEvent.status);
      } else if (uiEvent.type == UiEventType::EncoderInput) {
        if(uiEvent.imageBytes) EncoderManager::handleSimLine(actionQueue,reinterpret_cast<const char*>(uiEvent.imageBytes));
        releaseFrameBuffer(uiEvent.imageBytes);
      } else if (uiEvent.type == UiEventType::DescribeChannels) {
        UIBuilder::getSDUIEngine().sendChannels();
      } else if (uiEvent.type == UiEventType::ApplySnapshot) {
        RemoteImage::receive(uiEvent.generation, uiEvent.imageBytes, uiEvent.imageLen);
      } else if (uiEvent.type == UiEventType::ApplyPhoto) {
        ImageSequence::receive(uiEvent.generation, uiEvent.imageBytes, uiEvent.imageLen);
      } else if (uiEvent.type == UiEventType::PageLoadFailed) {
        UIBuilder::getSDUIEngine().pageLoadFailed(uiEvent.pageId, uiEvent.generation, uiEvent.status);
      } else if (uiEvent.type == UiEventType::ApplySduiChrome) {
        // Full-screen static bake that sits *behind* the live SDUI overlays.
        if (uiEvent.imageBytes && uiEvent.imageLen > 0) {
          if (!UIBuilder::applyChromeRgb565(uiEvent.imageBytes,
                                            uiEvent.imageLen, uiEvent.pageId, uiEvent.generation)) {
            Serial.println("Chrome apply failed");
            releaseFrameBuffer(uiEvent.imageBytes);
          }
        } else if (uiEvent.imageBytes) {
          releaseFrameBuffer(uiEvent.imageBytes);
        }
      } else if (uiEvent.type == UiEventType::ApplyRenderedScreen) {
        const bool canShowNow =
            successShownAt == 0 || millis() - successShownAt >= kSuccessOverlayMs;
        if (canShowNow && uiEvent.imageBytes && uiEvent.imageLen > 0) {
          const bool shown = uiEvent.imageFormat == RenderedScreenFormat::Rgb565
                                 ? UIBuilder::showRenderedScreenRgb565(
                                       uiEvent.imageBytes, uiEvent.imageLen)
                                 : UIBuilder::showRenderedScreen(
                                       uiEvent.imageBytes, uiEvent.imageLen);
          if (!shown) {
            Serial.println("Rendered screen apply failed");
            releaseFrameBuffer(uiEvent.imageBytes);
          }
        } else if (uiEvent.imageBytes && uiEvent.imageLen > 0) {
          if (deferredImageBytes) releaseFrameBuffer(deferredImageBytes);
          deferredImageBytes = uiEvent.imageBytes;
          deferredImageLen = uiEvent.imageLen;
          deferredImageFormat = uiEvent.imageFormat;
        } else if (uiEvent.imageBytes) {
          releaseFrameBuffer(uiEvent.imageBytes);
        }
      }
    }

    if (successShownAt != 0 &&
        millis() - successShownAt >= kSuccessOverlayMs) {
      successShownAt = 0;
      if (deferLayoutApply) {
        // UIBuilder::buildFromLayout(DeviceState::layoutDoc());
        EncoderManager::applyFromLayout(DeviceState::layoutDoc());
        deferLayoutApply = false;
      }
      if (deferWidgetApply) {
        // UIBuilder::updateLiveData(DeviceState::widgetDataDoc());
        deferWidgetApply = false;
      }
      if (deferredScene.imageBytes) {
        applyScene(deferredScene);
        releaseFrameBuffer(deferredScene.imageBytes);
        deferredScene = UiEvent{};
      }
      if (deferredImageBytes && deferredImageLen > 0) {
        const bool shown = deferredImageFormat == RenderedScreenFormat::Rgb565
                               ? UIBuilder::showRenderedScreenRgb565(
                                     deferredImageBytes, deferredImageLen)
                               : UIBuilder::showRenderedScreen(
                                     deferredImageBytes, deferredImageLen);
        if (!shown) {
          Serial.println("Deferred rendered screen apply failed");
          releaseFrameBuffer(deferredImageBytes);
        }
        deferredImageBytes = nullptr;
        deferredImageLen = 0;
        deferredImageFormat = RenderedScreenFormat::Jpeg;
      }
    }

    // LVGL UI only on this core — react to network phase changes
    static DevicePhase shown = DevicePhase::Boot;
    static char shownCode[16] = {0};
    DevicePhase p = DeviceState::phase();
    if (p != shown ||
        (p == DevicePhase::Unpaired &&
         strcmp(shownCode, DeviceState::pairingCode()) != 0)) {
      shown = p;
      strncpy(shownCode, DeviceState::pairingCode(), sizeof(shownCode) - 1);
      if (p == DevicePhase::Provisioning)
        UIBuilder::showProvisioning(Provisioning::apName());
      else if (p == DevicePhase::Unpaired)
        UIBuilder::showPairing(DeviceState::pairingCode());
    }

    UIBuilder::tickLocalWidgets();
    DisplayManager::loop();
    UIBuilder::getSDUIEngine().presentChrome();
    DisplayManager::loop();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void setup() {
#if !ARDUINO_USB_CDC_ON_BOOT
  Serial.setRxBufferSize(8192);
#endif
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== Macropad firmware " __DATE__ " " __TIME__ " ===");
  // #region debug-point A:setup-sequence
  Serial.println("[DEBUG] setup: entered");
  // #endregion

  // Optional SNTP (uses DHCP DNS once Wi‑Fi is up)
  setenv("TZ",ClockRuntime::kDefaultTimezone,1);tzset();

  // #region debug-point B:setup-sequence
  Serial.println("[DEBUG] setup: DeviceState::init");
  // #endregion
  DeviceState::init();
  // #region debug-point C:setup-sequence
  Serial.println("[DEBUG] setup: ConfigStore::init");
  // #endregion
  ConfigStore::init();

  actionQueue = xQueueCreate(20, sizeof(char[64]));
  sduiActionQueue = xQueueCreate(4, sizeof(SduiActionRequest));
  if (!actionQueue || !sduiActionQueue) {
    Serial.println("actionQueue failed");
    return;
  }

  sceneResultQueue = xQueueCreate(1,sizeof(SceneResult));
  if(!sceneResultQueue) { Serial.println("sceneResultQueue failed");return; }
  serialTxQueue = xQueueCreate(8, sizeof(char*));
  if(!serialTxQueue) { Serial.println("serialTxQueue failed");return; }
  snapshotRequestQueue = xQueueCreate(4, sizeof(SnapshotRequest));
  if (!snapshotRequestQueue) { Serial.println("snapshotRequestQueue failed"); return; }
  photoRequestQueue = xQueueCreate(8, sizeof(PhotoRequest));
  if (!photoRequestQueue) { Serial.println("photoRequestQueue failed"); return; }
  pageRequestQueue = xQueueCreate(1, sizeof(PageRequest));
  if (!pageRequestQueue) { Serial.println("pageRequestQueue failed"); return; }
  uiQueue = xQueueCreate(12, sizeof(UiEvent));
  if (!uiQueue) {
    Serial.println("uiQueue failed");
    return;
  }

  // #region debug-point D:setup-sequence
  Serial.println("[DEBUG] setup: creating tasks");
  // #endregion
  const auto netCreated=xTaskCreatePinnedToCore(NetworkTask, "Net", 12288, nullptr, 1, nullptr, 1);
  if(netCreated != pdPASS) { Serial.println("FATAL: NetworkTask allocation failed");return; }
  // LVGL's SDUI scene build + render path (recursive createNode, then the deep
  // draw/refresh pipeline) is stack-hungry on the 1024x600 panel; 24 KB was
  // overflowing into the heap and corrupting LVGL objects.
  const auto guiCreated=xTaskCreatePinnedToCore(GuiAndInputTask, "Gui", 49152, nullptr, 2, nullptr,0);
  if(xTaskCreatePinnedToCore(SerialBridge::task,"Bridge",8192,nullptr,2,nullptr,1)!=pdPASS) Serial.println("FATAL: BridgeTask allocation failed");
  if(guiCreated != pdPASS) Serial.println("FATAL: GuiTask allocation failed");
  // #region debug-point E:setup-sequence
  Serial.println("[DEBUG] setup: tasks created");
  // #endregion
}

void loop() {
  // NetworkTask owns Serial input; two readers were splitting companion JSON.
  vTaskDelay(pdMS_TO_TICKS(100));
}
