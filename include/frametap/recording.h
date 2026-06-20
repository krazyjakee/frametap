#pragma once

#include <frametap/types.h>

#include <cstdint>
#include <memory>
#include <string>

// Hardware video recording (Linux / NVIDIA NVENC).
//
// This is the first slice of frametap's video pipeline: it consumes the RGBA
// frames delivered by FrameTap::on_frame, encodes them with NVENC (H.264 or
// HEVC) on the GPU, and writes an Annex-B elementary stream to disk. An
// adaptive controller nudges the encoder bitrate at runtime so a chosen
// target -- frames-per-second or visual quality -- is held under load.
//
// Only compiled into the `scons record` target; the core library is unchanged.

namespace frametap {

enum class Codec {
  h264, // Widest compatibility, lowest decode latency.
  hevc, // ~30% better quality/bit at 4K, heavier decode.
};

// What the adaptive controller protects when the encoder falls behind.
enum class AdaptPriority {
  fps,     // Hold target fps; sacrifice bitrate first.
  quality, // Hold bitrate; let fps dip.
  none,    // No runtime adaptation (fixed bitrate).
};

struct EncoderConfig {
  Codec codec = Codec::h264;
  int fps = 60;             // Target frame rate (also drives GOP / VBV sizing).
  int bitrate_kbps = 20000; // Starting average bitrate (20 Mbps).
  int min_bitrate_kbps = 4000;  // Adaptation floor.
  int max_bitrate_kbps = 60000; // Adaptation ceiling.
  AdaptPriority priority = AdaptPriority::fps;
};

// Feed RGBA frames in, get an encoded file out. Not thread-safe: submit() must
// be called from a single thread (e.g. the FrameTap capture callback).
class VideoRecorder {
public:
  struct Stats {
    uint64_t frames_in = 0;     // Frames handed to the recorder.
    uint64_t frames_encoded = 0; // Frames the encoder emitted packets for.
    uint64_t bytes_written = 0;
    double avg_encode_ms = 0;   // Mean GPU encode wall time per frame.
    double last_encode_ms = 0;
    int adaptations = 0;        // Times the controller changed bitrate.
    int current_bitrate_kbps = 0;
  };

  // Opens `out_path` for writing. The encoder is created lazily on the first
  // submit() once frame dimensions are known. Throws CaptureError on failure.
  VideoRecorder(std::string out_path, EncoderConfig config);
  ~VideoRecorder();

  VideoRecorder(VideoRecorder &&) noexcept;
  VideoRecorder &operator=(VideoRecorder &&) noexcept;
  VideoRecorder(const VideoRecorder &) = delete;
  VideoRecorder &operator=(const VideoRecorder &) = delete;

  void submit(const Frame &frame);

  // Flush the encoder, close the file. Call once from the same context that
  // owns teardown (after FrameTap::stop()). Safe to call multiple times.
  void finish();

  Stats stats() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace frametap
