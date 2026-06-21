#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

// Thin wrapper over NVIDIA's NVDEC (cuvid) API, the decode-side counterpart of
// enc::NvencEncoder. It consumes Annex-B H.264/HEVC access units and produces
// packed-RGBA frames.
//
// As with the encoder, the CUDA driver and libnvcuvid symbols are resolved at
// runtime via dlopen, so this translation unit compiles with only the vendored
// nv-codec-headers and fails gracefully at open() time on a machine without the
// NVIDIA driver. NV12 -> RGBA conversion is done on the CPU after copying the
// decoded surface back from the GPU, which keeps us free of a CUDA kernel
// (no nvcc) at the cost of a per-frame device-to-host copy -- fine for preview.

namespace frametap::dec {

enum class Codec { h264, hevc };

// Called for each decoded frame, top-down packed RGBA (width*height*4 bytes).
// `data` is valid only for the duration of the call.
using FrameSink = std::function<void(const uint8_t *rgba, int width, int height,
                                     uint64_t pts_90k)>;

class NvdecDecoder {
public:
  NvdecDecoder();
  ~NvdecDecoder();
  NvdecDecoder(const NvdecDecoder &) = delete;
  NvdecDecoder &operator=(const NvdecDecoder &) = delete;

  // Throws std::runtime_error (with CUDA/NVDEC detail) on any failure.
  void open(Codec codec, FrameSink sink);

  // Feed one Annex-B access unit. Decoded frames are delivered via the sink,
  // possibly on this same thread and possibly after a small reorder delay.
  void decode(const uint8_t *annexb, size_t size, uint64_t pts_90k);

  void flush();
  void close();
  bool is_open() const;

private:
  struct Impl;
  Impl *impl_ = nullptr;
};

} // namespace frametap::dec
