#pragma once

#include "../../../backend.h"

#include <X11/Xlib.h>
#include <X11/extensions/XShm.h>
#include <sys/shm.h>

#include <atomic>
#include <mutex>
#include <thread>

namespace frametap::internal {

class X11Backend : public Backend {
public:
  X11Backend();
  explicit X11Backend(Rect region);
  explicit X11Backend(frametap::Monitor monitor);
  explicit X11Backend(frametap::Window window);
  ~X11Backend() override;

  ImageData screenshot(Rect region) override;
  void start(FrameCallback cb) override;
  void stop() override;
  void pause() override;
  void resume() override;
  bool is_paused() const override;
  void set_region(Rect region) override;

private:
  void open_display();
  void compute_capture_area();
  void init_shm(int width, int height);
  void cleanup_shm();
  ImageData capture_frame();
  void capture_loop(std::stop_token token);

  Display *display_ = nullptr;
  int screen_ = 0;
  ::Window root_ = 0;
  ::Window target_ = 0;
  bool capture_window_ = false;

  Rect region_{};
  int cap_x_ = 0, cap_y_ = 0;
  int cap_w_ = 0, cap_h_ = 0;

  XShmSegmentInfo shm_info_{};
  XImage *shm_image_ = nullptr;
  bool shm_attached_ = false;
  bool use_shm_ = false;

  FrameCallback callback_;
  std::jthread capture_thread_;
  std::atomic<bool> paused_{false};

  // Protects region_, cap_x/y/w/h_, and SHM state from concurrent access
  // between the capture thread and set_region() calls.
  std::mutex state_mutex_;
};

// Enumeration helpers
std::vector<frametap::Monitor> x11_enumerate_monitors();
std::vector<frametap::Window> x11_enumerate_windows();

// Standalone screenshot helper (opens its own display connection)
ImageData x11_take_screenshot(::Window target, Rect region,
                              bool capture_window);

} // namespace frametap::internal
