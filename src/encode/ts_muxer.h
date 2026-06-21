#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

// Minimal MPEG-2 Transport Stream muxer (ISO/IEC 13818-1).
//
// Packs Annex-B H.264/H.265 access units and raw AAC-LC frames into 188-byte
// TS packets and hands each finished packet to a byte sink (the caller pushes
// them over UDP or SRT). PAT/PMT are (re)emitted ahead of every video keyframe
// so a receiver joining mid-stream can lock on quickly. PCR rides the video
// PID's adaptation field.
//
// No B-frames are assumed (NVENC low-latency output), so DTS == PTS and only a
// PTS is written. Single program, fixed PIDs.

namespace frametap::enc {

class TsMuxer {
public:
  using ByteSink = std::function<void(const uint8_t *data, size_t size)>;

  // `audio_rate`/`audio_channels` are only used to build AAC ADTS headers when
  // has_audio is true. The sink is called for each 188-byte packet produced.
  void init(bool hevc, bool has_audio, int audio_rate, int audio_channels,
            ByteSink sink);

  void write_video(const uint8_t *annexb, size_t size, bool keyframe,
                   uint64_t pts_90k);
  void write_audio(const uint8_t *aac, size_t size, uint64_t pts_90k);

private:
  void emit_psi();                         // PAT + PMT
  void emit_pat();
  void emit_pmt();
  // Packetize one PES payload onto `pid`, optionally carrying PCR (video) and a
  // random-access indicator (keyframe) in the first packet's adaptation field.
  void emit_pes(int pid, uint8_t stream_id, const uint8_t *payload, size_t len,
                uint64_t pts_90k, bool with_pcr, bool rai);
  uint8_t next_cc(int pid);

  bool hevc_ = false;
  bool has_audio_ = false;
  int audio_rate_ = 48000;
  int audio_channels_ = 2;
  int sample_rate_index_ = 3; // ADTS index for 48 kHz
  ByteSink sink_;

  uint8_t pat_cc_ = 0;
  uint8_t pmt_cc_ = 0;
  uint8_t video_cc_ = 0;
  uint8_t audio_cc_ = 0;
  uint8_t pmt_version_ = 0;

  std::vector<uint8_t> scratch_; // reused PES assembly buffer
};

} // namespace frametap::enc
