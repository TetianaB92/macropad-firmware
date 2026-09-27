#include "remote_image.h"
#include "lan_relay.h"
#include "network_manager.h"
#include "board_config.h"
#include "bridge_queue.h"
#include "config_store.h"
#include "device_state.h"
#include "frame_buffer.h"
#include "hid_actions.h"
#include "encoder_manager.h"
#include "wifi_config.h"
#include "server_failover.h"
#include "provisioning.h"
#include "wifi_retry.h"
#include "clock_runtime.h"
#include "json_memory.h"
#include <sys/time.h>

#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WebSocketsClient.h>
#include "worker_transport.h"
#include <WiFi.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <cstdio>
#include <algorithm>

uint32_t DeviceNetworkManager::lastStatusMs_ = 0;
uint32_t DeviceNetworkManager::lastLayoutMs_ = 0;
uint32_t DeviceNetworkManager::lastWidgetMs_ = 0;
bool DeviceNetworkManager::forceSync_ = true;

static uint32_t sceneGeneration = 0;
static uint64_t sceneVersion = 0;
static char* pendingScene = nullptr;
static uint32_t pendingSceneAt = 0;
static WebSocketsClient updatesSocket;
static bool updatesConnected=false;
static uint32_t lastUpdatesAttempt=0;
static PageRequest selectedPage {};
static bool pageNeedsFetch = false;
static bool pageStatePending = false;
static uint32_t lastChromeMs = 0;

static WifiRetry wifiRetry;
static bool wifiWasConnected=false;
static int lastHttpStatus=0;
static uint32_t lastTimeAttempt=0;

static void connectionStatus(const char* message) {
  static char previous[96] = {};
  if(!strcmp(previous,message) || !uiQueue) return;
  UiEvent event {UiEventType::ConnectionStatus, reinterpret_cast<uint8_t*>(strdup(message)), strlen(message), RenderedScreenFormat::Jpeg};
  if (!event.imageBytes) return;
  if(xQueueSend(uiQueue,&event,0)!=pdTRUE) { releaseFrameBuffer(event.imageBytes);return; }
  snprintf(previous,sizeof(previous),"%s",message);
}
static void serverFailure(const char* stage) {
  char message[96];snprintf(message,sizeof(message),"Wi-Fi OK. %s failed (%d). Retrying...",stage,lastHttpStatus);
  connectionStatus(message);
}

static String queryEncode(const char* input) {
  String out;
  const char* hex = "0123456789ABCDEF";
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(input); *p; ++p) {
    const unsigned char c = *p;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') out += char(c);
    else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
  }
  return out;
}

static const char* wifiStatusName(wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS:
      return "WL_IDLE_STATUS";
    case WL_NO_SSID_AVAIL:
      return "WL_NO_SSID_AVAIL";
    case WL_SCAN_COMPLETED:
      return "WL_SCAN_COMPLETED";
    case WL_CONNECTED:
      return "WL_CONNECTED";
    case WL_CONNECT_FAILED:
      return "WL_CONNECT_FAILED";
    case WL_CONNECTION_LOST:
      return "WL_CONNECTION_LOST";
    case WL_DISCONNECTED:
      return "WL_DISCONNECTED";
    default:
      return "WL_UNKNOWN";
  }
}

static void postUiEvent(UiEventType type) {
  if (!uiQueue) return;
  UiEvent event {type, nullptr, 0, RenderedScreenFormat::Jpeg};
  if (type == UiEventType::ApplySduiPatch) {
    const char* patch = DeviceState::sduiPatchJson();
    event.imageLen = strlen(patch);
    event.imageBytes = reinterpret_cast<uint8_t*>(strdup(patch));
    if (!event.imageBytes) return;
  }
  if (xQueueSend(uiQueue, &event, 0) != pdTRUE && event.imageBytes)
    releaseFrameBuffer(event.imageBytes);
}

static bool postRenderedScreen(uint8_t* imageBytes, size_t imageLen,
                               RenderedScreenFormat imageFormat) {
  if (!uiQueue || !imageBytes || imageLen == 0) return false;
  UiEvent event {UiEventType::ApplyRenderedScreen, imageBytes, imageLen,
                 imageFormat};
  if (xQueueSend(uiQueue, &event, 0) != pdTRUE) {
    releaseFrameBuffer(imageBytes);
    return false;
  }
  return true;
}

static bool postSduiChrome(uint8_t* imageBytes, size_t imageLen, const PageRequest& request) {
  if (!uiQueue || !imageBytes || imageLen == 0) return false;
  UiEvent event {UiEventType::ApplySduiChrome, imageBytes, imageLen,
                 RenderedScreenFormat::Rgb565};
  event.generation = request.generation;
  snprintf(event.pageId, sizeof(event.pageId), "%s", request.pageId);
  if (xQueueSend(uiQueue, &event, 0) != pdTRUE) {
    releaseFrameBuffer(imageBytes);
    return false;
  }
  return true;
}

static bool extractJpegPayload(uint8_t* buffer, size_t* lenInOut) {
  if (!buffer || !lenInOut || *lenInOut < 4) return false;

  size_t start = SIZE_MAX;
  for (size_t i = 0; i + 1 < *lenInOut; ++i) {
    if (buffer[i] == 0xFF && buffer[i + 1] == 0xD8) {
      start = i;
      break;
    }
  }
  if (start == SIZE_MAX) return false;

  size_t end = SIZE_MAX;
  for (size_t i = *lenInOut; i >= start + 2; --i) {
    if (buffer[i - 2] == 0xFF && buffer[i - 1] == 0xD9) {
      end = i;
      break;
    }
  }
  if (end == SIZE_MAX || end <= start) return false;

  const size_t jpegLen = end - start;
  if (start > 0) {
    memmove(buffer, buffer + start, jpegLen);
  }
  *lenInOut = jpegLen;
  return true;
}

static bool isJpegContentType(const String& contentType) {
  return contentType.startsWith("image/jpeg") ||
         contentType.startsWith("image/jpg");
}

