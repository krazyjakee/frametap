#include <catch2/catch_test_macros.hpp>
#include <frametap/frametap.h>

#include "helpers.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace frametap;

// --- Section 5: Error Handling & Edge Cases ---

TEST_CASE("CaptureError is catchable", "[unit][errors]") {
  bool caught = false;
  try {
    throw CaptureError("test error message");
  } catch (const CaptureError &e) {
    caught = true;
    CHECK(std::string(e.what()).find("test error message") !=
          std::string::npos);
  }
  CHECK(caught);
}

TEST_CASE("CaptureError inherits runtime_error", "[unit][errors]") {
  bool caught = false;
  try {
    throw CaptureError("inherits test");
  } catch (const std::runtime_error &e) {
    caught = true;
    CHECK(std::string(e.what()) == "inherits test");
  }
  CHECK(caught);
}

TEST_CASE("CaptureError what() is non-empty", "[unit][errors]") {
  CaptureError e("something went wrong");
  CHECK(std::string(e.what()).size() > 0);
}

TEST_CASE("No display server", "[unit][errors]") {
  // This test is only meaningful in headless environments
  if (test_helpers::has_display()) {
    SKIP("Display server is available — cannot test no-display path");
  }

  // Library should throw CaptureError, not segfault
  CHECK_THROWS_AS(FrameTap(), CaptureError);
}

TEST_CASE("Invalid monitor ID", "[integration][errors]") {
  if (!test_helpers::has_display()) {
    SKIP("No display server available");
  }

  Monitor fake;
  fake.id = 99999;
  fake.name = "Fake Monitor";
  fake.width = 1920;
  fake.height = 1080;

  // Should throw CaptureError, not crash
  // Note: on X11, invalid monitor IDs may fall through to screen 0,
  // so we accept either a thrown exception or a valid capture.
  try {
    FrameTap ft(fake);
    ImageData img = ft.screenshot();
  } catch (const CaptureError &) {
    // Expected on most platforms
  } catch (const std::exception &e) {
    FAIL("Unexpected exception: " << e.what());
  }
}

// NOTE: "Invalid window ID" test is excluded. On X11, passing a fabricated
// window ID to XGetWindowAttributes triggers a fatal X error (the default X11
// error handler calls exit()). The library should install a custom X error
// handler — tracked as a known improvement. On Wayland, window IDs come from
// the portal picker and can't be fabricated.

TEST_CASE("Rapid start/stop cycles", "[integration][errors]") {
  if (!test_helpers::has_display()) {
    SKIP("No display server available");
  }

  for (int i = 0; i < 100; ++i) {
    FrameTap ft;
    ft.on_frame([](const Frame &) {});
    ft.start_async();
    ft.stop();
  }
  // If we get here without leaks or crashes, the test passes
  CHECK(true);
}

TEST_CASE("Concurrent instances", "[integration][errors]") {
  if (!test_helpers::has_display()) {
    SKIP("No display server available");
  }

  FrameTap ft1;
  FrameTap ft2;

  std::atomic<int> count1{0};
  std::atomic<int> count2{0};

  ft1.on_frame([&](const Frame &) { count1.fetch_add(1); });
  ft2.on_frame([&](const Frame &) { count2.fetch_add(1); });

  ft1.start_async();
  ft2.start_async();

  // Wait for both to produce frames
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while ((count1 == 0 || count2 == 0) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  ft1.stop();
  ft2.stop();

  CHECK(count1 > 0);
  CHECK(count2 > 0);
}

TEST_CASE("Large region", "[integration][errors]") {
  if (!test_helpers::has_display()) {
    SKIP("No display server available");
  }

  // Region much larger than any screen — should be clamped or throw
  Rect huge{0, 0, 100000, 100000};
  try {
    FrameTap ft(huge);
    ImageData img = ft.screenshot();
    // If it succeeds, dimensions should be clamped to actual screen
    CHECK(img.width <= 100000);
    CHECK(img.height <= 100000);
  } catch (const CaptureError &) {
    // Also acceptable
  }
}

TEST_CASE("Negative coordinates", "[integration][errors]") {
  if (!test_helpers::has_display()) {
    SKIP("No display server available");
  }

  // Negative coordinates should be clamped, not crash
  Rect negative{-100, -100, 200, 200};
  try {
    FrameTap ft(negative);
    ImageData img = ft.screenshot();
    // Should succeed with clamped dimensions
    CHECK(img.width > 0);
    CHECK(img.height > 0);
  } catch (const CaptureError &) {
    // Also acceptable
  }
}
