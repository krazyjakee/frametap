#include <catch2/catch_test_macros.hpp>
#include <frametap/frametap.h>

#include "helpers.h"

using namespace frametap;

// --- Section 3.2: Screenshots ---

TEST_CASE("Full screen screenshot", "[integration][screenshot]") {
  if (!test_helpers::has_display()) {
    SKIP("No display server available");
  }

  FrameTap ft;
  ImageData img = ft.screenshot();

  CHECK_FALSE(img.data.empty());
  CHECK(img.width > 0);
  CHECK(img.height > 0);
}

TEST_CASE("Screenshot dimensions", "[integration][screenshot]") {
  if (!test_helpers::has_display()) {
    SKIP("No display server available");
  }

  FrameTap ft;
  ImageData img = ft.screenshot();

  // Dimensions should be reasonable (at least 1x1)
  CHECK(img.width >= 1);
  CHECK(img.height >= 1);
}

TEST_CASE("Screenshot pixel format", "[integration][screenshot]") {
  if (!test_helpers::has_display()) {
    SKIP("No display server available");
  }

  FrameTap ft;
  ImageData img = ft.screenshot();

  // RGBA: 4 bytes per pixel
  CHECK(img.data.size() == img.width * img.height * 4);
}

TEST_CASE("Region screenshot", "[integration][screenshot]") {
  if (!test_helpers::has_display()) {
    SKIP("No display server available");
  }

  Rect region{0, 0, 100, 100};
  FrameTap ft(region);
  ImageData img = ft.screenshot();

  CHECK_FALSE(img.data.empty());
  CHECK(img.width > 0);
  CHECK(img.height > 0);
}

TEST_CASE("Non-zero pixels", "[integration][screenshot]") {
  if (!test_helpers::has_display()) {
    SKIP("No display server available");
  }

  FrameTap ft;
  ImageData img = ft.screenshot();
  REQUIRE_FALSE(img.data.empty());

  // At least some pixels should be non-zero (not a blank image)
  bool has_nonzero = false;
  for (uint8_t byte : img.data) {
    if (byte != 0) {
      has_nonzero = true;
      break;
    }
  }
  CHECK(has_nonzero);
}

TEST_CASE("Monitor screenshot", "[integration][screenshot]") {
  if (!test_helpers::has_display()) {
    SKIP("No display server available");
  }

  auto monitors = get_monitors();
  REQUIRE_FALSE(monitors.empty());

  FrameTap ft(monitors[0]);
  ImageData img = ft.screenshot();

  CHECK_FALSE(img.data.empty());
  CHECK(img.width > 0);
  CHECK(img.height > 0);
}

TEST_CASE("Window screenshot", "[integration][screenshot]") {
  if (!test_helpers::has_display()) {
    SKIP("No display server available");
  }

  auto windows = get_windows();
  if (windows.empty()) {
    SKIP("No windows available for capture");
  }

  FrameTap ft(windows[0]);
  ImageData img = ft.screenshot();

  CHECK_FALSE(img.data.empty());
  CHECK(img.width > 0);
  CHECK(img.height > 0);
}

TEST_CASE("Screenshot with explicit region", "[integration][screenshot]") {
  if (!test_helpers::has_display()) {
    SKIP("No display server available");
  }

  FrameTap ft;
  Rect region{0, 0, 100, 100};
  ImageData img = ft.screenshot(region);

  CHECK_FALSE(img.data.empty());
  CHECK(img.width > 0);
  CHECK(img.height > 0);
}
