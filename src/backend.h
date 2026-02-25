#pragma once

#include <frametap/frametap.h>

#include <memory>
#include <vector>

namespace frametap::internal {

class Backend {
public:
  virtual ~Backend() = default;

  // One-shot capture
  virtual ImageData screenshot(Rect region) = 0;

  // Streaming
  virtual void start(FrameCallback cb) = 0;
  virtual void stop() = 0;
  virtual void pause() = 0;
  virtual void resume() = 0;
  virtual bool is_paused() const = 0;

  // Configuration
  virtual void set_region(Rect region) = 0;
};

// Factory — returns platform-appropriate backend
std::unique_ptr<Backend> make_backend();
std::unique_ptr<Backend> make_backend(Rect region);
std::unique_ptr<Backend> make_backend(Monitor monitor);
std::unique_ptr<Backend> make_backend(Window window);

// Enumeration — platform-specific
std::vector<Monitor> enumerate_monitors();
std::vector<Window> enumerate_windows();

// Permission diagnostics — platform-specific
PermissionCheck check_platform_permissions();

} // namespace frametap::internal