static bool isRgb565ContentType(const String& contentType) {
  return contentType.startsWith("image/x-rgb565");
}

void DeviceNetworkManager::init() {
  WifiConfig::init();

  if (!WifiConfig::hasWifi()) {
    Provisioning::begin();
    return;
  }

  connectionStatus("Starting Wi-Fi / C6...");
  Serial.printf("Wi-Fi startup: internal free=%u largest=%u PSRAM free=%u\n",
    unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)), unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
    unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
  if (!WiFi.mode(WIFI_STA)) {
    connectionStatus("Wi-Fi driver failed. Check serial log.");
    return;
  }
  WiFi.setAutoReconnect(true);
  wifiRetry.started(millis());
  DeviceState::setPhase(DevicePhase::WifiConnecting);
  // #region debug-point A:init-before-begin
  Serial.printf("[DEBUG] init: api=%s status=%s(%d)\n",
                WifiConfig::apiBaseUrl(), wifiStatusName(WiFi.status()),
                (int)WiFi.status());
  // #endregion
  Serial.printf("NetworkManager: connecting to %s …\n", WifiConfig::ssid());
  // #region debug-point A:init-begin-call
  Serial.println("[DEBUG] init: calling WiFi.begin");
  // #endregion
  connectionStatus("Connecting Wi-Fi...");
  WiFi.begin(WifiConfig::ssid(), WifiConfig::password());
  // #region debug-point A:init-after-begin
  Serial.printf("[DEBUG] init: WiFi.begin returned status=%s(%d)\n",
                wifiStatusName(WiFi.status()), (int)WiFi.status());
  // #endregion
}

void DeviceNetworkManager::requestSync() { forceSync_ = true; }

bool DeviceNetworkManager::ensureWifi() {
  if (Provisioning::isActive()) {
    Provisioning::loop();
    return false;
  }
  // #region debug-point B:status-transition
  static wl_status_t lastLoggedStatus = WL_IDLE_STATUS;
  wl_status_t status = WiFi.status();
  if (status != lastLoggedStatus) {
    Serial.printf("[DEBUG] ensureWifi: status=%s(%d)\n",
                  wifiStatusName(status), (int)status);
    lastLoggedStatus = status;
  }
  // #endregion
  const uint32_t now=millis();
  if (status == WL_CONNECTED) {
    wifiRetry.connected(now);
    if(!wifiWasConnected) {
      configTzTime(getenv("TZ")?getenv("TZ"):ClockRuntime::kDefaultTimezone,"pool.ntp.org","time.nist.gov");
      lastTimeAttempt=millis()-3000;
      Serial.printf("Wi-Fi connected ip=%s RSSI=%d internal=%u\n",WiFi.localIP().toString().c_str(),WiFi.RSSI(),
        unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
      connectionStatus("Wi-Fi connected. Contacting server...");
      forceSync_=true;
    }
    wifiWasConnected=true;
    return true;
  }
  wifiWasConnected=false;
  if (!WifiConfig::hasWifi()) { Provisioning::begin();return false; }
  DeviceState::setPhase(DevicePhase::WifiConnecting);
  char message[96];snprintf(message,sizeof(message),"Connecting Wi-Fi... %s",wifiStatusName(status));
  connectionStatus(message);
  if(wifiRetry.due(now)) {
    if(wifiRetry.retry(now)) { Provisioning::begin();return false; }
    Serial.printf("Wi-Fi reconnect status=%s internal=%u largest=%u\n",wifiStatusName(status),
      unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)), unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
    WiFi.reconnect();
  }
  return false;
}

// App requests need the same verified TLS path as remote assets on ESP32-P4.
static bool beginAppHttp(HTTPClient& http, WiFiClientSecure& secure, const String& url) {
#if defined(CONFIG_IDF_TARGET_ESP32P4)
  if (url.startsWith("https://")) {
    secure.setCACertBundle(worker_ca_start, worker_ca_end-worker_ca_start);
    return http.begin(secure, url);
  }
#else
  (void)secure;
#endif
  return http.begin(url);
}

static constexpr const char* hostedApi = "https://macropad-tawny.vercel.app";
static ServerFailover serverRoute;
static const char* activeApi() {
  return serverRoute.useLocal() ? WifiConfig::apiBaseUrl() : hostedApi;
}
static void announceServerChange(const char* previous) {
  if (!strcmp(previous, activeApi())) return;
  updatesSocket.disconnect(); updatesConnected = false;
  lastUpdatesAttempt = millis() - 60001U;
  DeviceNetworkManager::requestSync();
  Serial.printf("API selected: %s\n", activeApi());
}
static void checkPreferredServer() {
  if (!strcmp(WifiConfig::apiBaseUrl(), hostedApi) || !serverRoute.probeDue(millis())) return;
  const char* previous = activeApi();
  WiFiClientSecure secure;
  HTTPClient http;
  bool healthy = false;
  const String url = String(WifiConfig::apiBaseUrl()) + "/api/devices/time";
  if (beginAppHttp(http, secure, url)) {
    http.setConnectTimeout(1000); http.setTimeout(1000);
    if (http.GET() == 200 && http.getSize() <= 256) {
      JsonDocument response;
      const String body = http.getString();
      healthy = body.length() <= 256 && !deserializeJson(response, body) &&
        ClockRuntime::valid(response["epochMs"].as<uint64_t>());
    }
    http.end();
  }
  serverRoute.probeResult(millis(), healthy);
  Serial.printf("API probe: preferred=%s reachable=%d active=%s\n", WifiConfig::apiBaseUrl(), healthy, activeApi());
  announceServerChange(previous);
}
static void recordApiResponse(int status) {
  const char* previous = activeApi();
  serverRoute.response(millis(), status);
  announceServerChange(previous);
}

