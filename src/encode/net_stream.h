#pragma once

#include "encode/stream_sink.h"

#include <frametap/recording.h> // StreamProtocol

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Network sink for the encoded stream.
//
// Takes the same Annex-B H.264/HEVC access units the Mp4Muxer receives, plus
// the AAC frames from AacEncoder, and muxes them onto a live transport instead
// of (or alongside) a file:
//
//   srt / udp_ts -> hand-rolled MPEG-TS pushed over SRT or UDP
//   rtmp         -> hand-rolled FLV pushed to an RTMP ingest
//
// The actual muxing + network I/O lives behind a StreamSink (TsSink / RtmpSink);
// this class only owns the worker thread and a bounded queue so a slow or absent
// receiver never stalls the capture/encode thread. The producer drops to the
// latest keyframe under backpressure. Streaming is best-effort: any failure is
// recorded in last_error() and turns subsequent writes into no-ops; it never
// throws back into the encode path.

namespace frametap::enc {

class NetworkStreamer {
public:
  NetworkStreamer();
  ~NetworkStreamer();
  NetworkStreamer(const NetworkStreamer &) = delete;
  NetworkStreamer &operator=(const NetworkStreamer &) = delete;

  // Start the worker. The transport is opened lazily on the worker once codec
  // parameters (extradata) and the audio decision are known.
  void open(StreamProtocol proto, const std::string &url, bool hevc, int width,
            int height, int fps);

  // Feed one Annex-B access unit (start-coded NALs). `pts_90k` is the frame's
  // presentation time on a 90 kHz clock, relative to the first frame.
  void write_access_unit(const uint8_t *data, size_t size, bool keyframe,
                         uint64_t pts_90k);

  // Declare an AAC audio track. `asc` is the AudioSpecificConfig. Must be
  // called before write_audio_sample(). `start_delay_90k` offsets audio PTS so
  // it shares the video's epoch (the first frame), matching when capture began.
  // If audio capture is unavailable, call no_audio() instead so the header can
  // be written video-only.
  void set_audio(int sample_rate, int channels, const uint8_t *asc,
                 size_t asc_len, uint64_t start_delay_90k = 0);
  void write_audio_sample(const uint8_t *data, size_t size);
  void no_audio();

  // Flush and stop the worker. Safe to call multiple times.
  void close();

  std::string last_error() const;

private:
  struct Packet {
    bool audio = false;
    bool keyframe = false;
    uint64_t pts = 0; // 90 kHz for both video and audio
    std::vector<uint8_t> data;
  };

  void run();
  void extract_extradata(const uint8_t *data, size_t size);
  void set_error(const std::string &msg);

  // Config (immutable after open()).
  StreamProtocol proto_ = StreamProtocol::srt;
  std::string url_;
  bool hevc_ = false;
  int width_ = 0;
  int height_ = 0;
  int fps_ = 60;

  std::thread worker_;
  mutable std::mutex m_;
  std::condition_variable cv_;
  std::deque<Packet> q_;

  // Pre-header state, guarded by m_.
  std::vector<uint8_t> extradata_; // Annex-B SPS/PPS(/VPS)
  bool have_extradata_ = false;
  std::vector<uint8_t> asc_;
  int audio_rate_ = 48000;
  int audio_channels_ = 2;
  bool has_audio_ = false;
  bool audio_decided_ = false;
  uint64_t video_seen_ = 0;     // frames pushed before audio decision
  uint64_t audio_samples_ = 0;  // running audio pts in samples
  uint64_t audio_start_delay_90k_ = 0; // audio epoch offset vs first frame

  bool started_ = false;
  bool streaming_ = false; // header written, draining queue
  bool failed_ = false;
  bool eos_ = false;
  std::string error_;

  // Set by close() to break a blocking SRT listener accept on the worker.
  std::atomic<bool> cancel_{false};

  std::unique_ptr<StreamSink> sink_; // worker-only after creation
};

} // namespace frametap::enc
