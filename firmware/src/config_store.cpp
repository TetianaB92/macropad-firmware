#include "config_store.h"
#include "frame_buffer.h"
#include "json_memory.h"
#include "rgb565_rle.h"
#include <Arduino.h>
#include <string.h>
#if defined(ESP_PLATFORM)
#include <esp_task_wdt.h>
#endif

static const char* kLayoutPath = "/layout.json";
static const char* kScenePath = "/scene.json";
static const char* kChromePath = "/chrome.bin";
static const char* kChromeTemp = "/chrome.tmp";
static constexpr uint32_t kChromeMagic = 0x4D504348;  // 'MPCH'
static constexpr uint32_t kChromeVersion = 1;
static constexpr uint32_t kChromePersistMinMs = 10UL * 60UL * 1000UL;

struct ChromeHeader {
  uint32_t magic;
  uint32_t version;
  uint16_t width;
  uint16_t height;
  uint32_t crc32;
  uint32_t length;
  char pageId[64];
};
static_assert(sizeof(ChromeHeader) == 84, "chrome header must stay packed");

static bool fsReady = false;
static uint32_t lastChromeWriteMs = 0;

static bool ensureFs() {
  if (fsReady) return true;
  fsReady = LittleFS.begin(true);
  return fsReady;
}

static uint32_t crc32Bytes(const uint8_t* data, size_t length) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
  }
  return ~crc;
}

static bool readChromeHeader(ChromeHeader* out) {
  if (!out || !ensureFs() || !LittleFS.exists(kChromePath)) return false;
  File f = LittleFS.open(kChromePath, "r");
  if (!f) return false;
  const size_t got = f.read(reinterpret_cast<uint8_t*>(out), sizeof(*out));
  f.close();
  return got == sizeof(*out) && out->magic == kChromeMagic &&
         (out->version == kChromeVersion || out->version == 2) && out->pageId[0] &&
         memchr(out->pageId, '\0', sizeof(out->pageId));
}

bool ConfigStore::cachedChromePage(char* pageId, size_t capacity, int width, int height) {
  ChromeHeader header{};
  if (!pageId || capacity < sizeof(header.pageId) || !readChromeHeader(&header) ||
      header.width != width || header.height != height ||
      header.length != size_t(width) * height * 2) return false;
  memcpy(pageId, header.pageId, sizeof(header.pageId));
  return true;
}

static bool writeAll(File& file, const uint8_t* bytes, size_t length) {
  size_t offset = 0;
  while (offset < length) {
    size_t chunk = length - offset;
    if (chunk > 16384) chunk = 16384;
    if (file.write(bytes + offset, chunk) != chunk) return false;
    offset += chunk;
    delay(0);
#if defined(ESP_PLATFORM)
    esp_task_wdt_reset();
#endif
  }
  return true;
}

void ConfigStore::init() {
  fsReady = LittleFS.begin(true);
  if (!fsReady) {
    Serial.println("ConfigStore: LittleFS mount failed");
    return;
  }
  Serial.println("ConfigStore: LittleFS ready");
}

bool ConfigStore::loadFromFlash(JsonDocument& dest) {
  if (!ensureFs() || !LittleFS.exists(kLayoutPath)) return false;
  File f = LittleFS.open(kLayoutPath, "r");
  if (!f) return false;
  DeserializationError err = deserializeJson(dest, f, DeserializationOption::NestingLimit(SCENE_NESTING_LIMIT));
  f.close();
  if (err) {
    Serial.printf("ConfigStore: load error %s\n", err.c_str());
    return false;
  }
  return true;
}

bool ConfigStore::saveToFlash(const JsonDocument& doc) {
  if (!ensureFs()) return false;
  File f = LittleFS.open(kLayoutPath, "w");
  if (!f) return false;
  serializeJson(doc, f);
  f.close();
  return true;
}

bool ConfigStore::loadScene(String& dest) {
  if (!ensureFs() || !LittleFS.exists(kScenePath)) return false;
  File f = LittleFS.open(kScenePath, "r");
  if (!f) return false;
  dest = f.readString();
  f.close();
  return dest.length() > 8;
}

bool ConfigStore::saveScene(const char* json) {
  if (!json || !ensureFs()) return false;
  const char* temporary = "/scene.tmp";
  File f = LittleFS.open(temporary, "w");
  if (!f) return false;
  const size_t written = f.print(json);
  f.flush();
  f.close();
  if (written != strlen(json)) {
    LittleFS.remove(temporary);
    return false;
  }
  if (!LittleFS.rename(temporary, kScenePath)) {
    LittleFS.remove(temporary);
    return false;
  }
  return true;
}