bool DeviceNetworkManager::httpGet(const char* path, String& bodyOut) {
  WiFiClientSecure secure;
  HTTPClient http;
  checkPreferredServer();
  String url = String(activeApi()) + path;
  // #region debug-point C:http-get-start
  Serial.printf("[DEBUG] httpGet: %s\n", url.c_str());
  // #endregion
  if (!beginAppHttp(http, secure, url)) { lastHttpStatus=-1; recordApiResponse(-1); return false; }
  http.addHeader("Authorization",
                 String("Bearer ") + DeviceState::deviceToken());
  http.setTimeout(12000);
  int code = http.GET();
  lastHttpStatus=code;
  recordApiResponse(code);
  bodyOut = http.getString();
  // #region debug-point C:http-get-result
  Serial.printf("[DEBUG] httpGet: code=%d bytes=%u path=%s\n", code,
                (unsigned)bodyOut.length(), path);
  // #endregion
  http.end();
  if (code < 200 || code >= 300) {
    Serial.printf("GET %s → %d\n", path, code);
    return false;
  }
  return true;
}

bool DeviceNetworkManager::httpGetBinary(const char* path, uint8_t** bodyOut,
                                         size_t* lenOut,
                                         RenderedScreenFormat* formatOut, int* statusOut, size_t expectedBytes) {
  if (statusOut) *statusOut = -1;
  if (!bodyOut || !lenOut) return false;
  *bodyOut = nullptr;
  *lenOut = 0;
  if (formatOut) *formatOut = RenderedScreenFormat::Jpeg;

  HTTPClient http;
  WiFiClientSecure secure;
  const bool external=strncmp(path,"https://",8)==0;
  if (!external) checkPreferredServer();
  String url = external ? String(path) : String(activeApi()) + path;
  const char* responseHeaderKeys[] = {"Content-Type"};
  if(external) {
#if defined(CONFIG_IDF_TARGET_ESP32P4)
    secure.setCACertBundle(worker_ca_start,worker_ca_end-worker_ca_start);
    if(!http.begin(secure,url)) return false;
#else
    return false;
#endif
  } else {
    if (!beginAppHttp(http, secure, url)) { lastHttpStatus=-1; recordApiResponse(-1); return false; }
    http.addHeader("Authorization",String("Bearer ")+DeviceState::deviceToken());
  }
  http.collectHeaders(responseHeaderKeys,1);
  http.setTimeout(20000);
  int code = http.GET();
  // #region debug-point D:http-binary-code
  if (statusOut) *statusOut = code;
  if (!external) recordApiResponse(code);
  Serial.printf("Asset response code=%d\n",code);
  // #endregion
  if (code < 200 || code >= 300) {
    Serial.printf("GET %s → %d\n", external ? "Cloudflare asset" : path, code);
    http.end();
    return false;
  }

  const String contentType = http.header("Content-Type");
  // #region debug-point D:http-binary-content-type
  Serial.printf("[DEBUG] httpGetBinary: content-type=%s\n",
                contentType.length() ? contentType.c_str() : "(empty)");
  // #endregion
  const bool pathWantsRgb565 = expectedBytes || strstr(path, "format=rgb565") != nullptr;
  const bool isRgb565 =
      pathWantsRgb565 || isRgb565ContentType(contentType);
  const bool isJpeg =
      !isRgb565 &&
      (contentType.isEmpty() || isJpegContentType(contentType));
  if (!isRgb565 && !isJpeg) {
    Serial.printf("GET %s → unexpected content-type %s\n", path,
                  contentType.c_str());
    http.end();
    return false;
  }

  const size_t rgbBytes = expectedBytes ? expectedBytes : MEDIA_RGB565_FRAME_BYTES;
  int len = http.getSize();
  if (isRgb565) {
    len = (int)rgbBytes;
  } else if (len <= 0 || len > MEDIA_JPEG_MAX_BYTES) {
    len = MEDIA_JPEG_MAX_BYTES;
  }

  uint8_t* buffer = nullptr;
  if (isRgb565) {
    buffer = (uint8_t*)heap_caps_aligned_alloc(
        128, (size_t)len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  if (!buffer) {
    buffer = (uint8_t*)heap_caps_malloc(len + 8,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  if (!buffer) buffer = (uint8_t*)malloc(len + 8);
  if (!buffer) {
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  size_t got = 0;
  uint32_t idleAt = millis();
  while ((http.connected() || stream->available()) && got < (size_t)len) {
    size_t avail = stream->available();
    if (!avail) {
      if (millis() - idleAt > 15000) break;
      delay(1);
      continue;
    }
    idleAt = millis();
    int read = stream->readBytes(buffer + got, min(avail, len - got));
    if (read <= 0) break;
    got += (size_t)read;
  }
  http.end();

  if (isRgb565) {
    if (got != rgbBytes) {
      Serial.printf("httpGetBinary: RGB565 bytes mismatch got=%u expected=%u\n",
                    (unsigned)got, (unsigned)rgbBytes);
      releaseFrameBuffer(buffer);
      return false;
    }
    if (!expectedBytes && buffer[0] == 0xFF && buffer[1] == 0xD8) {
      Serial.println("httpGetBinary: RGB565 payload looks like JPEG, aborting");
      releaseFrameBuffer(buffer);
      return false;
    }
    psramWriteback(buffer, got);
    if(got>=4) Serial.printf("httpGetBinary: RGB565 magic=%02X %02X %02X %02X\n",
                  buffer[0], buffer[1], buffer[2], buffer[3]);
    *bodyOut = buffer;
    *lenOut = got;
    if (formatOut) *formatOut = RenderedScreenFormat::Rgb565;
    return true;
  }

  if (got < 100) {
    releaseFrameBuffer(buffer);
    return false;
  }

  if (!extractJpegPayload(buffer, &got)) {
    Serial.println("httpGetBinary: JPEG markers not found");
    releaseFrameBuffer(buffer);
    return false;
  }

  *bodyOut = buffer;
  *lenOut = got;
  if (formatOut) *formatOut = RenderedScreenFormat::Jpeg;
  return true;
}

bool DeviceNetworkManager::httpPostJson(const char* path, const String& json,
                                        String& bodyOut) {
  WiFiClientSecure secure;
  HTTPClient http;
  checkPreferredServer();
  String url = String(activeApi()) + path;
  if (!beginAppHttp(http, secure, url)) { lastHttpStatus=-1; recordApiResponse(-1); return false; }
  http.addHeader("Content-Type", "application/json");
  if(DeviceState::deviceToken()[0]) http.addHeader("Authorization",String("Bearer ")+DeviceState::deviceToken());
  http.setTimeout(12000);
  int code = http.POST(json);
  lastHttpStatus=code;
  recordApiResponse(code);
  bodyOut = http.getString();
  http.end();
  if (code < 200 || code >= 300) {
    Serial.printf("POST %s → %d %s\n", path, code, bodyOut.c_str());
    return false;
  }
  return true;
}

void DeviceNetworkManager::registerDevice() {
  JsonDocument req;
  req["mac_address"] = DeviceState::mac();
  String payload;
  serializeJson(req, payload);
  // #region debug-point E:register-start
  Serial.printf("[DEBUG] registerDevice: mac=%s payload=%s\n",
                DeviceState::mac(), payload.c_str());
  // #endregion
  String body;
  if (!httpPostJson("/api/devices/register", payload, body)) {
    DeviceState::setPhase(DevicePhase::Error);
    serverFailure("Registration");
    return;
  }
  // #region debug-point E:register-body
  Serial.printf("[DEBUG] registerDevice: response bytes=%u\n",unsigned(body.length()));
  // #endregion
  JsonDocument res;
  if (deserializeJson(res, body, DeserializationOption::NestingLimit(SCENE_NESTING_LIMIT))) return;

  const char* token = res["device_token"] | "";
  if (token[0]) DeviceState::setDeviceToken(token);

  const char* code = res["code"] | "";
  DeviceState::setPairingCode(code);
  DeviceState::setPaired(res["paired"] | false);

  if (DeviceState::paired()) {
    DeviceState::setPhase(DevicePhase::Synced);
  } else {
    DeviceState::setPhase(DevicePhase::Unpaired);
  }
  Serial.printf("Registered code=%s paired=%d\n", code,
                (int)DeviceState::paired());
}

void DeviceNetworkManager::pollStatus() {
  if (pendingScene) return;
  if (!DeviceState::deviceToken()[0]) {
    registerDevice();
    return;
  }
  // #region debug-point C:poll-status-start
  Serial.printf("[DEBUG] pollStatus: cachedPaired=%d hasLayout=%d mac=%s\n",
                (int)DeviceState::paired(), (int)DeviceState::hasLayout(),
                DeviceState::mac());
  // #endregion
  String body;
  if (!httpGet("/api/devices/status", body)) {
    serverFailure("Server connection");
    if (lastHttpStatus == 401) {
      DeviceState::setDeviceToken("");
      registerDevice();
    }
    return;
  }
  // #region debug-point C:poll-status-body
  Serial.printf("[DEBUG] pollStatus: body=%s\n", body.c_str());
  // #endregion
  JsonDocument res;
  if (deserializeJson(res, body, DeserializationOption::NestingLimit(SCENE_NESTING_LIMIT))) {
    // #region debug-point C:poll-status-parse-failed
    Serial.println("[DEBUG] pollStatus: JSON parse failed");
    // #endregion
    return;
  }

  bool paired = res["paired"] | false;
  bool wasPaired = DeviceState::paired();
  const char* responseMac = res["mac_address"] | "";
  if (responseMac[0] && strcmp(responseMac, DeviceState::mac()) != 0) {
    // #region debug-point E:token-mismatch
    Serial.printf("[DEBUG] pollStatus: token belongs to mac=%s but device is mac=%s; re-registering\n",
                  responseMac, DeviceState::mac());
    // #endregion
    DeviceState::setDeviceToken("");
    DeviceState::setPairingCode("");
    DeviceState::setPaired(false);
    registerDevice();
    return;
  }
  DeviceState::setPaired(paired);
  // #region debug-point C:poll-status-parsed
  Serial.printf("[DEBUG] pollStatus: paired=%d wasPaired=%d mac=%s updated=%s code=%s\n",
                (int)paired, (int)wasPaired,
                (res["mac_address"] | "(none)"),
                (res["layout_updated_at"] | "(null)"),
                (res["pairing_code"] | "(null)"));
  // #endregion

  if (!paired) {
    const char* code = res["pairing_code"] | "";
    if (!code[0]) {
      // No active pairing code — re-register to get a new one
      registerDevice();
      return;
    }
    DeviceState::setPairingCode(code);
    DeviceState::setPhase(DevicePhase::Unpaired);
    return;
  }

  const char* updated = res["layout_updated_at"] | "";
  // Flash restores the UI's timestamp before the network task knows its scene
  // version. Also honor a 409/push-triggered refresh even with that same date.
  bool needLayout =
      forceSync_ || sceneVersion == 0 || !DeviceState::hasLayout() ||
      (updated[0] && strcmp(updated, DeviceState::layoutUpdatedAt()) != 0);

  if (!wasPaired && paired) {
    postUiEvent(UiEventType::ShowPairingSuccess);
  }
  if (needLayout) {
    // Hybrid SDUI on every target: fetch the primitive scene, then the static
    // chrome bake that sits behind the live LVGL overlays.
    pollScene();
    // The GUI requests the matching active page after installing the scene.
    // pollScene records the version only after successfully queueing that scene.
  }
  if (!forceSync_) { DeviceState::setPhase(DevicePhase::Synced);connectionStatus(""); }
}

int DeviceNetworkManager::tzOffsetMin() {
  return ClockRuntime::offsetMinutes(time(nullptr));
}

void DeviceNetworkManager::pollScene() {
  if (pendingScene) return; // Wait for the GUI acknowledgement before replacing an in-flight scene.
  char path[96];
  snprintf(path, sizeof(path), "/api/devices/scene?tz_offset_min=%d",
           tzOffsetMin());
  String body;
  connectionStatus("Wi-Fi OK. Loading layout...");
  if (!httpGet(path, body)) {
    serverFailure("Layout");
    Serial.println("SDUI scene fetch failed — keeping current scene");
    return;
  }
  SceneDocument meta;
  const auto parseError = deserializeJson(meta, body, DeserializationOption::NestingLimit(SCENE_NESTING_LIMIT));
  if (parseError) { Serial.printf("Scene JSON rejected: %s\n", parseError.c_str());return; }
  if (meta["protocol"] != "sdui" ||
      !meta["payload"]["scene"]["pages"].is<JsonArray>()) return;
  const uint64_t version = meta["payload"]["scene"]["version"].as<uint64_t>();
  if (version && version == sceneVersion && DeviceState::hasLayout()) {
    forceSync_ = false;
    Serial.printf("SDUI scene unchanged version=%llu\n", (unsigned long long)version);
    return;
  }
  UiEvent sceneEvent {UiEventType::ApplySduiScene,
      reinterpret_cast<uint8_t*>(copySceneBytes(body.c_str())), body.length(), RenderedScreenFormat::Jpeg};
  if (!sceneEvent.imageBytes) return;
  sceneEvent.generation = sceneGeneration + 1;
  if (xQueueSend(uiQueue, &sceneEvent, 0) != pdTRUE) {
    releaseFrameBuffer(sceneEvent.imageBytes);
    return;
  }
  sceneGeneration = sceneEvent.generation;
  sceneVersion = meta["payload"]["scene"]["version"].as<uint64_t>();
  selectedPage = PageRequest{};
  pageNeedsFetch = false;
  pendingScene=copySceneBytes(body.c_str());pendingSceneAt=millis();
  // Persist and acknowledge this version only after the GUI installs it.
  if(!pendingScene) forceSync_=true;
  Serial.printf("SDUI scene queued (%u bytes)\n", (unsigned)body.length());
}

void DeviceNetworkManager::pollSduiState() {
  if (!DeviceState::paired()) return;
  if (!selectedPage.pageId[0]) return;
  String path = "/api/devices/sdui-state?tz_offset_min=" + String(tzOffsetMin()) +
                "&screenId=" + queryEncode(selectedPage.pageId) + (pageStatePending ? "&wled=1" : "");
  String body;
  if (!httpGet(path.c_str(), body)) return;
  SceneDocument metadata;
  if (deserializeJson(metadata, body, DeserializationOption::NestingLimit(SCENE_NESTING_LIMIT))) return;
  const char* updated = metadata["updated_at"] | "";
  if (updated[0] && strcmp(updated, DeviceState::layoutUpdatedAt()) != 0) {
    requestSync();return; // Never apply new-layout state to old nodes.
  }
  DeviceState::setSduiPatchJson(body);
  postUiEvent(UiEventType::ApplySduiPatch);
}

void DeviceNetworkManager::pollLayout() {
  String body;
  if (!httpGet("/api/devices/layout", body)) return;

  JsonDocument res;
  if (deserializeJson(res, body, DeserializationOption::NestingLimit(SCENE_NESTING_LIMIT))) {
    Serial.println("layout JSON parse failed");
    return;
  }

  JsonObjectConst layout = res["layout"].as<JsonObjectConst>();
  if (layout.isNull()) return;

  String tmp;
  serializeJson(layout, tmp);
  DeviceState::layoutDoc().clear();
  if (deserializeJson(DeviceState::layoutDoc(), tmp)) {
    Serial.println("layout copy failed");
    return;
  }
  DeviceState::setHasLayout(true);

  const char* updated = res["updated_at"] | "";
  DeviceState::setLayoutUpdatedAt(updated);
  ConfigStore::saveToFlash(DeviceState::layoutDoc());
  String pretty;
  serializeJsonPretty(DeviceState::layoutDoc(), pretty);
  Serial.println("Layout JSON:");
  Serial.println(pretty);
  postUiEvent(UiEventType::ApplyLayout);
  forceSync_ = false;
  Serial.println("Layout applied");
}

void DeviceNetworkManager::pollWidgetData() {
  if (!DeviceState::paired()) return;
  pollSduiState();
}

bool DeviceNetworkManager::pollRenderedScreen() {
  char path[128];
  snprintf(path, sizeof(path),
           "/api/devices/render-screen?width=%d&height=%d&format=rgb565",
           PANEL_WIDTH, PANEL_HEIGHT);

  uint8_t* imageBytes = nullptr;
  size_t imageLen = 0;
  RenderedScreenFormat imageFormat = RenderedScreenFormat::Jpeg;
  if (!httpGetBinary(path, &imageBytes, &imageLen, &imageFormat)) return false;

  if (!postRenderedScreen(imageBytes, imageLen, imageFormat)) {
    Serial.println("Rendered screen queue full");
    return false;
  }

  DeviceState::setPhase(DevicePhase::Active);
  forceSync_ = false;
  Serial.printf("Rendered screen queued (%u bytes, format=%s)\n",
                (unsigned)imageLen,
                imageFormat == RenderedScreenFormat::Rgb565 ? "rgb565"
                                                            : "jpeg");
  return true;
}

// Fetch only the static "chrome" bake (everything except live overlay widgets)
// as RGB565 and hand it to the SDUI engine as a full-screen background image.
bool DeviceNetworkManager::pollChrome() {
  if (!DeviceState::paired() || !selectedPage.pageId[0]) return false;
  const PageRequest request = selectedPage; // response identity survives later swipes
  char version[24];
  snprintf(version, sizeof(version), "%llu", (unsigned long long)sceneVersion);
  String path = "/api/devices/render-screen?chrome=1&width=" + String(PANEL_WIDTH) +
      "&height=" + String(PANEL_HEIGHT) + "&format=rgb565&screenId=" +
      queryEncode(request.pageId) + "&sceneVersion=" + version +
      "&tz_offset_min=" + String(tzOffsetMin());

  uint8_t* imageBytes = nullptr;
  size_t imageLen = 0;
  RenderedScreenFormat imageFormat = RenderedScreenFormat::Rgb565;
  int status = 0;
  auto failed = [&](int error) {
    UiEvent failure {UiEventType::PageLoadFailed, nullptr, 0, RenderedScreenFormat::Rgb565};
    failure.generation = request.generation;
    failure.status = error;
    snprintf(failure.pageId, sizeof(failure.pageId), "%s", request.pageId);
    xQueueSend(uiQueue, &failure, 0);
    return false;
  };
  bool loaded=false;
  bool triedLocal=false;
  String manifestBody;
  if(httpGet((path+"&manifest=1").c_str(),manifestBody)) {
    SceneDocument manifest;
    if(!deserializeJson(manifest, manifestBody, DeserializationOption::NestingLimit(SCENE_NESTING_LIMIT))) {
      if(manifest["local"]==true) {
        triedLocal=true;
        loaded=httpGetBinary(path.c_str(),&imageBytes,&imageLen,&imageFormat,&status);
      }
      else if(manifest["format"]=="rgb565" && manifest["bytes"].as<size_t>()==MEDIA_RGB565_FRAME_BYTES) {
        const char* assetUrl=manifest["url"] | "";
        if(!strncmp(assetUrl,"https://",8)) loaded=httpGetBinary(assetUrl,&imageBytes,&imageLen,&imageFormat,&status,MEDIA_RGB565_FRAME_BYTES);
      }
    }
  } else status=lastHttpStatus;
  // A failed Worker TLS transfer must not leave every baked widget blank.
  // The authenticated render endpoint also delivers RGB565 directly, without
  // depending on R2 availability or a second TLS connection on the device.
  // A stale/removed scene needs a new scene instead of another render request.
  if(!loaded && !triedLocal && status!=409 && status!=404 && status!=401 && status!=403) {
    Serial.printf("Chrome CDN transfer failed (%d); trying render server\n",status);
    loaded=httpGetBinary(path.c_str(),&imageBytes,&imageLen,&imageFormat,&status);
  }
  if (!loaded) {
    if (status == 409 || status == 404) forceSync_ = true;
    return failed(status == 200 ? -2 : status);
  }
  if (imageFormat != RenderedScreenFormat::Rgb565) {
    releaseFrameBuffer(imageBytes);
    return failed(-3);
  }
  // Copy before handing the original to the GUI thread so flash I/O cannot
  // race with setChromeImage freeing or replacing the bake.
  uint8_t* persistCopy = allocFrameBuffer(imageLen);
  if (persistCopy) memcpy(persistCopy, imageBytes, imageLen);
  if (!postSduiChrome(imageBytes, imageLen, request)) {
    releaseFrameBuffer(persistCopy);
    Serial.println("Chrome bake queue full");
    return failed(-4);
  }
  if (persistCopy) {
    ConfigStore::saveChrome(request.pageId, PANEL_WIDTH, PANEL_HEIGHT,
                            persistCopy, imageLen);
    releaseFrameBuffer(persistCopy);
  }
  Serial.printf("Chrome bake queued (%u bytes)\n", (unsigned)imageLen);
  return true;
}

void RemoteImage::fetch(const SnapshotRequest& request) {
  if (!isCurrent(request.epoch)) return;
  uint8_t* pixels = nullptr;
  HTTPClient http;
  WiFiClientSecure secure;
  bool started = false;
  if (!strncmp(request.url, "http://", 7)) started = http.begin(request.url);
#if defined(CONFIG_IDF_TARGET_ESP32P4)
  else if (!strncmp(request.url, "https://", 8)) {
    secure.setCACertBundle(worker_ca_start, worker_ca_end - worker_ca_start);
    started = http.begin(secure, request.url);
  }
#endif
  if (started) {
    // User-selected image origin: never attach the macropad API token.
    http.setConnectTimeout(2000);
    http.setTimeout(3000);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    http.addHeader("Cache-Control", "no-cache");
    const char* headers[] = {"Content-Type"};
    http.collectHeaders(headers, 1);
    const int status = http.GET();
    if (status == 200 && http.header("Content-Type").startsWith("image/jpeg") &&
        http.getSize() <= 512 * 1024 && isCurrent(request.epoch)) {
      class BoundedImage : public Stream {
      public:
        uint8_t* data;
        size_t used = 0;
        uint32_t epoch;
        uint32_t startedAt = millis();
        explicit BoundedImage(uint32_t generation)
            : data(allocFrameBuffer(512 * 1024)), epoch(generation) {}
        ~BoundedImage() { releaseFrameBuffer(data); }
        size_t write(uint8_t byte) override { return write(&byte, 1); }
        size_t write(const uint8_t* bytes, size_t length) override {
          if (!data || length > 512 * 1024 - used || millis() - startedAt > 6000 ||
              !RemoteImage::isCurrent(epoch))
            return 0;
          memcpy(data + used, bytes, length);
          used += length;
          return length;
        }
        int available() override { return 0; }
        int read() override { return -1; }
        int peek() override { return -1; }
        void flush() override {}
      };
      BoundedImage body(request.epoch);
      if (body.data && http.writeToStream(&body) > 0 && body.used >= 4 &&
          isCurrent(request.epoch))
        pixels = decode(body.data, body.used, request.width, request.height,
                        request.cover);
    }
    http.end();
  }
  UiEvent event{UiEventType::ApplySnapshot, pixels,
                pixels ? size_t(request.width) * request.height * 2 : 0,
                RenderedScreenFormat::Rgb565};
  event.generation = request.ticket;
  if (!uiQueue || xQueueSend(uiQueue, &event, 0) != pdTRUE)
    releaseFrameBuffer(pixels);
}

void DeviceNetworkManager::loop() {
  SceneResult result {};
  if(sceneResultQueue && xQueueReceive(sceneResultQueue,&result,0)==pdTRUE && result.generation==sceneGeneration && pendingScene) {
    if(result.applied) {
      SceneDocument meta;deserializeJson(meta, pendingScene, DeserializationOption::NestingLimit(SCENE_NESTING_LIMIT));
      const char* updated=meta["updated_at"] | "";
      if(updated[0]) DeviceState::setLayoutUpdatedAt(updated);
      DeviceState::setHasLayout(true);ConfigStore::saveScene(pendingScene);
      forceSync_=false;DeviceState::setPhase(DevicePhase::Active);
    } else forceSync_=true;
    releaseFrameBuffer(reinterpret_cast<uint8_t*>(pendingScene));pendingScene=nullptr;
  }
  if(pendingScene && millis()-pendingSceneAt>45000) {
    releaseFrameBuffer(reinterpret_cast<uint8_t*>(pendingScene));pendingScene=nullptr;forceSync_=true;
  }
  if (!ensureWifi()) return;

  checkPreferredServer();
  LanRelay::configure(activeApi(), DeviceState::deviceToken(), DeviceState::paired());
  uint32_t now = millis();
  updatesSocket.loop();
  if(DeviceState::paired() && !updatesConnected && ClockRuntime::valid(uint64_t(time(nullptr))*1000) && now-lastUpdatesAttempt>60000U) {
    lastUpdatesAttempt=now;String response;
    if(httpGet("/api/devices/updates",response)) {
      SceneDocument config;
      if(!deserializeJson(config, response, DeserializationOption::NestingLimit(SCENE_NESTING_LIMIT))) {
        String address=config["url"] | "";
        if(address.startsWith("wss://")) {
          const int slash=address.indexOf('/',6);String host=address.substring(6,slash),path=address.substring(slash);
          if(slash>6 && host.indexOf(':')<0 && host.indexOf('@')<0) {
            updatesSocket.disconnect();
            updatesSocket.onEvent([](WStype_t type,uint8_t* data,size_t length) {
              if(type==WStype_CONNECTED) updatesConnected=true;
              else if(type==WStype_DISCONNECTED) updatesConnected=false;
              else if(type==WStype_TEXT && length<256) {
                SceneDocument notice;if(!deserializeJson(notice,data,length) && notice["type"]=="layout.changed" && notice["version"].as<uint64_t>()>sceneVersion) DeviceNetworkManager::requestSync();
              }
            });
#if defined(CONFIG_IDF_TARGET_ESP32P4)
            updatesSocket.beginSslWithBundle(host.c_str(),443,path.c_str(),worker_ca_start,worker_ca_end-worker_ca_start);
#endif
            updatesSocket.setReconnectInterval(5000);updatesSocket.enableHeartbeat(20000,5000,2);
          }
        }
      }
    }
  }

  if(now-lastTimeAttempt >= (ClockRuntime::valid(uint64_t(time(nullptr))*1000)?3600000U:3000U)) {
    lastTimeAttempt=now;String body;
    if(httpGet("/api/devices/time",body)) {
      JsonDocument clock;
      if(!deserializeJson(clock, body, DeserializationOption::NestingLimit(SCENE_NESTING_LIMIT))) {
        const uint64_t epoch=clock["epochMs"].as<uint64_t>()+(millis()-now)/2;
        if(ClockRuntime::valid(epoch)) {
          struct timeval tv {static_cast<time_t>(epoch/1000),static_cast<suseconds_t>((epoch%1000)*1000)};
          settimeofday(&tv,nullptr);
          Serial.println("Clock synchronized from server (UTC)");
        }
      }
    }
  }

  if (!DeviceState::deviceToken()[0]) {
    if (now - lastStatusMs_ > 2000) {
      lastStatusMs_ = now;
      registerDevice();
    }
    return;
  }

  uint32_t statusEvery =
      DeviceState::paired() ? (updatesConnected?60000U:LAYOUT_POLL_MS) : STATUS_POLL_UNPAIRED_MS;
  if ((forceSync_ && now - lastStatusMs_ > 2000) || now - lastStatusMs_ > statusEvery) {
    lastStatusMs_ = now;
    pollStatus();
  }

  PageRequest request {};
  if (pageRequestQueue && xQueueReceive(pageRequestQueue, &request, 0) == pdTRUE &&
      request.generation == sceneGeneration) {
    selectedPage = request;
    selectedPage.chromeRefreshMs=request.chromeRefreshMs ? std::max<uint32_t>(30000,std::min<uint32_t>(86400000,request.chromeRefreshMs)) : 0;
    selectedPage.stateRefreshMs=request.stateRefreshMs ? std::max<uint32_t>(5000,std::min<uint32_t>(86400000,request.stateRefreshMs)) : 0;
    pageStatePending = true;
    lastWidgetMs_=now-selectedPage.stateRefreshMs-1;
    pageNeedsFetch = !request.chromeReady;
    lastChromeMs = request.chromeReady ? now : 0;
  }
  // A missing page bake hides every overlay. Fetch it before album frames so
  // photo conversion/transfer cannot hold the whole dashboard on its loader.
  if (selectedPage.pageId[0] && DeviceState::paired() && pageNeedsFetch &&
      (lastChromeMs == 0 || now - lastChromeMs >= 5000U)) {
    lastChromeMs = now;
    pageNeedsFetch = !pollChrome();
    return;
  }
  PhotoRequest photo {};
  if(DeviceState::paired() && photoRequestQueue && xQueueReceive(photoRequestQueue,&photo,0)==pdTRUE) {
    uint8_t* bytes=nullptr;size_t length=0;RenderedScreenFormat format;int status=0;
    // Pull RGB565 from the same authenticated API host as the scene. A second
    // TLS session to the Cloudflare worker is what left albums on
    // "Photos unavailable" while the JSON manifest still returned 200.
    String path=String("/api/devices/photo?format=rgb565&binary=1&id=")+photo.assetId+
                "&width="+photo.width+"&height="+photo.height;
    const bool ok=httpGetBinary(path.c_str(),&bytes,&length,&format,&status,
                                size_t(photo.width)*photo.height*2);
    if(!ok) Serial.printf("Photo download failed: api=%d bytes=%u expected=%u\n",
                          status,unsigned(length),unsigned(photo.width)*photo.height*2);
    UiEvent event {UiEventType::ApplyPhoto,ok?bytes:nullptr,ok?length:0,RenderedScreenFormat::Rgb565};
    event.generation=photo.ticket;
    if(!uiQueue || xQueueSend(uiQueue,&event,pdMS_TO_TICKS(100))!=pdTRUE) releaseFrameBuffer(event.imageBytes);
  }
  // Refresh only the visible page. A failed request retries without blocking
  // local timers, and an obsolete result cannot replace the new page's bake.
  if (selectedPage.pageId[0] && DeviceState::paired() &&
      (lastChromeMs == 0 || (pageNeedsFetch ? now-lastChromeMs >= 5000U : selectedPage.chromeRefreshMs && now-lastChromeMs >= selectedPage.chromeRefreshMs))) {
    lastChromeMs = now;
    pageNeedsFetch = !pollChrome();
    return; // Give newer swipe requests priority over routine state polling.
  }
  if (DeviceState::paired() && (pageStatePending || (selectedPage.stateRefreshMs && now - lastWidgetMs_ > selectedPage.stateRefreshMs))) {
    lastWidgetMs_ = now;
    pollSduiState();
    pageStatePending = false;
    return;
  }
  // Lowest priority: only active-page images are scheduled by the GUI.
  SnapshotRequest snapshot{};
  if (DeviceState::paired() && snapshotRequestQueue && xQueueReceive(snapshotRequestQueue,&snapshot,0)==pdTRUE)
    RemoteImage::fetch(snapshot);
}

void DeviceNetworkManager::handleSduiAction(const SduiActionRequest& action) {
  JsonDocument ev;
  ev["protocol"]="sdui";ev["version"]=1;ev["type"]="event";
  auto payload=ev["payload"].to<JsonObject>();
  payload["widgetId"]=action.widgetId;payload["nodeId"]=action.nodeId;
  payload["action"]=action.name;payload["dataId"]=action.dataId;payload["event"]="click";
  String request,body;serializeJson(ev,request);
  const bool ok=WiFi.status()==WL_CONNECTED && httpPostJson("/api/devices/event",request,body);
  JsonDocument response;
  if(!ok || deserializeJson(response, body, DeserializationOption::NestingLimit(SCENE_NESTING_LIMIT)) || response["type"]!="patch") {
    response.clear();response["protocol"]="sdui";response["version"]=1;response["type"]="patch";
    auto operation=response["payload"]["operations"].add<JsonObject>();
    operation["op"]="replace";operation["target"]="state";
    operation["path"]=String(action.widgetId)+".status";
    operation["value"]=WiFi.status()==WL_CONNECTED ? "Not confirmed. Refresh and retry." : "Offline. Task not saved. Tap to retry.";
    body="";serializeJson(response,body);
  }
  UiEvent event {UiEventType::ApplySduiPatch,reinterpret_cast<uint8_t*>(strdup(body.c_str())),body.length(),RenderedScreenFormat::Jpeg};
  if(event.imageBytes && (!uiQueue || xQueueSend(uiQueue,&event,pdMS_TO_TICKS(100))!=pdTRUE)) free(event.imageBytes);
  lastWidgetMs_=0; // Reconcile even if completion succeeded but its response was lost.
}

void DeviceNetworkManager::handleAction(const char* action) {
  if (!action) return;
  if(!strcmp(action,"factory_reset")) { WifiConfig::clear();DeviceState::setDeviceToken("");DeviceState::setPaired(false);ESP.restart();return; }
  Serial.printf("Action: %s\n", action);
  if (strncmp(action, "SDUI_EVENT:", 11) == 0) {
    JsonDocument ev;
    ev["protocol"] = "sdui";
    ev["version"] = 1;
    ev["type"] = "event";
    ev["payload"]["widgetId"] = action + 11;
    ev["payload"]["event"] = "click";
    ev["payload"]["timestamp"] = (uint32_t)millis();
    String payload;
    serializeJson(ev, payload);
    String body;
    httpPostJson("/api/devices/event", payload, body);
    return;
  }
  if (strncmp(action, "ENC_ROTATE:", 11) == 0 || strncmp(action, "ENC_PRESS:", 10) == 0) {
    int idx = 0;
    char kind[32] = {0};
    int dir = 1;
    JsonDocument ev;
    ev["protocol"] = "sdui";
    ev["version"] = 1;
    ev["type"] = "event";
    auto payload = ev["payload"].to<JsonObject>();
    payload["action"] = "encoder.set";
    bool cloud = false;
    if (strncmp(action, "ENC_ROTATE:", 11) == 0) {
      if (sscanf(action, "ENC_ROTATE:%d:%31[^:]:%d", &idx, kind, &dir) >= 2 &&
          (!strcmp(kind, "wled_brightness") || !strcmp(kind, "ha_brightness"))) {
        payload["event"] = "rotate";
        payload["index"] = idx;
        payload["dir"] = dir;
        payload["kind"] = kind;
        cloud = true;
      }
    } else if (sscanf(action, "ENC_PRESS:%d:%31s", &idx, kind) >= 2 &&
               (!strcmp(kind, "wled_power") || !strcmp(kind, "ha_toggle"))) {
      payload["event"] = "press";
      payload["index"] = idx;
      payload["kind"] = kind;
      cloud = true;
    }
    if (cloud) {
      String request, body;
      serializeJson(ev, request);
      const bool ok = WiFi.status() == WL_CONNECTED && httpPostJson("/api/devices/event", request, body);
      JsonDocument response;
      if (ok && !deserializeJson(response, body, DeserializationOption::NestingLimit(SCENE_NESTING_LIMIT)) &&
          response["type"] == "patch") {
        UiEvent event{UiEventType::ApplySduiPatch,
                      reinterpret_cast<uint8_t*>(strdup(body.c_str())), body.length(),
                      RenderedScreenFormat::Jpeg};
        if (event.imageBytes &&
            (!uiQueue || xQueueSend(uiQueue, &event, pdMS_TO_TICKS(100)) != pdTRUE))
          free(event.imageBytes);
        lastWidgetMs_ = 0;
      }
    }
    HidActions::dispatch(action);
    return;
  }
  HidActions::dispatch(action);
}
