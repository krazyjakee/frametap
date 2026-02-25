#include <catch2/catch_test_macros.hpp>
#include <frametap/frametap.h>

#include "helpers.h"

using namespace frametap;

// --- Section 3.1: Monitor & window enumeration ---

TEST_CASE("get_monitors non-empty", "[integration][enumerate]") {
  if (!test_helpers::has_display()) {
    SKIP("No display server available");
  }

  auto monitors = get_monitors();
  REQUIRE_FALSE(monitors.empty());
}

TEST_CASE("Monitor fields valid", "[integration][enumerate]") {
  if (!test_helpers::has_display()) {
    SKIP("No display server available");
  }

  auto monitors = get_monitors();
  REQUIRE_FALSE(monitors.empty());

  for (const auto &m : monitors) {
    CHECK(m.width > 0);
    CHECK(m.height > 0);
    CHECK(m.scale >= 1.0f);
  }
}

TEST_CASE("Monitor names non-empty", "[integration][enumerate]") {
  if (!test_helpers::has_display()) {
    SKIP("No display server available");
  }

  auto monitors = get_monitors();
  REQUIRE_FALSE(monitors.empty());

  for (const auto &m : monitors) {
    CHECK_FALSE(m.name.empty());
  }
}

TEST_CASE("get_windows returns list", "[integration][enumerate]") {
  if (!test_helpers::has_display()) {
    SKIP("No display server available");
  }

  // Should not throw, may return empty list
  std::vector<Window> windows;
  CHECK_NOTHROW(windows = get_windows());
}

TEST_CASE("Window fields valid", "[integration][enumerate]") {
  if (!test_helpers::has_display()) {
    SKIP("No display server available");
  }

  auto windows = get_windows();
  // Windows list may be empty on minimal environments, so just
  // validate fields for any windows that are present.
  for (const auto &w : windows) {
    CHECK(w.id != 0);
    CHECK(w.width > 0);
    CHECK(w.height > 0);
  }
}
