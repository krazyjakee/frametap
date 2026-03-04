#pragma once

#include "../../backend.h"

#include <atomic>
#include <mutex>
#include <thread>

namespace frametap::internal {

class AndroidBackend : public Backend {
public:
  AndroidBackend();
  explicit AndroidBackend(Rect region);
  explicit AndroidBackend(frametap::Monitor monitor);
  explicit AndroidBackend(frametap::Window window);
  ~AndroidBackend() override;

  ImageData screenshot(Rect region) override;
  void start(FrameCallback cb) override;
  void stop() override;
  void pause() override;
  void resume() override;
  bool is_paused() const override;
  void set_region(Rect region) override;

private:
  ImageData capture_full_screen();
  ImageData capture_via_screencap();
  ImageData capture_via_projection();
  bool ensure_projection_active();
  ImageData crop(const ImageData &full, Rect region);
  void capture_loop();

  Rect region_{};

  FrameCallback callback_;
  std::thread capture_thread_;
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> paused_{false};

  // Protects region_ from concurrent access between the capture thread
  // and set_region() calls.
  mutable std::mutex state_mutex_;
};

} // namespace frametap::internal
