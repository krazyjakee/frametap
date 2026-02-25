#pragma once

#include <cstdlib>

namespace test_helpers {

// Returns true if a display server is available (X11 or Wayland).
// Used to SKIP integration tests in headless CI environments.
inline bool has_display() {
#if defined(__APPLE__) || defined(_WIN32)
  return true; // macOS and Windows always have a display in user sessions
#else
  return std::getenv("DISPLAY") != nullptr ||
         std::getenv("WAYLAND_DISPLAY") != nullptr;
#endif
}

} // namespace test_helpers
