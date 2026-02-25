#pragma once

#include "../../backend.h"

namespace frametap::internal {

// Factory implementations for Windows
std::unique_ptr<Backend> make_windows_backend();
std::unique_ptr<Backend> make_windows_backend(Rect region);
std::unique_ptr<Backend> make_windows_backend(Monitor monitor);
std::unique_ptr<Backend> make_windows_backend(Window window);

} // namespace frametap::internal
