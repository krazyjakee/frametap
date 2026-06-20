#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// A live network muxer + transport, one implementation per protocol family:
//
//   TsSink   -> MPEG-TS over UDP or SRT   (StreamProtocol::udp_ts / ::srt)
//   RtmpSink -> FLV over RTMP             (StreamProtocol::rtmp)
//
// The NetworkStreamer owns the queue, threading and backpressure; a StreamSink
// only has to mux and push bytes. Every call happens on the streamer's single
// worker thread, so implementations need no internal locking.

namespace frametap::enc {

// Stream parameters known before the first access unit is muxed.
struct StreamSinkParams {
  std::string url;
  bool hevc = false;
  int width = 0;
  int height = 0;
  int fps = 60;

  // Annex-B parameter sets (H.264 SPS/PPS, plus VPS for HEVC), each prefixed
  // with a 4-byte start code. Captured by the streamer from the first frames.
  std::vector<uint8_t> extradata;

  // Audio is AAC-LC. has_audio == false means a video-only stream.
  bool has_audio = false;
  int audio_rate = 48000;
  int audio_channels = 2;
  std::vector<uint8_t> asc; // AudioSpecificConfig
};

class StreamSink {
public:
  virtual ~StreamSink() = default;

  // Open the transport and write any stream header. Returns false and fills
  // `err` on a fatal error; the streamer then stops writing.
  virtual bool start(const StreamSinkParams &p, std::string &err) = 0;

  // One Annex-B access unit (start-coded NALs). `pts_90k` is a 90 kHz
  // presentation time relative to the first frame.
  virtual void write_video(const uint8_t *annexb, size_t size, bool keyframe,
                           uint64_t pts_90k) = 0;

  // One raw AAC-LC frame (no ADTS wrapper). `pts_90k` shares the video clock.
  virtual void write_audio(const uint8_t *aac, size_t size,
                           uint64_t pts_90k) = 0;

  // Flush and close the transport. Called once at end of stream.
  virtual void finish() = 0;

  // True once a fatal transport error has occurred; the streamer then stops.
  virtual bool failed() const { return false; }
};

} // namespace frametap::enc
