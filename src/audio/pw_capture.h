#pragma once

#include <cstdint>
#include <functional>

// System / per-application audio capture via PipeWire.
//
// With target_pid == 0 it connects an Audio/Capture stream to the monitor of
// the default sink (PW_KEY_STREAM_CAPTURE_SINK) and records everything the
// machine plays. With a non-zero target_pid it instead creates an unconnected
// capture node and links it, port by port, to the output of the application
// owning that PID (or any process in its tree) -- so only that app's sound is
// recorded, without disturbing playback. PCM is delivered as interleaved
// 32-bit float on PipeWire's own thread.

struct pw_thread_loop;
struct pw_stream;
struct pw_context;
struct pw_core;

namespace frametap::audio {

class PwCapture {
public:
  using PcmSink =
      std::function<void(const float *interleaved, uint32_t frames)>;

  PwCapture() = default;
  ~PwCapture();
  PwCapture(const PwCapture &) = delete;
  PwCapture &operator=(const PwCapture &) = delete;

  // Starts capture. Throws std::runtime_error on failure. `sink` is invoked
  // from the PipeWire thread. target_pid == 0 captures all system audio.
  void start(PcmSink sink, uint64_t target_pid = 0);
  void stop();

  bool running() const { return loop_ != nullptr; }
  int rate() const { return rate_; }
  int channels() const { return channels_; }

  // Called from PipeWire trampolines.
  void on_process();
  void on_param_changed(uint32_t id, const void *param);
  void on_registry_global(uint32_t id, const char *type, const void *props);
  void on_node_info(uint32_t node_id, const void *props);
  void try_link();

private:
  struct AppState; // per-app registry/link bookkeeping (defined in .cpp)

  pw_thread_loop *loop_ = nullptr;
  pw_context *context_ = nullptr;
  pw_core *core_ = nullptr;
  pw_stream *stream_ = nullptr;
  void *listener_ = nullptr; // spa_hook, owned
  AppState *app_ = nullptr;  // non-null only in per-app mode

  PcmSink sink_;
  uint64_t target_pid_ = 0;
  int rate_ = 48000;
  int channels_ = 2;
};

} // namespace frametap::audio