bool ConfigStore::saveChrome(const char* pageId, int width, int height,
                             const uint8_t* bytes, size_t length) {
  if (!pageId || !pageId[0] || !bytes || width <= 0 || height <= 0) return false;
  const size_t expected = size_t(width) * size_t(height) * 2;
  if (length != expected) return false;
  if (!ensureFs()) return false;

  const uint32_t crc = crc32Bytes(bytes, length);
  ChromeHeader existing {};
  const bool haveExisting = readChromeHeader(&existing);
  if (haveExisting && existing.width == uint16_t(width) &&
      existing.height == uint16_t(height) && existing.length == length &&
      existing.crc32 == crc && strcmp(existing.pageId, pageId) == 0) {
    return true;
  }
  const uint32_t now = millis();
  if (haveExisting && lastChromeWriteMs && now - lastChromeWriteMs < kChromePersistMinMs) {
    return true;
  }

  ChromeHeader header {};
  header.magic = kChromeMagic;
  header.version = kChromeVersion;
  header.width = uint16_t(width);
  header.height = uint16_t(height);
  header.crc32 = crc;
  header.length = uint32_t(length);
  strncpy(header.pageId, pageId, sizeof(header.pageId) - 1);

  auto* compressed = allocFrameBuffer(length);
  const size_t packed = compressed ? encodeRgb565Runs(bytes, length, compressed, length) : 0;
  const bool usePacked = packed && packed < length;
  if (usePacked) header.version = 2;

  const uint32_t started = millis();
  File f = LittleFS.open(kChromeTemp, "w");
  if (!f) {
    releaseFrameBuffer(compressed);
    Serial.println("Chrome persist: temp open failed");
    return false;
  }
  const bool ok =
      writeAll(f, reinterpret_cast<const uint8_t*>(&header), sizeof(header)) &&
      writeAll(f, usePacked ? compressed : bytes, usePacked ? packed : length);
  releaseFrameBuffer(compressed);
  f.flush();
  f.close();
  if (!ok) {
    LittleFS.remove(kChromeTemp);
    Serial.println("Chrome persist: write failed");
    return false;
  }
  if (!LittleFS.rename(kChromeTemp, kChromePath)) {
    LittleFS.remove(kChromeTemp);
    Serial.println("Chrome persist: rename failed");
    return false;
  }
  lastChromeWriteMs = millis();
  Serial.printf("Chrome persisted page=%s bytes=%u crc=%08lx ms=%lu\n", pageId,
                unsigned(usePacked ? packed : length), (unsigned long)crc,
                (unsigned long)(lastChromeWriteMs - started));
  return true;
}

bool ConfigStore::loadChrome(const char* pageId, int width, int height,
                             uint8_t** bytesOut, size_t* lengthOut) {
  if (bytesOut) *bytesOut = nullptr;
  if (lengthOut) *lengthOut = 0;
  if (!pageId || !pageId[0] || !bytesOut || width <= 0 || height <= 0) return false;
  ChromeHeader header {};
  if (!readChromeHeader(&header)) return false;
  if (header.width != uint16_t(width) || header.height != uint16_t(height) ||
      strcmp(header.pageId, pageId) != 0) {
    return false;
  }
  const size_t expected = size_t(width) * size_t(height) * 2;
  if (header.length != expected) return false;

  File f = LittleFS.open(kChromePath, "r");
  if (!f) return false;
  uint8_t skip[sizeof(ChromeHeader)];
  if (f.read(skip, sizeof(skip)) != sizeof(skip)) {
    f.close();
    return false;
  }
  uint8_t* bytes = allocFrameBuffer(expected);
  if (!bytes) {
    f.close();
    return false;
  }
  size_t got = 0;
  while (got < expected) {
    size_t chunk = expected - got;
    if (chunk > 16384) chunk = 16384;
    const size_t n = f.read(bytes + got, chunk);
    if (n == 0) break;
    got += n;
  }
  f.close();
  if (header.version == 2) {
    auto* decoded = allocFrameBuffer(expected);
    const bool valid = decoded && decodeRgb565Runs(bytes, got, decoded, expected);
    releaseFrameBuffer(bytes);
    if (!valid) { releaseFrameBuffer(decoded); return false; }
    bytes = decoded; got = expected;
  }
  if (got != expected || crc32Bytes(bytes, expected) != header.crc32) {
    releaseFrameBuffer(bytes);
    Serial.println("Chrome restore: corrupt flash copy");
    return false;
  }
  *bytesOut = bytes;
  if (lengthOut) *lengthOut = expected;
  return true;
}
