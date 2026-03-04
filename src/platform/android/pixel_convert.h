#pragma once

#include "../../util/color.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace frametap::internal::android {

// Android screencap pixel format constants (from AOSP PixelFormat.h)
constexpr uint32_t PIXEL_FORMAT_RGBA_8888 = 1;
constexpr uint32_t PIXEL_FORMAT_RGBX_8888 = 2;
constexpr uint32_t PIXEL_FORMAT_RGB_888 = 3;
constexpr uint32_t PIXEL_FORMAT_RGB_565 = 4;
constexpr uint32_t PIXEL_FORMAT_BGRA_8888 = 5;

// Bytes per pixel for each format (0 = unsupported)
inline int bytes_per_pixel(uint32_t format) {
  switch (format) {
  case PIXEL_FORMAT_RGBA_8888:
  case PIXEL_FORMAT_RGBX_8888:
  case PIXEL_FORMAT_BGRA_8888:
    return 4;
  case PIXEL_FORMAT_RGB_888:
    return 3;
  case PIXEL_FORMAT_RGB_565:
    return 2;
  default:
    return 0;
  }
}

// Convert pixel data from the screencap format to RGBA_8888.
// `data` has `pixel_count * bpp` bytes; `out` must hold `pixel_count * 4`.
inline void convert_to_rgba(const uint8_t *data, uint8_t *out,
                            size_t pixel_count, uint32_t format) {
  switch (format) {
  case PIXEL_FORMAT_RGBA_8888:
    // Already RGBA — straight copy
    std::memcpy(out, data, pixel_count * 4);
    break;

  case PIXEL_FORMAT_RGBX_8888:
    // Same layout as RGBA but alpha is undefined — copy + force alpha
    std::memcpy(out, data, pixel_count * 4);
    for (size_t i = 0; i < pixel_count; i++)
      out[i * 4 + 3] = 0xFF;
    break;

  case PIXEL_FORMAT_BGRA_8888:
    bgra_to_rgba(data, out, pixel_count);
    break;

  case PIXEL_FORMAT_RGB_888:
    // Expand 3 bytes → 4 bytes per pixel
    for (size_t i = 0; i < pixel_count; i++) {
      out[i * 4 + 0] = data[i * 3 + 0]; // R
      out[i * 4 + 1] = data[i * 3 + 1]; // G
      out[i * 4 + 2] = data[i * 3 + 2]; // B
      out[i * 4 + 3] = 0xFF;            // A
    }
    break;

  case PIXEL_FORMAT_RGB_565: {
    // Unpack 16-bit 5-6-5 to RGBA
    const auto *src16 = reinterpret_cast<const uint16_t *>(data);
    for (size_t i = 0; i < pixel_count; i++) {
      uint16_t px = src16[i];
      uint8_t r5 = (px >> 11) & 0x1F;
      uint8_t g6 = (px >> 5) & 0x3F;
      uint8_t b5 = px & 0x1F;
      out[i * 4 + 0] = static_cast<uint8_t>((r5 * 255 + 15) / 31);
      out[i * 4 + 1] = static_cast<uint8_t>((g6 * 255 + 31) / 63);
      out[i * 4 + 2] = static_cast<uint8_t>((b5 * 255 + 15) / 31);
      out[i * 4 + 3] = 0xFF;
    }
    break;
  }

  default:
    break;
  }
}

} // namespace frametap::internal::android
