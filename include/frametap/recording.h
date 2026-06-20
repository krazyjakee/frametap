#pragma once

#include <frametap/types.h>

#include <cstdint>
#include <memory>
#include <string>

// Hardware video recording (Linux / NVIDIA NVENC).
//
// This is the first slice of frametap's video pipeline: it consumes the RGBA
// frames delivered by FrameTap::on_frame, encodes them with NVENC (H.264 or
// HEVC) on the GPU, and writes a directly-playable .mp4 file. An adaptive
// controller nudges the encoder bitrate at runtime so a chosen target --
// frames-per-second or visual quality -- is held under load.
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

// Network streaming transport. The same encoded H.264/HEVC + AAC the recorder
// would write to disk can instead (or also) be pushed over the network.
//
// All three carry a hand-rolled MPEG-TS (srt/udp) or FLV (rtmp) stream; no
// ffmpeg/libavformat dependency. SRT is the recommended choice for sending to a
// specific person: in listener mode this machine serves and their player
// connects, and it stays watchable over the lossy public internet.
enum class StreamProtocol {
  srt,    // SRT (loss-tolerant, internet-friendly). Requires libsrt at build
          // time (FRAMETAP_HAVE_SRT). Two modes, chosen by the url:
          //   listener (you serve): srt://0.0.0.0:9000?mode=listener
          //     -> the viewer connects to srt://your-ip:9000
          //   caller   (you push):  srt://their-host:9000
  udp_ts, // Push a raw MPEG-TS over UDP (simplest, LAN only).
          //   url e.g. udp://their-host:1234   (receiver: udp://@:1234)
  rtmp,   // Push FLV to an RTMP ingest (Twitch/YouTube/nginx-rtmp relay).
          //   url e.g. rtmp://host/app/key
};

struct StreamConfig {
  bool enabled = false;
  StreamProtocol protocol = StreamProtocol::srt;
  std::string url;
  bool also_save_file = true; // false = stream only, write no local file.
};

// Default directory for saved recordings, created if it doesn't exist:
//   Linux:   $XDG_VIDEOS_DIR/Screencasts  (else ~/Videos/Screencasts)
//   macOS:   ~/Movies/Screencasts
//   Windows: %USERPROFILE%\Videos\Screencasts
// Falls back to $HOME, then the current directory, if those can't be resolved.
std::string default_recording_dir();

// A timestamped output path inside default_recording_dir() for the codec,
// e.g. ".../frametap-20260620-143052.h264".
std::string default_recording_path(Codec codec);

struct EncoderConfig {
  Codec codec = Codec::h264;
  int fps = 60;             // Target frame rate (also drives GOP / VBV sizing).
  int bitrate_kbps = 20000; // Starting average bitrate (20 Mbps).
  int min_bitrate_kbps = 4000;  // Adaptation floor.
  int max_bitrate_kbps = 60000; // Adaptation ceiling.
  AdaptPriority priority = AdaptPriority::fps;

  // Audio source. 0 captures all system audio (the default sink monitor); a
  // non-zero process id captures only that application's sound (and any
  // process in its tree). Use Window::pid when recording a single window.
  uint64_t audio_source_pid = 0;

  // Optional network streaming. When stream.enabled is set, encoded frames are
  // pushed to stream.url over stream.protocol. Streaming is best-effort: a
  // transport failure is logged but does not abort the recording. If
  // stream.also_save_file is false, no local .mp4 is written.
  StreamConfig stream;
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
  // Rethrows any error raised while encoding on the capture thread (e.g. the
  // encoder rejecting the frame size), so wrap this call in try/catch.
  void finish();

  Stats stats() const;

  // Empty unless network streaming was enabled and the transport failed (e.g.
  // no SRT support, connection refused). Meaningful after finish(). Streaming
  // failures are non-fatal: the local file, if any, is still written.
  std::string stream_error() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace frametap
