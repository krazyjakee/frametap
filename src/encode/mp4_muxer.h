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
  void write_access_unit(const uint8_t *data, size_t size, bool keyframe);

  // Finalize: patch the mdat size and write moov. Safe to call multiple times.
  void close();

  bool is_open() const { return file_.is_open(); }
  uint64_t media_bytes() const { return mdat_payload_; }

private:
  struct Sample {
    uint32_t size = 0;
    bool keyframe = false;
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
  uint64_t mdat_payload_ = 0;  // bytes of sample data written
  bool finalized_ = false;
};

} // namespace frametap::enc
