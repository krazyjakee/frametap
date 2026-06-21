#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

// Thin wrapper over NVIDIA's NVENC API for packed-RGBA input.
//
// Pimpl keeps nvEncodeAPI.h and the CUDA driver symbols out of this header so
// the rest of the build needs neither. libnvidia-encode.so.1 and libcuda.so.1
// are resolved at runtime via dlopen -- no link-time dependency, so a build
// machine without the NVIDIA driver still compiles this translation unit (it
// just fails at open() time).

namespace frametap::enc {

enum class Codec { h264, hevc };

struct NvencParams {
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

class NvencEncoder {
public:
  NvencEncoder();
  ~NvencEncoder();
  NvencEncoder(const NvencEncoder &) = delete;
  NvencEncoder &operator=(const NvencEncoder &) = delete;

  // Throws std::runtime_error (with NVENC/CUDA detail) on any failure.
  void open(const NvencParams &params, PacketSink sink);

  // rgba: width*height*4 bytes, rows separated by `stride` bytes.
  void encode(const uint8_t *rgba, int width, int height, size_t stride,
              int64_t timestamp);

  // Change target average bitrate between frames (drives adaptive control).
  void set_bitrate(int bitrate_kbps);

  void flush();
  void close();

  bool is_open() const;

private:
  struct Impl;
  Impl *impl_ = nullptr;
};

} // namespace frametap::enc
