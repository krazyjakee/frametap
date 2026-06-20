#pragma once

#include <cstdint>
#include <functional>
#include <memory>

// System audio capture via ScreenCaptureKit (macOS 13+).
//
// The macOS counterpart to PwCapture: it captures everything the machine plays
// (the system mix, minus this process's own output) and delivers it as
// interleaved 32-bit float PCM on ScreenCaptureKit's audio queue -- the same
// shape recorder.cpp feeds into the AAC encoder.
//
// ScreenCaptureKit cannot isolate a single application's audio on macOS 13, so
// target_pid is accepted for interface parity but ignored: capture is always the
// full system mix. Requires the same Screen Recording permission as video
// capture; start() throws if unavailable (recorder then records video-only).

namespace frametap::audio {

class CaCapture {
public:
  using PcmSink =
      std::function<void(const float *interleaved, uint32_t frames)>;

  CaCapture();
  ~CaCapture();
  CaCapture(const CaCapture &) = delete;
  CaCapture &operator=(const CaCapture &) = delete;

  // Starts capture. Throws std::runtime_error on failure. `sink` is invoked
  // from ScreenCaptureKit's audio queue. target_pid is ignored (see above).
  void start(PcmSink sink, uint64_t target_pid = 0);
  void stop();

  bool running() const;
  int rate() const { return rate_; }
  int channels() const { return channels_; }

  // Called from the Objective-C output delegate with a CMSampleBufferRef (typed
  // as void* to keep CoreMedia out of this C++ header). Interleaves the PCM and
  // forwards it to the sink.
  void handle_sample(void *cm_sample_buffer);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  PcmSink sink_;
  int rate_ = 48000;
  int channels_ = 2;
};

} // namespace frametap::audio
