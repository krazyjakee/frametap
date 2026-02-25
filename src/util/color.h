#pragma once

#include <cstddef>
#include <cstdint>

namespace frametap::internal {

// Convert BGRA pixel buffer to RGBA in-place.
inline void bgra_to_rgba(uint8_t *buf, size_t pixel_count) {
  for (size_t i = 0; i < pixel_count * 4; i += 4) {
    uint8_t tmp = buf[i + 0];   // B
    buf[i + 0] = buf[i + 2];    // R
    buf[i + 2] = tmp;            // B
    // G and A stay
  }
}

// Convert BGRA source to RGBA destination buffer.
inline void bgra_to_rgba(const uint8_t *src, uint8_t *dst,
                          size_t pixel_count) {
  for (size_t i = 0; i < pixel_count * 4; i += 4) {
    dst[i + 0] = src[i + 2]; // R <- B
    dst[i + 1] = src[i + 1]; // G
    dst[i + 2] = src[i + 0]; // B <- R
    dst[i + 3] = src[i + 3]; // A
  }
}

} // namespace frametap::internal
