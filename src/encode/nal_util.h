#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// H.264/H.265 NAL helpers shared by the streaming sinks. These convert the
// Annex-B access units NVENC emits into the length-prefixed form RTMP/FLV
// (and MP4) expect, and build the avcC / hvcC decoder configuration records.

namespace frametap::enc {

struct ParamSets {
  std::vector<uint8_t> sps; // raw NAL (no start code)
  std::vector<uint8_t> pps;
  std::vector<uint8_t> vps; // HEVC only
  bool complete(bool hevc) const {
    return hevc ? (!vps.empty() && !sps.empty() && !pps.empty())
                : (!sps.empty() && !pps.empty());
  }
};

// Split an Annex-B buffer, capturing parameter-set NALs into `ps` and appending
// the remaining NALs to `out` as 4-byte-length-prefixed units.
void annexb_to_length_prefixed(const uint8_t *data, size_t size, bool hevc,
                               ParamSets &ps, std::vector<uint8_t> &out);

// avcC (H.264) / hvcC (H.265) decoder configuration record bytes.
std::vector<uint8_t> build_avcc(const ParamSets &ps);
std::vector<uint8_t> build_hvcc(const ParamSets &ps);

} // namespace frametap::enc
