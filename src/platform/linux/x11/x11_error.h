#pragma once

#include <X11/Xlib.h>

#include <mutex>

namespace frametap::internal::x11_err {

// Thread-local error code set by our custom X11 error handler.
// Usage: reset to 0 before an X call, call XSync(), then check the value.
inline thread_local int g_code = 0;

inline int handler(Display * /*display*/, XErrorEvent *event) {
  g_code = event->error_code;
  return 0; // non-fatal: do not call exit()
}

inline void install() {
  static std::once_flag flag;
  std::call_once(flag, [] { XSetErrorHandler(handler); });
}

} // namespace frametap::internal::x11_err
