#pragma once

#include "../../backend.h"

namespace frametap::internal {

// Factory implementations for Linux — dispatches to X11 or Wayland at runtime
// based on $WAYLAND_DISPLAY / $DISPLAY environment variables.
std::unique_ptr<Backend> make_linux_backend();
std::unique_ptr<Backend> make_linux_backend(Rect region);
std::unique_ptr<Backend> make_linux_backend(Monitor monitor);
std::unique_ptr<Backend> make_linux_backend(Window window);

} // namespace frametap::internal
