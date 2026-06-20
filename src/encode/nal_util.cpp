#include "encode/nal_util.h"

#include <cstring>
#include <utility>

namespace frametap::enc {
namespace {

void put_u16(std::vector<uint8_t> &v, uint16_t x) {
  v.push_back(static_cast<uint8_t>(x >> 8));
  v.push_back(static_cast<uint8_t>(x));
}
void put_u32(std::vector<uint8_t> &v, uint32_t x) {
  v.push_back(static_cast<uint8_t>(x >> 24));
  v.push_back(static_cast<uint8_t>(x >> 16));
  v.push_back(static_cast<uint8_t>(x >> 8));
  v.push_back(static_cast<uint8_t>(x));
}

// Strip emulation-prevention bytes (00 00 03 -> 00 00) for header parsing.
std::vector<uint8_t> to_rbsp(const uint8_t *d, size_t n) {
  std::vector<uint8_t> r;
  r.reserve(n);
  int zeros = 0;
  for (size_t i = 0; i < n; ++i) {
    uint8_t b = d[i];
    if (zeros >= 2 && b == 3) {
      zeros = 0;
      continue;
    }
    r.push_back(b);
    zeros = (b == 0) ? zeros + 1 : 0;
  }
  return r;
}

} // namespace

void annexb_to_length_prefixed(const uint8_t *d, size_t n, bool hevc,
                               ParamSets &ps, std::vector<uint8_t> &out) {
  auto sc_len = [&](size_t p) -> int {
    if (p + 3 <= n && d[p] == 0 && d[p + 1] == 0 && d[p + 2] == 1)
      return 3;
    if (p + 4 <= n && d[p] == 0 && d[p + 1] == 0 && d[p + 2] == 0 &&
        d[p + 3] == 1)
      return 4;
    return 0;
  };
  size_t p = 0;
  while (p < n && sc_len(p) == 0)
    ++p;
  while (p < n) {
    int sc = sc_len(p);
    size_t start = p + sc;
    size_t q = start;
    while (q < n && sc_len(q) == 0)
      ++q;
    size_t len = q - start;
    while (len > 0 && d[start + len - 1] == 0)
      --len;
    if (len > 0) {
      const uint8_t *nal = d + start;
      const int type = hevc ? ((nal[0] >> 1) & 0x3F) : (nal[0] & 0x1F);
      const bool is_param = hevc ? (type == 32 || type == 33 || type == 34)
                                 : (type == 7 || type == 8);
      if (is_param) {
        std::vector<uint8_t> v(nal, nal + len);
        if (hevc) {
          if (type == 32 && ps.vps.empty()) ps.vps = std::move(v);
          else if (type == 33 && ps.sps.empty()) ps.sps = std::move(v);
          else if (type == 34 && ps.pps.empty()) ps.pps = std::move(v);
        } else {
          if (type == 7 && ps.sps.empty()) ps.sps = std::move(v);
          else if (type == 8 && ps.pps.empty()) ps.pps = std::move(v);
        }
      } else {
        put_u32(out, static_cast<uint32_t>(len));
        out.insert(out.end(), nal, nal + len);
      }
    }
    p = q;
  }
}

std::vector<uint8_t> build_avcc(const ParamSets &ps) {
  std::vector<uint8_t> v;
  v.push_back(1); // configurationVersion
  v.push_back(ps.sps.size() > 1 ? ps.sps[1] : 0); // AVCProfileIndication
  v.push_back(ps.sps.size() > 2 ? ps.sps[2] : 0); // profile_compatibility
  v.push_back(ps.sps.size() > 3 ? ps.sps[3] : 0); // AVCLevelIndication
  v.push_back(0xFF);                              // lengthSizeMinusOne = 3
  v.push_back(0xE1);                              // numOfSPS = 1
  put_u16(v, static_cast<uint16_t>(ps.sps.size()));
  v.insert(v.end(), ps.sps.begin(), ps.sps.end());
  v.push_back(1); // numOfPPS
  put_u16(v, static_cast<uint16_t>(ps.pps.size()));
  v.insert(v.end(), ps.pps.begin(), ps.pps.end());
  return v;
}

std::vector<uint8_t> build_hvcc(const ParamSets &ps) {
  std::vector<uint8_t> rbsp = to_rbsp(ps.sps.data(), ps.sps.size());
  uint8_t ptl[12] = {0};
  if (rbsp.size() >= 15)
    std::memcpy(ptl, &rbsp[3], 12);

  std::vector<uint8_t> v;
  v.push_back(1);      // configurationVersion
  v.push_back(ptl[0]); // general_profile_space/tier/profile_idc
  v.insert(v.end(), &ptl[1], &ptl[5]);  // profile_compatibility_flags
  v.insert(v.end(), &ptl[5], &ptl[11]); // constraint_indicator_flags
  v.push_back(ptl[11]);                 // general_level_idc
  v.push_back(0xF0);
  v.push_back(0x00);
  v.push_back(0xFC);
  v.push_back(0xFD);
  v.push_back(0xF8);
  v.push_back(0xF8);
  put_u16(v, 0); // avgFrameRate
  v.push_back((1 << 3) | 3);
  v.push_back(3); // numOfArrays
  auto put_array = [&](int nal_type, const std::vector<uint8_t> &nal) {
    v.push_back(static_cast<uint8_t>(0x80 | nal_type));
    put_u16(v, 1);
    put_u16(v, static_cast<uint16_t>(nal.size()));
    v.insert(v.end(), nal.begin(), nal.end());
  };
  put_array(32, ps.vps);
  put_array(33, ps.sps);
  put_array(34, ps.pps);
  return v;
}

} // namespace frametap::enc
