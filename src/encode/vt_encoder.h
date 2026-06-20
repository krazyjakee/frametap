#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

// VideoToolbox H.264/HEVC encoder for packed-RGBA input (macOS).
//
// The macOS counterpart to NvencEncoder: it wraps a hardware VTCompressionSession
// and exposes the same shape (open/encode/set_bitrate/flush/close) so the shared
// recorder.cpp can swap encoders by platform. Packed RGBA frames are wrapped in a
// CVPixelBuffer and handed to the session; the encoded CMSampleBuffers are
// rewritten to Annex-B access units (start codes, parameter sets inlined on each
// keyframe) so they feed the same hand-rolled MP4/TS/FLV muxers the NVENC path
// uses.
//
// Pimpl keeps the VideoToolbox / CoreMedia headers out of this header so the rest
// of the build (recorder.cpp, which is plain C++) needs no Objective-C.

namespace frametap::enc {

enum class Codec { h264, hevc };

struct EncParams {
  Codec codec = Codec::h264;
  int width = 0;
  int height = 0;
  int fps = 60;
  int bitrate_kbps = 20000;
};

// Called for each encoded Annex-B access unit. `data` is valid only for the
// duration of the call.
using PacketSink =
    std::function<void(const uint8_t *data, size_t size, bool keyframe)>;

class VtEncoder {
public:
  VtEncoder();
  ~VtEncoder();
  VtEncoder(const VtEncoder &) = delete;
  VtEncoder &operator=(const VtEncoder &) = delete;

  // Throws std::runtime_error (with VideoToolbox status detail) on failure.
  void open(const EncParams &params, PacketSink sink);

  // rgba: width*height*4 bytes, rows separated by `stride` bytes.
  void encode(const uint8_t *rgba, int width, int height, size_t stride,
              int64_t timestamp);

  // Change target average bitrate between frames (drives adaptive control).
  void set_bitrate(int bitrate_kbps);

  void flush();
  void close();

  bool is_open() const;

  // Public so the C VideoToolbox output callback (a free function) can reach it.
  struct Impl;

private:
  Impl *impl_ = nullptr;
};

} // namespace frametap::enc
