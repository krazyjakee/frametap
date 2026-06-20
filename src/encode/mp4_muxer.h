#pragma once

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

// Minimal MP4 (ISO BMFF) muxer for H.264/HEVC elementary streams.
//
// The NVENC encoder emits Annex-B access units (NAL units separated by start
// codes, with SPS/PPS repeated in band). This muxer rewrites them into a
// directly-playable .mp4: parameter sets are lifted into the avcC/hvcC config
// record in `stsd`, each access unit becomes one length-prefixed sample in
// `mdat`, and a flat `moov` (single chunk, constant frame rate) is written on
// close. Output opens in QuickTime / Windows Media Player / browsers.

namespace frametap::enc {

class Mp4Muxer {
public:
  Mp4Muxer() = default;
  ~Mp4Muxer();
  Mp4Muxer(const Mp4Muxer &) = delete;
  Mp4Muxer &operator=(const Mp4Muxer &) = delete;

  // Throws std::runtime_error if the file can't be opened. `hevc` selects
  // HEVC (hvc1) vs H.264 (avc1). `fps` drives the constant per-sample timing.
  void open(const std::string &path, bool hevc, int width, int height,
            int fps);

  // Feed one Annex-B access unit (may contain multiple NALs). Parameter-set
  // NALs are captured for the config record and stripped from the sample.
  // `pts_90k` is the frame's presentation time in a 90 kHz clock (relative to
  // the first frame); it drives real per-sample timing so playback speed and
  // A/V sync match wall-clock capture.
  void write_access_unit(const uint8_t *data, size_t size, bool keyframe,
                         uint64_t pts_90k);

  // Enable an AAC audio track. `asc` is the AudioSpecificConfig for esds.
  // Call once before write_audio_sample(); samples_per_frame is the AAC frame
  // size (1024). Audio samples are buffered in memory and appended to mdat on
  // close.
  void set_audio(int sample_rate, int channels, const uint8_t *asc,
                 size_t asc_len, int samples_per_frame);
  void write_audio_sample(const uint8_t *data, size_t size);

  // Finalize: patch the mdat size and write moov. Safe to call multiple times.
  void close();

  bool is_open() const { return file_.is_open(); }
  uint64_t media_bytes() const { return mdat_payload_; }

private:
  struct Sample {
    uint32_t size = 0;
    bool keyframe = false;
    uint64_t pts = 0; // 90 kHz, relative to first frame
  };

  void finalize();

  std::ofstream file_;
  bool hevc_ = false;
  int width_ = 0;
  int height_ = 0;
  int fps_ = 60;

  // Parameter sets (raw NAL payloads, no start code), captured once.
  std::vector<uint8_t> sps_;
  std::vector<uint8_t> pps_;
  std::vector<uint8_t> vps_; // HEVC only

  std::vector<Sample> samples_;
  uint64_t mdat_size_pos_ = 0; // file offset of the mdat 64-bit largesize
  uint64_t mdat_payload_ = 0;  // bytes of video sample data written
  bool finalized_ = false;

  // Audio track (optional). Samples are buffered, then appended to mdat as a
  // second chunk on close.
  bool has_audio_ = false;
  int audio_rate_ = 48000;
  int audio_channels_ = 2;
  int audio_frame_samples_ = 1024;
  std::vector<uint8_t> audio_asc_;
  std::vector<uint8_t> audio_data_;        // concatenated AAC frames
  std::vector<uint32_t> audio_sample_sizes_;
};

} // namespace frametap::enc
