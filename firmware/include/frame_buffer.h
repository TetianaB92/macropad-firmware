#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <Arduino.h>
#include <esp_heap_caps.h>
#if defined(CONFIG_IDF_TARGET_ESP32P4)
#include <esp_cache.h>
#endif

inline void releaseFrameBuffer(uint8_t* ptr) {
  if (!ptr) return;
  heap_caps_free(ptr);
}

inline uint8_t* allocFrameBuffer(size_t bytes) {
  if (!bytes) return nullptr;
#if defined(CONFIG_IDF_TARGET_ESP32P4)
  uint8_t* aligned = static_cast<uint8_t*>(heap_caps_aligned_alloc(
      128, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (aligned) return aligned;
#endif
  uint8_t* spiram = static_cast<uint8_t*>(
      heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (spiram) return spiram;
  return static_cast<uint8_t*>(malloc(bytes));
}

/** Write this core's cached stores out to PSRAM (call after filling a frame). */
inline void psramWriteback(const void* ptr, size_t bytes) {
#if defined(CONFIG_IDF_TARGET_ESP32P4)
  if (!ptr || bytes == 0) return;
  esp_err_t err = esp_cache_msync(const_cast<void*>(ptr), bytes,
                                  ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                                      ESP_CACHE_MSYNC_FLAG_TYPE_DATA |
                                      ESP_CACHE_MSYNC_FLAG_UNALIGNED);
  if (err != ESP_OK) {
    Serial.printf("psramWriteback failed err=%d ptr=%p bytes=%u\n", (int)err,
                  ptr, (unsigned)bytes);
  }
#else
  (void)ptr;
  (void)bytes;
#endif
}

/** Drop this core's stale cache lines so DMA/CPU see PSRAM (never writeback).
 *  The M2C (invalidate) direction rejects ESP_CACHE_MSYNC_FLAG_UNALIGNED, so we
 *  snap the range out to the 64-byte data-cache line. Only valid for read-only
 *  buffers (e.g. the chrome bake) where dropping neighbouring clean lines is
 *  harmless. */
inline void psramInvalidate(const void* ptr, size_t bytes) {
#if defined(CONFIG_IDF_TARGET_ESP32P4)
  if (!ptr || bytes == 0) return;
  const uintptr_t kLine = 128;  // ESP32-P4 data cache line size
  uintptr_t start = reinterpret_cast<uintptr_t>(ptr);
  uintptr_t end = start + bytes;
  uintptr_t alignedStart = start & ~(kLine - 1);
  uintptr_t alignedEnd = (end + kLine - 1) & ~(kLine - 1);
  esp_err_t err = esp_cache_msync(reinterpret_cast<void*>(alignedStart),
                                  (size_t)(alignedEnd - alignedStart),
                                  ESP_CACHE_MSYNC_FLAG_DIR_M2C |
                                      ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
  if (err != ESP_OK) {
    Serial.printf("psramInvalidate failed err=%d ptr=%p bytes=%u\n", (int)err,
                  ptr, (unsigned)bytes);
  }
#else
  (void)ptr;
  (void)bytes;
#endif
}
