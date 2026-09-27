#pragma once
#include <cmath>
#include <cstdint>

// Generic HSV disc: hue around the rim, saturation from center to edge.
namespace ColorDisc {
inline bool pick(float x, float y, uint32_t& color) {
  const float saturation = std::hypot(x, y);
  if (!std::isfinite(saturation) || saturation > 1) return false;
  const float hue = std::fmod(std::atan2(y, x) * 180.0f / 3.14159265358979323846f + 360, 360) / 60;
  const float q = saturation * (1 - std::fabs(std::fmod(hue, 2) - 1));
  float r = 0, g = 0, b = 0;
  if (hue < 1) { r = saturation; g = q; }
  else if (hue < 2) { r = q; g = saturation; }
  else if (hue < 3) { g = saturation; b = q; }
  else if (hue < 4) { g = q; b = saturation; }
  else if (hue < 5) { r = q; b = saturation; }
  else { r = saturation; b = q; }
  auto channel = [saturation](float n) { return uint32_t(std::lround((n + 1 - saturation) * 255)); };
  color = (channel(r) << 16) | (channel(g) << 8) | channel(b);
  return true;
}
}
