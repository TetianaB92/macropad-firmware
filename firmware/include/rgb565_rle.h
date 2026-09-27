#pragma once
#include <stddef.h>
#include <stdint.h>

// Lossless, byte-order-independent runs: little-endian count, then RGB565.
// Return zero if compression would not fit; callers retain the raw format.
inline size_t encodeRgb565Runs(const uint8_t* src, size_t size, uint8_t* out, size_t capacity) {
  if (!src || !out || !size || size % 2) return 0;
  size_t written = 0;
  for (size_t pos = 0; pos < size;) {
    size_t count = 1;
    while (count < 65535 && pos + count * 2 < size &&
           src[pos] == src[pos + count * 2] && src[pos + 1] == src[pos + count * 2 + 1]) ++count;
    if (capacity - written < 4) return 0;
    out[written++] = uint8_t(count); out[written++] = uint8_t(count >> 8);
    out[written++] = src[pos]; out[written++] = src[pos + 1];
    pos += count * 2;
  }
  return written;
}
inline bool decodeRgb565Runs(const uint8_t* src, size_t size, uint8_t* out, size_t expected) {
  if (!src || !out || !size || size % 4 || expected % 2) return false;
  size_t written = 0;
  for (size_t pos = 0; pos < size; pos += 4) {
    size_t count = size_t(src[pos]) | (size_t(src[pos + 1]) << 8);
    if (!count || count > (expected - written) / 2) return false;
    while (count--) { out[written++] = src[pos + 2]; out[written++] = src[pos + 3]; }
  }
  return written == expected;
}
