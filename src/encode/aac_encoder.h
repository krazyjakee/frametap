#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

// AAC-LC audio encoder (libavcodec).
//
// Takes interleaved 32-bit float PCM, buffers it into the codec's native
// 1024-sample frames, and emits raw AAC access units (no ADTS) suitable for an
// MP4 `mp4a` track. The AudioSpecificConfig produced at open() goes into the
// muxer's esds.

struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace frametap::enc {

class AacEncoder {
public:
  // Called for each encoded AAC access unit.
  using PacketSink = std::function<void(const uint8_t *data, size_t size)>;

  AacEncoder() = default;
  ~AacEncoder();
  AacEncoder(const AacEncoder &) = delete;
  AacEncoder &operator=(const AacEncoder &) = delete;

  // Throws std::runtime_error on failure.
  void open(int sample_rate, int channels, int bitrate_bps, PacketSink sink);

  // Feed interleaved float PCM (`frames` sample-frames across `channels`).
  void encode(const float *interleaved, uint32_t frames);

  // Drain the encoder. Safe to call multiple times.
  void flush();
  void close();

  bool is_open() const { return ctx_ != nullptr; }
  int sample_rate() const { return sample_rate_; }
  int channels() const { return channels_; }
  // AudioSpecificConfig (codec extradata), valid after open().
  const std::vector<uint8_t> &asc() const { return asc_; }

private:
  void drain();
  void send_buffered_frame();

  AVCodecContext *ctx_ = nullptr;
  AVFrame *frame_ = nullptr;
  AVPacket *pkt_ = nullptr;
  PacketSink sink_;

  int sample_rate_ = 48000;
  int channels_ = 2;
  int frame_size_ = 1024;
  int64_t pts_ = 0;

  std::vector<float> buffer_; // interleaved accumulator
  std::vector<uint8_t> asc_;
  bool flushed_ = false;
};

} // namespace frametap::enc
