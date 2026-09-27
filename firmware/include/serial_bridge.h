#pragma once
#include <ArduinoJson.h>
#include <string>
#include <atomic>
#include "bridge_queue.h"
namespace SerialBridge {
inline std::atomic<unsigned> patches{0}, invalid{0}, dropped{0}, applied{0};
inline void send(const JsonDocument& document) {
  std::string json;serializeJson(document,json);json+='\n';
  char* bytes=strdup(json.c_str());if(!bytes) return;
  if(!serialTxQueue || xQueueSend(serialTxQueue,&bytes,0)!=pdTRUE) free(bytes);
}
void task(void*);
}
