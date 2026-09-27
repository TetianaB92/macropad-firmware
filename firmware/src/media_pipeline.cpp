#include "media_pipeline.h"
#include "board_config.h"
#if !defined(MACRO_PAD_USE_ESP_PANEL)
#include "device_state.h"
#include <HTTPClient.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <TJpg_Decoder.h>
#endif

#if defined(MACRO_PAD_USE_ESP_PANEL)

void MediaPipeline::init() {
  Serial.println("MediaPipeline: disabled on image-first ESP32-P4 path");
}

void MediaPipeline::detachAll() {}

bool MediaPipeline::attach(lv_obj_t* img, const char* url, bool mjpeg,
                           int refreshMs) {
  (void)img;
  (void)url;
  (void)mjpeg;
  (void)refreshMs;
  return false;
}

bool MediaPipeline::attachAlbum(lv_obj_t* img, const char* const* urls,
                                int count, uint32_t rotateMs) {
  (void)img;
  (void)urls;
  (void)count;
  (void)rotateMs;
  return false;
}

void MediaPipeline::loop() {}

#else


MediaPipeline::Slot MediaPipeline::slots_[kSlots];

static uint16_t* g_decodeTarget = nullptr;
static int g_decodeW = 0;
static int g_decodeH = 0;
static int g_decodeMaxW = 0;
static int g_decodeMaxH = 0;

static bool tjpg_output(int16_t x, int16_t y, uint16_t w, uint16_t h,
                        uint16_t* bitmap) {
  if (!g_decodeTarget || !bitmap) return false;
  if (x >= g_decodeMaxW || y >= g_decodeMaxH) return true;
  for (int16_t row = 0; row < (int16_t)h; row++) {
    int dy = y + row;
    if (dy < 0 || dy >= g_decodeMaxH) continue;
    int copyW = w;
    if (x + copyW > g_decodeMaxW) copyW = g_decodeMaxW - x;
    if (x < 0) continue;
    memcpy(g_decodeTarget + dy * g_decodeMaxW + x, bitmap + row * w,
           (size_t)copyW * sizeof(uint16_t));
  }
  if (x + (int)w > g_decodeW) g_decodeW = x + w;
  if (y + (int)h > g_decodeH) g_decodeH = y + h;
  return true;
}

void MediaPipeline::init() {
  memset(slots_, 0, sizeof(slots_));
  TJpgDec.setSwapBytes(true);
  TJpgDec.setCallback(tjpg_output);
  Serial.println("MediaPipeline: ready (JPEG/MJPEG)");
}

void MediaPipeline::detachAll() {
  for (int i = 0; i < kSlots; i++) {
    if (slots_[i].rgb) {
      free(slots_[i].rgb);
      slots_[i].rgb = nullptr;
    }
    slots_[i].used = false;
    slots_[i].img = nullptr;
  }
}

