#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

// VideoToolbox H.264/HEVC decoder (macOS), the decode-side counterpart of
// enc::VtEncoder and the macOS equivalent of dec::NvdecDecoder. It consumes
// Annex-B access units and produces packed-RGBA frames via a hardware
// VTDecompressionSession.
//
// Pimpl keeps the VideoToolbox / CoreMedia headers out of this header so
// receiver.cpp (plain C++) needs no Objective-C.

namespace frametap::dec {

enum class Codec { h264, hevc };

// Called for each decoded frame, top-down packed RGBA (width*height*4 bytes).
// `data` is valid only for the duration of the call.
using FrameSink = std::function<void(const uint8_t *rgba, int width, int height,
                                     uint64_t pts_90k)>;

class VtDecoder {
public:
  VtDecoder();
  ~VtDecoder();
  VtDecoder(const VtDecoder &) = delete;
  VtDecoder &operator=(const VtDecoder &) = delete;

  // Throws std::runtime_error (with VideoToolbox status detail) on failure.
  void open(Codec codec, FrameSink sink);

  // Feed one Annex-B access unit. Decoded frames are delivered via the sink on
  // this same thread (the session decodes synchronously).
  void decode(const uint8_t *annexb, size_t size, uint64_t pts_90k);

  void flush();
  void close();
  bool is_open() const;

  // Public so the C VideoToolbox output callback (a free function) can reach it.
  struct Impl;

private:
  Impl *impl_ = nullptr;
};

} // namespace frametap::dec
