#include <catch2/catch_test_macros.hpp>

// Internal header — tests verify the color conversion utility
#include "util/color.h"

#include <cstdint>
#include <vector>

using namespace frametap::internal;

// --- Section 2.3: Color conversion ---

TEST_CASE("BGRA to RGBA single pixel", "[unit][color]") {
  uint8_t buf[] = {100, 150, 200, 255}; // B=100, G=150, R=200, A=255
  bgra_to_rgba(buf, 1);
  CHECK(buf[0] == 200); // R
  CHECK(buf[1] == 150); // G
  CHECK(buf[2] == 100); // B
  CHECK(buf[3] == 255); // A
}

TEST_CASE("RGBA to BGRA roundtrip", "[unit][color]") {
  uint8_t original[] = {10, 20, 30, 40};
  uint8_t buf[] = {10, 20, 30, 40};

  // First conversion: treat as BGRA → RGBA (swaps R and B)
  bgra_to_rgba(buf, 1);
  // Second conversion: swaps them back
  bgra_to_rgba(buf, 1);

  CHECK(buf[0] == original[0]);
  CHECK(buf[1] == original[1]);
  CHECK(buf[2] == original[2]);
  CHECK(buf[3] == original[3]);
}

TEST_CASE("Full buffer conversion", "[unit][color]") {
  constexpr size_t width = 1920;
  constexpr size_t height = 1080;
  constexpr size_t pixels = width * height;

  std::vector<uint8_t> buf(pixels * 4);
  // Fill with known pattern: B=i%256, G=(i+1)%256, R=(i+2)%256, A=255
  for (size_t i = 0; i < pixels; ++i) {
    buf[i * 4 + 0] = static_cast<uint8_t>(i % 256);       // B
    buf[i * 4 + 1] = static_cast<uint8_t>((i + 1) % 256); // G
    buf[i * 4 + 2] = static_cast<uint8_t>((i + 2) % 256); // R
    buf[i * 4 + 3] = 255;                                  // A
  }

  bgra_to_rgba(buf.data(), pixels);

  // Verify all pixels converted correctly
  for (size_t i = 0; i < pixels; ++i) {
    REQUIRE(buf[i * 4 + 0] == static_cast<uint8_t>((i + 2) % 256)); // R
    REQUIRE(buf[i * 4 + 1] == static_cast<uint8_t>((i + 1) % 256)); // G
    REQUIRE(buf[i * 4 + 2] == static_cast<uint8_t>(i % 256));       // B
    REQUIRE(buf[i * 4 + 3] == 255);                                  // A
  }
}

TEST_CASE("In-place conversion", "[unit][color]") {
  uint8_t buf[] = {50, 100, 150, 200};
  uint8_t *ptr = buf;

  bgra_to_rgba(buf, 1);

  // Verify same buffer was modified (pointer unchanged)
  CHECK(ptr == buf);
  CHECK(buf[0] == 150); // R (was B)
  CHECK(buf[2] == 50);  // B (was R)
}

TEST_CASE("Src-dst conversion", "[unit][color]") {
  uint8_t src[] = {100, 150, 200, 255}; // BGRA
  uint8_t dst[4] = {};

  bgra_to_rgba(src, dst, 1);

  // Source unchanged
  CHECK(src[0] == 100);
  CHECK(src[1] == 150);
  CHECK(src[2] == 200);
  CHECK(src[3] == 255);

  // Destination has RGBA
  CHECK(dst[0] == 200); // R
  CHECK(dst[1] == 150); // G
  CHECK(dst[2] == 100); // B
  CHECK(dst[3] == 255); // A
}

TEST_CASE("Edge values", "[unit][color]") {
  // All zeros
  uint8_t black[] = {0, 0, 0, 0};
  bgra_to_rgba(black, 1);
  CHECK(black[0] == 0);
  CHECK(black[1] == 0);
  CHECK(black[2] == 0);
  CHECK(black[3] == 0);

  // All 255
  uint8_t white[] = {255, 255, 255, 255};
  bgra_to_rgba(white, 1);
  CHECK(white[0] == 255);
  CHECK(white[1] == 255);
  CHECK(white[2] == 255);
  CHECK(white[3] == 255);
}

TEST_CASE("Odd buffer sizes", "[unit][color]") {
  // 1x1 image
  uint8_t one[] = {10, 20, 30, 40};
  bgra_to_rgba(one, 1);
  CHECK(one[0] == 30);
  CHECK(one[2] == 10);

  // 3x3 image (9 pixels)
  std::vector<uint8_t> buf(9 * 4);
  for (size_t i = 0; i < 9; ++i) {
    buf[i * 4 + 0] = static_cast<uint8_t>(i);      // B
    buf[i * 4 + 1] = static_cast<uint8_t>(i + 10);  // G
    buf[i * 4 + 2] = static_cast<uint8_t>(i + 20);  // R
    buf[i * 4 + 3] = 255;                            // A
  }

  bgra_to_rgba(buf.data(), 9);

  for (size_t i = 0; i < 9; ++i) {
    CHECK(buf[i * 4 + 0] == static_cast<uint8_t>(i + 20)); // R (was B pos)
    CHECK(buf[i * 4 + 1] == static_cast<uint8_t>(i + 10)); // G unchanged
    CHECK(buf[i * 4 + 2] == static_cast<uint8_t>(i));       // B (was R pos)
    CHECK(buf[i * 4 + 3] == 255);                            // A unchanged
  }
}

TEST_CASE("Zero pixel count", "[unit][color]") {
  uint8_t buf[] = {1, 2, 3, 4};
  bgra_to_rgba(buf, 0); // Should be a no-op
  CHECK(buf[0] == 1);
  CHECK(buf[1] == 2);
  CHECK(buf[2] == 3);
  CHECK(buf[3] == 4);
}

TEST_CASE("Multi-pixel src-dst", "[unit][color]") {
  uint8_t src[] = {
      10, 20, 30, 40, // pixel 0: BGRA
      50, 60, 70, 80, // pixel 1: BGRA
  };
  uint8_t dst[8] = {};

  bgra_to_rgba(src, dst, 2);

  CHECK(dst[0] == 30); // pixel 0: R
  CHECK(dst[1] == 20); // pixel 0: G
  CHECK(dst[2] == 10); // pixel 0: B
  CHECK(dst[3] == 40); // pixel 0: A
  CHECK(dst[4] == 70); // pixel 1: R
  CHECK(dst[5] == 60); // pixel 1: G
  CHECK(dst[6] == 50); // pixel 1: B
  CHECK(dst[7] == 80); // pixel 1: A
}
