#pragma once

#include <frametap/types.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

// Network stream receiving (Linux / NVIDIA NVDEC), the counterpart to the
// recording.h transmit path. A StreamReceiver pulls a hand-rolled MPEG-TS
// stream over SRT, demuxes it, and -- when decoding is enabled -- turns the
// H.264/HEVC video back into RGBA frames on the GPU via NVDEC for live preview.
// It can also write the received TS straight to a directly-playable file.
//
// No ffmpeg/libav dependency: the TS demuxer is hand-rolled and NVDEC/CUDA are
// dlopen'd at runtime. Compiled into the CLI (--receive) and GUI when the
// NVENC/NVDEC headers are present; the core library is unchanged.

namespace frametap {

struct ReceiverConfig {
  // SRT URL. Listener (you serve, the sender connects to you):
  //   srt://0.0.0.0:9000?mode=listener
  // Caller (you pull from a listening sender):
  //   srt://their-host:9000
  std::string url;

  // Decode the video to RGBA frames delivered via on_frame(). Requires NVDEC.
  bool decode = true;

  // If non-empty, the raw received MPEG-TS is written here as well. The file is
  // a faithful copy of the stream (no transcode) and plays in mpv/VLC/ffplay.
  std::string out_path;
};

// Receives and (optionally) decodes a stream on a background thread. The frame
// callback is invoked from that worker thread, so consumers must hand frames
// off in a thread-safe way (e.g. a queue).
class StreamReceiver {
public:
  using FrameCallback = std::function<void(const ImageData &)>;

  struct Stats {
    uint64_t bytes_received = 0;
    uint64_t frames_decoded = 0;
  };

  // Throws CaptureError if the config is unusable (e.g. a non-SRT URL, or
  // decoding requested in a build without NVDEC support).
  explicit StreamReceiver(ReceiverConfig config);
  ~StreamReceiver();

  StreamReceiver(const StreamReceiver &) = delete;
  StreamReceiver &operator=(const StreamReceiver &) = delete;

  // Register the decoded-frame sink. Call before start().
  void on_frame(FrameCallback cb);

  // Spawn the worker thread and return immediately. In listener mode the worker
  // blocks waiting for a peer; watch connected() for progress.
  void start();

  // Stop the worker and join. Safe to call multiple times.
  void stop();

  bool running() const;
  bool connected() const;     // a peer is connected and bytes can flow
  std::string error() const;  // non-empty if the worker failed
  Stats stats() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace frametap
