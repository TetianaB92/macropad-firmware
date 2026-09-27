#pragma once
#include <stdint.h>

// Give association and DHCP time to finish. No forced disconnect every 5 seconds.
class WifiRetry {
public:
  void started(uint32_t now) { since_=now; failures_=0; }
  void connected(uint32_t now) { since_=now; failures_=0; everConnected_=true; }
  bool due(uint32_t now) const { return uint32_t(now-since_) >= 30000U; }
  bool retry(uint32_t now) { since_=now;return ++failures_>=4 && !everConnected_; }
private:
  uint32_t since_=0;
  unsigned failures_=0;
  bool everConnected_=false;
};
