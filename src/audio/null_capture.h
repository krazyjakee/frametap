#pragma once

#include <cstdint>
#include <functional>
#include <stdexcept>

// Placeholder system-audio capture for platforms without a native backend yet
// (currently Windows). start() always throws, so VideoRecorder falls back to
// its video-only path -- the recording / stream proceeds without an audio
// track. A real WASAPI loopback capture is the follow-up that replaces this.

namespace frametap::audio {

class NullCapture {
public:
  using PcmSink =
      std::function<void(const float *interleaved, uint32_t frames)>;

  NullCapture() = default;
  ~NullCapture() = default;
  NullCapture(const NullCapture &) = delete;
  NullCapture &operator=(const NullCapture &) = delete;

  void start(PcmSink, uint64_t = 0) {
    throw std::runtime_error("audio capture not implemented on this platform");
  }
  void stop() {}

  bool running() const { return false; }
  int rate() const { return rate_; }
  int channels() const { return channels_; }

private:
  int rate_ = 48000;
  int channels_ = 2;
};

} // namespace frametap::audio
