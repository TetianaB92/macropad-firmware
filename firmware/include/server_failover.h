#pragma once
#include <cstdint>

// Network-task-only routing. Never replays a command after an ambiguous failure.
class ServerFailover {
public:
  static constexpr uint32_t probeIntervalMs = 30000;
  bool useLocal() const { return local_; }
  bool probeDue(uint32_t now) const {
    return !probed_ || uint32_t(now - checkedAt_) >= probeIntervalMs;
  }
  void probeResult(uint32_t now, bool healthy) {
    probed_ = true; checkedAt_ = now; local_ = healthy;
  }
  void response(uint32_t now, int status) {
    if (local_ && (status <= 0 || status >= 500)) probeResult(now, false);
  }
private:
  bool local_ = false, probed_ = false;
  uint32_t checkedAt_ = 0;
};
