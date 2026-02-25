#pragma once

#include <frametap/types.h>

#include <functional>
#include <memory>
#include <stdexcept>
#include <vector>

namespace frametap {

using FrameCallback = std::function<void(const Frame &)>;

// --- Query functions ---
std::vector<Monitor> get_monitors();
std::vector<Window> get_windows();

// --- Permission diagnostics ---
// Returns platform-specific information about capture readiness:
// whether required permissions are granted, dependencies are available, etc.
PermissionCheck check_permissions();

class CaptureError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

class FrameTap {
public:
  FrameTap();
  explicit FrameTap(Rect region);
  explicit FrameTap(Monitor monitor);
  explicit FrameTap(Window window);
  ~FrameTap();

  FrameTap(FrameTap &&) noexcept;
  FrameTap &operator=(FrameTap &&) noexcept;

  FrameTap(const FrameTap &) = delete;
  FrameTap &operator=(const FrameTap &) = delete;

  void set_region(Rect region);
  void on_frame(FrameCallback callback);

  void start();
  void start_async();
  void stop();

  void pause();
  void resume();
  bool is_paused() const;

  ImageData screenshot();
  ImageData screenshot(Rect region);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace frametap
