#pragma once

#include "../../../backend.h"
#include "wl_portal.h"

#include <atomic>
#include <mutex>
#include <thread>

// Forward declarations for PipeWire/SPA types (global namespace)
struct pw_main_loop;
struct pw_context;
struct pw_core;
struct pw_stream;
struct spa_hook;
struct spa_pod;

namespace frametap::internal {

// H3: Per-instance stream callback data (not thread_local)
struct StreamData {
  class WaylandBackend *backend = nullptr;
};

class WaylandBackend : public Backend {
public:
  WaylandBackend();
  explicit WaylandBackend(Rect region);
  explicit WaylandBackend(frametap::Monitor monitor);
  explicit WaylandBackend(frametap::Window window);
  ~WaylandBackend() override;

  ImageData screenshot(Rect region) override;
  void start(FrameCallback cb) override;
  void stop() override;
  void pause() override;
  void resume() override;
  bool is_paused() const override;
  void set_region(Rect region) override;

  // Called by PipeWire callbacks (internal)
  void on_process();
  void on_param_changed(uint32_t id, const ::spa_pod *param);

private:
  void setup_pipewire();
  void teardown_pipewire();

  Rect region_{};
  bool capture_window_ = false;

  // Portal session
  PortalSession portal_{};
  bool portal_open_ = false;

  // PipeWire state
  pw_main_loop *pw_loop_ = nullptr;
  pw_context *pw_context_ = nullptr;
  pw_core *pw_core_ = nullptr;
  pw_stream *pw_stream_ = nullptr;
  // L2: Owned by this class, freed in teardown_pipewire()
  spa_hook *stream_listener_ = nullptr;

  // H3: Per-instance stream data (replaces thread_local global)
  StreamData stream_data_;

  // Negotiated format
  uint32_t video_format_ = 0;
  int32_t frame_width_ = 0;
  int32_t frame_height_ = 0;

  // Streaming state
  FrameCallback callback_;
  std::jthread pw_thread_;
  std::atomic<bool> paused_{false};

  // H2/M6: Mutex protects region_ and last_frame_time_
  mutable std::mutex state_mutex_;
  std::chrono::steady_clock::time_point last_frame_time_;
};

// Enumeration
std::vector<frametap::Monitor> wl_enumerate_monitors();
std::vector<frametap::Window> wl_enumerate_windows();

} // namespace frametap::internal