bool MediaPipeline::httpFetch(const char* url, uint8_t** out, size_t* outLen) {
  *out = nullptr;
  *outLen = 0;
  if (!url || !url[0] || WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  http.begin(url);
  http.setTimeout(12000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (strstr(url, "/api/devices/") && DeviceState::deviceToken()[0]) {
    http.addHeader("Authorization",
                   String("Bearer ") + DeviceState::deviceToken());
  }
  int code = http.GET();
  if (code != 200) {
    Serial.printf("Media GET %d %s\n", code, url);
    http.end();
    return false;
  }
  int len = http.getSize();
  if (len <= 0 || len > MEDIA_JPEG_MAX_BYTES) len = MEDIA_JPEG_MAX_BYTES;

  uint8_t* buf =
      (uint8_t*)heap_caps_malloc(len + 8, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) buf = (uint8_t*)malloc(len + 8);
  if (!buf) {
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  size_t got = 0;
  while (http.connected() && got < (size_t)len) {
    size_t avail = stream->available();
    if (!avail) {
      delay(1);
      continue;
    }
    int r = stream->readBytes(buf + got, min(avail, len - got));
    if (r <= 0) break;
    got += r;
  }
  http.end();
  if (got < 100) {
    free(buf);
    return false;
  }
  *out = buf;
  *outLen = got;
  return true;
}

bool MediaPipeline::httpFetchMjpegFrame(const char* url, uint8_t** out,
                                        size_t* outLen) {
  // Many IP cams: multipart/x-mixed-replace. We read a chunk and find SOI/EOI.
  *out = nullptr;
  *outLen = 0;
  if (!url || WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  http.begin(url);
  http.setTimeout(8000);
  int code = http.GET();
  if (code != 200) {
    http.end();
    return false;
  }

  const size_t cap = MEDIA_JPEG_MAX_BYTES;
  uint8_t* buf =
      (uint8_t*)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) {
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  size_t got = 0;
  uint32_t start = millis();
  while (millis() - start < 6000 && got < cap) {
    while (stream->available() && got < cap) {
      buf[got++] = stream->read();
    }
    // Look for JPEG SOI … EOI
    int soi = -1, eoi = -1;
    for (size_t i = 0; i + 1 < got; i++) {
      if (buf[i] == 0xFF && buf[i + 1] == 0xD8) {
        soi = (int)i;
        break;
      }
    }
    if (soi >= 0) {
      for (size_t i = soi + 2; i + 1 < got; i++) {
        if (buf[i] == 0xFF && buf[i + 1] == 0xD9) {
          eoi = (int)i + 2;
          break;
        }
      }
    }
    if (soi >= 0 && eoi > soi) {
      size_t frameLen = (size_t)(eoi - soi);
      uint8_t* frame = (uint8_t*)heap_caps_malloc(
          frameLen, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!frame) frame = (uint8_t*)malloc(frameLen);
      if (frame) {
        memcpy(frame, buf + soi, frameLen);
        free(buf);
        http.end();
        *out = frame;
        *outLen = frameLen;
        return true;
      }
      break;
    }
    delay(2);
  }
  free(buf);
  http.end();
  // Fallback: treat whole response as still JPEG
  return httpFetch(url, out, outLen);
}

bool MediaPipeline::decodeJpegToSlot(Slot& s, const uint8_t* jpg, size_t len) {
  if (!jpg || len < 100) return false;

  uint16_t jw = 0, jh = 0;
  if (TJpgDec.getJpgSize(&jw, &jh, jpg, len) != 0) return false;

  uint8_t scale = 1;
  while ((jw / scale > MEDIA_MAX_W || jh / scale > MEDIA_MAX_H) && scale < 8) {
    scale <<= 1;
  }

  TJpgDec.setJpgScale(scale);
  int dw = jw / scale;
  int dh = jh / scale;
  if (dw < 8) dw = 8;
  if (dh < 8) dh = 8;

  size_t bytes = (size_t)dw * dh * sizeof(uint16_t);
  uint16_t* rgb =
      (uint16_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!rgb) rgb = (uint16_t*)malloc(bytes);
  if (!rgb) return false;
  memset(rgb, 0, bytes);

  g_decodeTarget = rgb;
  g_decodeMaxW = dw;
  g_decodeMaxH = dh;
  g_decodeW = 0;
  g_decodeH = 0;
  JRESULT result = TJpgDec.drawJpg(0, 0, jpg, len);
  g_decodeTarget = nullptr;
  if (result != JDR_OK) {
    free(rgb);
    return false;
  }

  if (s.rgb) free(s.rgb);
  s.rgb = rgb;
  s.w = dw;
  s.h = dh;
  s.dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
  s.dsc.header.w = dw;
  s.dsc.header.h = dh;
  s.dsc.header.always_zero = 0;
  s.dsc.data_size = bytes;
  s.dsc.data = (const uint8_t*)rgb;

  if (s.img) {
    lv_img_set_src(s.img, &s.dsc);
  }
  return true;
}

void MediaPipeline::refreshSlot(Slot& s) {
  if (!s.used || !s.img) return;

  const char* url = s.url;
  if (s.album && s.albumCount > 0) {
    s.albumIndex = (s.albumIndex + 1) % s.albumCount;
    url = s.albumUrls[s.albumIndex];
  }

  uint8_t* jpg = nullptr;
  size_t len = 0;
  bool ok = s.mjpeg ? httpFetchMjpegFrame(url, &jpg, &len)
                    : httpFetch(url, &jpg, &len);
  if (!ok || !jpg) return;
  decodeJpegToSlot(s, jpg, len);
  free(jpg);
  s.lastMs = millis();
}

bool MediaPipeline::attach(lv_obj_t* img, const char* url, bool mjpeg,
                           int refreshMs) {
  if (!img || !url) return false;
  for (int i = 0; i < kSlots; i++) {
    if (slots_[i].used) continue;
    Slot& s = slots_[i];
    memset(&s, 0, sizeof(s));
    s.used = true;
    s.img = img;
    s.mjpeg = mjpeg;
    s.pollMs = refreshMs < 0 ? MEDIA_POLL_MS : (uint32_t)refreshMs;
    strncpy(s.url, url, sizeof(s.url) - 1);
    s.lastMs = 0;
    refreshSlot(s);
    return true;
  }
  return false;
}

bool MediaPipeline::attachAlbum(lv_obj_t* img, const char* const* urls,
                                int count, uint32_t rotateMs) {
  if (!img || !urls || count <= 0) return false;
  for (int i = 0; i < kSlots; i++) {
    if (slots_[i].used) continue;
    Slot& s = slots_[i];
    memset(&s, 0, sizeof(s));
    s.used = true;
    s.img = img;
    s.album = true;
    s.albumCount = min(count, 8);
    s.rotateMs = rotateMs ? rotateMs : 12000;
    for (int u = 0; u < s.albumCount; u++) {
      strncpy(s.albumUrls[u], urls[u] ? urls[u] : "", sizeof(s.albumUrls[u]) - 1);
    }
    strncpy(s.url, s.albumUrls[0], sizeof(s.url) - 1);
    refreshSlot(s);
    return true;
  }
  return false;
}

void MediaPipeline::loop() {
  uint32_t now = millis();
  for (int i = 0; i < kSlots; i++) {
    Slot& s = slots_[i];
    if (!s.used) continue;
    if (!s.mjpeg && !s.album && s.pollMs == 0) continue;
    uint32_t every =
        s.mjpeg ? CAMERA_FRAME_MS : (s.album ? s.rotateMs : s.pollMs);
    if (now - s.lastMs >= every) refreshSlot(s);
  }
}

#endif
