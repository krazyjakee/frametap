#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

// Minimal MPEG-2 Transport Stream demuxer (ISO/IEC 13818-1), the inverse of
// enc::TsMuxer. It resyncs on the 0x47 sync byte, tracks PAT -> PMT to learn
// the video/audio PIDs and codec, reassembles PES packets (delimited by the
// payload_unit_start_indicator), and hands each complete elementary access unit
// to a sink. Annex-B video comes back out exactly as it was muxed, so it can be
// fed straight to a decoder; AAC frames come back with their ADTS header.
//
// Single program, sections assumed to fit one TS packet (which is how TsMuxer
// emits them). No B-frames, so PTS is the only timestamp carried.

namespace frametap::dec {

class TsDemuxer {
public:
  // keyframe reflects the random_access_indicator the muxer set for IDR frames.
  using VideoSink = std::function<void(const uint8_t *data, size_t size,
                                       bool keyframe, uint64_t pts_90k)>;
  using AudioSink =
      std::function<void(const uint8_t *data, size_t size, uint64_t pts_90k)>;

  void init(VideoSink video, AudioSink audio = nullptr);

  // Feed received bytes (any length; need not be packet-aligned).
  void feed(const uint8_t *data, size_t size);
  // Emit the final buffered access unit. Call when the stream ends.
  void flush();

  bool codec_known() const { return video_pid_ >= 0; }
  bool is_hevc() const { return hevc_; }

private:
  struct PesBuf {
    std::vector<uint8_t> data;
    bool started = false;
    bool keyframe = false;
  };

  void handle_packet(const uint8_t *pkt); // exactly 188 bytes
  void parse_pat(const uint8_t *section, size_t len);
  void parse_pmt(const uint8_t *section, size_t len);
  void flush_video();
  void flush_audio();

  VideoSink video_sink_;
  AudioSink audio_sink_;

  std::vector<uint8_t> partial_; // bytes awaiting a full 188-byte packet

  int pmt_pid_ = -1;
  int video_pid_ = -1;
  int audio_pid_ = -1;
  bool hevc_ = false;

  PesBuf video_;
  PesBuf audio_;
};

} // namespace frametap::dec
