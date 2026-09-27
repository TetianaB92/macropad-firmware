#pragma once
#include <ArduinoJson.h>
#include <stdint.h>
#include <string.h>

// Platform-neutral countdown model. Epoch timestamps are always 64-bit;
// deadlines of locally started timers use a monotonic clock (including offline).
namespace CountdownRuntime {
inline uint64_t durationMs(JsonObject value) {
  int seconds = value["durationSec"] | 1500;
  return uint64_t(seconds > 0 ? seconds : 1) * 1000;
}
inline uint64_t remainingMs(JsonObject value, uint64_t epochMs, uint64_t monoMs) {
  if (strcmp(value["mode"] | "idle", "countdown") == 0) {
    const uint64_t localDeadline = value["_deadlineMs"].as<uint64_t>();
    const uint64_t deadline = localDeadline ? localDeadline : value["endAt"].as<uint64_t>();
    const uint64_t now = localDeadline ? monoMs : epochMs;
    return deadline > now ? deadline - now : 0;
  }
  if (value["remainingMs"].is<uint64_t>()) return value["remainingMs"].as<uint64_t>();
  return uint64_t(value["remainingSec"].as<unsigned>()) * 1000;
}
inline int remainingSeconds(JsonObject value, uint64_t epochMs, uint64_t monoMs) {
  return int((remainingMs(value, epochMs, monoMs) + 999) / 1000);
}
inline void reset(JsonObject value) {
  value["mode"] = "idle";
  value["endAt"] = uint64_t(0);
  value.remove("_deadlineMs");
  value["remainingMs"] = durationMs(value);
  value["remainingSec"] = durationMs(value) / 1000;
}
inline void toggle(JsonObject value, uint64_t epochMs, uint64_t monoMs) {
  uint64_t left = remainingMs(value, epochMs, monoMs);
  if (strcmp(value["mode"] | "idle", "countdown") == 0) {
    value["mode"] = "paused";
    value["endAt"] = uint64_t(0);
    value.remove("_deadlineMs");
  } else {
    if (!left) left = durationMs(value);
    value["mode"] = "countdown";
    value["endAt"] = epochMs + left;
    value["_deadlineMs"] = monoMs + left;
  }
  value["remainingMs"] = left;
  value["remainingSec"] = (left + 999) / 1000;
}
inline void tick(JsonObject value, uint64_t epochMs, uint64_t monoMs) {
  if (strcmp(value["mode"] | "idle", "countdown") != 0) return;
  const auto left = remainingMs(value, epochMs, monoMs);
  value["remainingMs"] = left;
  value["remainingSec"] = (left + 999) / 1000;
  if (!left) {
    value["mode"] = "idle";
    value["endAt"] = uint64_t(0);
    value.remove("_deadlineMs");
  }
}
} // namespace CountdownRuntime
