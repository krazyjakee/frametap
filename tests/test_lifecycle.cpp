#include <catch2/catch_test_macros.hpp>
#include <frametap/frametap.h>

#include "helpers.h"

#include <type_traits>

using namespace frametap;

// --- Section 2.4: FrameTap lifecycle ---

TEST_CASE("FrameTap move semantics", "[unit][lifecycle]") {
  // Move-constructible
  CHECK(std::is_move_constructible_v<FrameTap>);
  // Move-assignable
  CHECK(std::is_move_assignable_v<FrameTap>);
}

TEST_CASE("FrameTap no copy", "[unit][lifecycle]") {
  // Not copy-constructible
  CHECK_FALSE(std::is_copy_constructible_v<FrameTap>);
  // Not copy-assignable
  CHECK_FALSE(std::is_copy_assignable_v<FrameTap>);
}

TEST_CASE("CaptureError is catchable", "[unit][lifecycle]") {
  try {
    throw CaptureError("test error");
  } catch (const CaptureError &e) {
    CHECK(std::string(e.what()) == "test error");
  }
}

TEST_CASE("CaptureError inherits runtime_error", "[unit][lifecycle]") {
  try {
    throw CaptureError("test error");
  } catch (const std::runtime_error &e) {
    CHECK(std::string(e.what()) == "test error");
  }
}

TEST_CASE("Default constructor", "[integration][lifecycle]") {
  if (!test_helpers::has_display()) {
    SKIP("No display server available");
  }

  CHECK_NOTHROW(FrameTap());
}

TEST_CASE("Rect constructor", "[integration][lifecycle]") {
  if (!test_helpers::has_display()) {
    SKIP("No display server available");
  }

  CHECK_NOTHROW(FrameTap(Rect{0, 0, 100, 100}));
}

TEST_CASE("Move construction transfers ownership", "[integration][lifecycle]") {
  if (!test_helpers::has_display()) {
    SKIP("No display server available");
  }

  FrameTap a;
  FrameTap b = std::move(a);
  // b should be usable — take a screenshot to verify
  CHECK_NOTHROW(b.screenshot());
}

TEST_CASE("Move assignment transfers ownership", "[integration][lifecycle]") {
  if (!test_helpers::has_display()) {
    SKIP("No display server available");
  }

  FrameTap a;
  FrameTap b;
  b = std::move(a);
  CHECK_NOTHROW(b.screenshot());
}

TEST_CASE("Double stop", "[integration][lifecycle]") {
  if (!test_helpers::has_display()) {
    SKIP("No display server available");
  }

  FrameTap ft;
  ft.on_frame([](const Frame &) {});
  ft.start_async();
  ft.stop();
  CHECK_NOTHROW(ft.stop()); // Second stop should not crash
}

TEST_CASE("Stop without start", "[integration][lifecycle]") {
  if (!test_helpers::has_display()) {
    SKIP("No display server available");
  }

  FrameTap ft;
  CHECK_NOTHROW(ft.stop()); // Should be a no-op
}

TEST_CASE("Destructor stops", "[integration][lifecycle]") {
  if (!test_helpers::has_display()) {
    SKIP("No display server available");
  }

  // Create a streaming FrameTap and let it go out of scope
  {
    FrameTap ft;
    ft.on_frame([](const Frame &) {});
    ft.start_async();
    // Destructor should call stop() implicitly
  }
  // If we get here without hanging or crashing, the test passes
  CHECK(true);
}

TEST_CASE("Pause without start", "[integration][lifecycle]") {
  if (!test_helpers::has_display()) {
    SKIP("No display server available");
  }

  FrameTap ft;
  // pause() before start() should not crash
  CHECK_NOTHROW(ft.pause());
}

TEST_CASE("Start without callback throws", "[integration][lifecycle]") {
  if (!test_helpers::has_display()) {
    SKIP("No display server available");
  }

  FrameTap ft;
  CHECK_THROWS_AS(ft.start(), CaptureError);
}

TEST_CASE("start_async without callback throws", "[integration][lifecycle]") {
  if (!test_helpers::has_display()) {
    SKIP("No display server available");
  }

  FrameTap ft;
  CHECK_THROWS_AS(ft.start_async(), CaptureError);
}
