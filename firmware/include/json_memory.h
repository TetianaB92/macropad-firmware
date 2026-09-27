#pragma once
#include <ArduinoJson.h>
#include <cstring>

// Envelope + pages + nested primitive children exceed ArduinoJson's default 10.
inline constexpr uint8_t SCENE_NESTING_LIMIT = 32;
#if defined(CONFIG_IDF_TARGET_ESP32P4)
#include <esp_heap_caps.h>
struct SceneAllocator : ArduinoJson::Allocator {
  void* allocate(size_t size) override { return heap_caps_malloc(size,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT); }
  void deallocate(void* ptr) override { heap_caps_free(ptr); }
  void* reallocate(void* ptr,size_t size) override { return heap_caps_realloc(ptr,size,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT); }
};
inline SceneAllocator sceneAllocator;
struct SceneDocument : JsonDocument { SceneDocument():JsonDocument(&sceneAllocator){} };
inline char* copySceneBytes(const char* text) { auto* result=static_cast<char*>(heap_caps_malloc(strlen(text)+1,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));if(result) strcpy(result,text);return result; }
#else
using SceneDocument=JsonDocument;
inline char* copySceneBytes(const char* text) { return strdup(text); }
#endif
