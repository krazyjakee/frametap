#pragma once

#include <cstddef>
#include <cstdint>
#include <frametap/frametap.h>

namespace frametap::internal {

// Returns width * height * 4, or throws CaptureError if the multiplication
// would overflow size_t. Use this before allocating pixel buffers.
inline size_t checked_rgba_size(size_t width, size_t height) {
  if (width == 0 || height == 0)
    return 0;
  // Check: width * height overflows?
  if (width > SIZE_MAX / height)
    throw CaptureError(
        "Image dimensions too large: overflow in pixel buffer allocation");
  size_t pixels = width * height;
  // Check: pixels * 4 overflows?
  if (pixels > SIZE_MAX / 4)
    throw CaptureError(
        "Image dimensions too large: overflow in pixel buffer allocation");
  return pixels * 4;
}

} // namespace frametap::internal
