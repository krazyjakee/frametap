#include <catch2/catch_test_macros.hpp>

#include "platform/android/pixel_convert.h"

#include <cstdint>
#include <cstring>
#include <vector>

using namespace frametap::internal::android;

// ---------------------------------------------------------------------------
// bytes_per_pixel
// ---------------------------------------------------------------------------

TEST_CASE("bytes_per_pixel for known formats", "[unit][android]") {
  CHECK(bytes_per_pixel(PIXEL_FORMAT_RGBA_8888) == 4);
  CHECK(bytes_per_pixel(PIXEL_FORMAT_RGBX_8888) == 4);
  CHECK(bytes_per_pixel(PIXEL_FORMAT_BGRA_8888) == 4);
  CHECK(bytes_per_pixel(PIXEL_FORMAT_RGB_888) == 3);
  CHECK(bytes_per_pixel(PIXEL_FORMAT_RGB_565) == 2);
}

TEST_CASE("bytes_per_pixel returns 0 for unknown format", "[unit][android]") {
  CHECK(bytes_per_pixel(0) == 0);
  CHECK(bytes_per_pixel(99) == 0);
}

// ---------------------------------------------------------------------------
// convert_to_rgba — RGBA_8888 (identity)
// ---------------------------------------------------------------------------

TEST_CASE("convert_to_rgba RGBA_8888 identity copy", "[unit][android]") {
  uint8_t src[] = {10, 20, 30, 40, 50, 60, 70, 80};
  uint8_t dst[8] = {};

  convert_to_rgba(src, dst, 2, PIXEL_FORMAT_RGBA_8888);

  CHECK(std::memcmp(src, dst, 8) == 0);
}

// ---------------------------------------------------------------------------
// convert_to_rgba — RGBX_8888 (force alpha)
// ---------------------------------------------------------------------------

TEST_CASE("convert_to_rgba RGBX_8888 forces alpha to 0xFF", "[unit][android]") {
  uint8_t src[] = {10, 20, 30, 0, 50, 60, 70, 0};
  uint8_t dst[8] = {};

  convert_to_rgba(src, dst, 2, PIXEL_FORMAT_RGBX_8888);

  // RGB preserved
  CHECK(dst[0] == 10);
  CHECK(dst[1] == 20);
  CHECK(dst[2] == 30);
  CHECK(dst[3] == 0xFF); // alpha forced

  CHECK(dst[4] == 50);
  CHECK(dst[5] == 60);
  CHECK(dst[6] == 70);
  CHECK(dst[7] == 0xFF); // alpha forced
}

// ---------------------------------------------------------------------------
// convert_to_rgba — BGRA_8888 (channel swap)
// ---------------------------------------------------------------------------

TEST_CASE("convert_to_rgba BGRA_8888 swaps R and B", "[unit][android]") {
  uint8_t src[] = {100, 150, 200, 255}; // B=100, G=150, R=200, A=255
  uint8_t dst[4] = {};

  convert_to_rgba(src, dst, 1, PIXEL_FORMAT_BGRA_8888);

  CHECK(dst[0] == 200); // R
  CHECK(dst[1] == 150); // G
  CHECK(dst[2] == 100); // B
  CHECK(dst[3] == 255); // A
}

// ---------------------------------------------------------------------------
// convert_to_rgba — RGB_888 (3bpp → 4bpp)
// ---------------------------------------------------------------------------

TEST_CASE("convert_to_rgba RGB_888 expands to RGBA", "[unit][android]") {
  uint8_t src[] = {10, 20, 30, 40, 50, 60}; // 2 pixels, 3 bytes each
  uint8_t dst[8] = {};

  convert_to_rgba(src, dst, 2, PIXEL_FORMAT_RGB_888);

  CHECK(dst[0] == 10);
  CHECK(dst[1] == 20);
  CHECK(dst[2] == 30);
  CHECK(dst[3] == 0xFF);

  CHECK(dst[4] == 40);
  CHECK(dst[5] == 50);
  CHECK(dst[6] == 60);
  CHECK(dst[7] == 0xFF);
}

// ---------------------------------------------------------------------------
// convert_to_rgba — RGB_565 (16-bit unpack)
// ---------------------------------------------------------------------------

TEST_CASE("convert_to_rgba RGB_565 all zeros", "[unit][android]") {
  uint16_t src16 = 0x0000;
  uint8_t dst[4] = {};

  convert_to_rgba(reinterpret_cast<const uint8_t *>(&src16), dst, 1,
                  PIXEL_FORMAT_RGB_565);

  CHECK(dst[0] == 0);    // R
  CHECK(dst[1] == 0);    // G
  CHECK(dst[2] == 0);    // B
  CHECK(dst[3] == 0xFF); // A
}

TEST_CASE("convert_to_rgba RGB_565 all max", "[unit][android]") {
  uint16_t src16 = 0xFFFF; // R=31, G=63, B=31
  uint8_t dst[4] = {};

  convert_to_rgba(reinterpret_cast<const uint8_t *>(&src16), dst, 1,
                  PIXEL_FORMAT_RGB_565);

  CHECK(dst[0] == 255);  // R
  CHECK(dst[1] == 255);  // G
  CHECK(dst[2] == 255);  // B
  CHECK(dst[3] == 0xFF); // A
}

TEST_CASE("convert_to_rgba RGB_565 midpoint", "[unit][android]") {
  // R=16, G=32, B=16 → 10000_100000_10000 = 0x8410
  uint16_t src16 = 0x8410;
  uint8_t dst[4] = {};

  convert_to_rgba(reinterpret_cast<const uint8_t *>(&src16), dst, 1,
                  PIXEL_FORMAT_RGB_565);

  // (16 * 255 + 15) / 31 = 4095 / 31 = 132
  CHECK(dst[0] == 132);
  // (32 * 255 + 31) / 63 = 8191 / 63 = 130
  CHECK(dst[1] == 130);
  // (16 * 255 + 15) / 31 = 132
  CHECK(dst[2] == 132);
  CHECK(dst[3] == 0xFF);
}

// ---------------------------------------------------------------------------
// Multi-pixel and edge cases
// ---------------------------------------------------------------------------

TEST_CASE("convert_to_rgba multi-pixel BGRA buffer", "[unit][android]") {
  uint8_t src[] = {
      100, 150, 200, 255, // pixel 0 BGRA
      10,  20,  30,  128, // pixel 1 BGRA
  };
  uint8_t dst[8] = {};

  convert_to_rgba(src, dst, 2, PIXEL_FORMAT_BGRA_8888);

  CHECK(dst[0] == 200);
  CHECK(dst[1] == 150);
  CHECK(dst[2] == 100);
  CHECK(dst[3] == 255);
  CHECK(dst[4] == 30);
  CHECK(dst[5] == 20);
  CHECK(dst[6] == 10);
  CHECK(dst[7] == 128);
}

TEST_CASE("convert_to_rgba zero pixel count is a no-op", "[unit][android]") {
  uint8_t src[] = {1, 2, 3, 4};
  uint8_t dst[] = {0xAA, 0xBB, 0xCC, 0xDD};

  convert_to_rgba(src, dst, 0, PIXEL_FORMAT_RGBA_8888);

  // dst untouched
  CHECK(dst[0] == 0xAA);
  CHECK(dst[1] == 0xBB);
  CHECK(dst[2] == 0xCC);
  CHECK(dst[3] == 0xDD);
}
