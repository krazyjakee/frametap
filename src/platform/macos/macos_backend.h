#pragma once

#include "../../backend.h"

namespace frametap::internal {

// Factory implementations for macOS
std::unique_ptr<Backend> make_macos_backend();
std::unique_ptr<Backend> make_macos_backend(Rect region);
std::unique_ptr<Backend> make_macos_backend(Monitor monitor);
std::unique_ptr<Backend> make_macos_backend(Window window);

} // namespace frametap::internal
