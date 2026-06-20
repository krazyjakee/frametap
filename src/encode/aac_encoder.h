#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

// AAC-LC audio encoder (vo-aacenc, Apache-2.0; vendored, no ffmpeg).
//
// Takes interleaved 32-bit float PCM, converts to the 16-bit PCM vo-aacenc
// wants, buffers it into 1024-sample frames, and emits raw AAC access units (no
// ADTS) suitable for an MP4 `mp4a` track. The AudioSpecificConfig is synthesized
// at open() from the sample rate / channel count and goes into the muxer's esds.

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

  bool is_open() const { return enc_ != nullptr; }
  int sample_rate() const { return sample_rate_; }
  int channels() const { return channels_; }
  // AudioSpecificConfig, valid after open().
  const std::vector<uint8_t> &asc() const { return asc_; }

private:
  void encode_block(); // encode one full 1024-sample frame from buffer_ front

  void *enc_ = nullptr; // VO_HANDLE
  void *api_ = nullptr; // heap VO_AUDIO_CODECAPI
  void *mem_ = nullptr; // heap VO_MEM_OPERATOR
  PacketSink sink_;

  int sample_rate_ = 48000;
  int channels_ = 2;
  int frame_size_ = 1024;

  std::vector<int16_t> buffer_; // interleaved 16-bit PCM accumulator
  std::vector<uint8_t> asc_;
  bool flushed_ = false;
};

} // namespace frametap::enc
