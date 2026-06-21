#include <catch2/catch_test_macros.hpp>

#include "encode/mp4_muxer.h"
#include "encode/nal_util.h"
#include "encode/ts_muxer.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

using namespace frametap::enc;

namespace {

// Append an Annex-B NAL (4-byte start code + 1 header byte + filler).
void push_nal(std::vector<uint8_t> &b, uint8_t header, int len) {
  b.push_back(0);
  b.push_back(0);
  b.push_back(0);
  b.push_back(1);
  b.push_back(header);
  for (int i = 1; i < len; ++i)
    b.push_back(static_cast<uint8_t>((i * 7) & 0xFF));
}

int pid_of(const uint8_t *pkt) {
  return ((pkt[1] & 0x1F) << 8) | pkt[2];
}
bool pusi(const uint8_t *pkt) { return (pkt[1] & 0x40) != 0; }

} // namespace

TEST_CASE("TS muxer emits aligned, sync-prefixed packets", "[unit][muxer]") {
  std::vector<uint8_t> out;
  TsMuxer m;
  m.init(/*hevc=*/false, /*has_audio=*/false, 48000, 2,
         [&](const uint8_t *d, size_t n) { out.insert(out.end(), d, d + n); });

  std::vector<uint8_t> key;
  push_nal(key, 0x67, 16); // SPS
  push_nal(key, 0x68, 8);  // PPS
  push_nal(key, 0x65, 4000); // IDR (spans multiple TS packets)
  std::vector<uint8_t> inter;
  push_nal(inter, 0x41, 1200);

  for (int f = 0; f < 6; ++f) {
    if (f == 0)
      m.write_video(key.data(), key.size(), true, 0);
    else
      m.write_video(inter.data(), inter.size(), false,
                    static_cast<uint64_t>(f) * 3000);
  }

  REQUIRE(out.size() % 188 == 0);
  REQUIRE(out.size() / 188 > 6);

  bool saw_pat = false, saw_pmt = false, saw_video = false;
  for (size_t i = 0; i < out.size(); i += 188) {
    const uint8_t *pkt = &out[i];
    CHECK(pkt[0] == 0x47); // sync byte on every packet
    const int pid = pid_of(pkt);
    if (pid == 0x0000)
      saw_pat = true;
    else if (pid == 0x1000)
      saw_pmt = true;
    else if (pid == 0x0100)
      saw_video = true;
  }
  CHECK(saw_pat);
  CHECK(saw_pmt);
  CHECK(saw_video);
}

TEST_CASE("TS muxer (re)sends PAT/PMT before each keyframe", "[unit][muxer]") {
  std::vector<uint8_t> out;
  TsMuxer m;
  m.init(false, false, 48000, 2,
         [&](const uint8_t *d, size_t n) { out.insert(out.end(), d, d + n); });

  std::vector<uint8_t> key;
  push_nal(key, 0x67, 12);
  push_nal(key, 0x68, 6);
  push_nal(key, 0x65, 200);

  m.write_video(key.data(), key.size(), true, 0);
  m.write_video(key.data(), key.size(), true, 3000);

  int pat_count = 0;
  for (size_t i = 0; i < out.size(); i += 188)
    if (pid_of(&out[i]) == 0x0000)
      ++pat_count;
  CHECK(pat_count == 2); // one ahead of each keyframe
}

TEST_CASE("TS muxer wraps AAC in an ADTS-framed PES", "[unit][muxer]") {
  std::vector<uint8_t> out;
  TsMuxer m;
  m.init(false, /*has_audio=*/true, 48000, 2,
         [&](const uint8_t *d, size_t n) { out.insert(out.end(), d, d + n); });

  std::vector<uint8_t> aac(300, 0xAB);
  m.write_audio(aac.data(), aac.size(), 0);

  // Find the first audio-PID packet (PID 0x101) carrying a PES start.
  const uint8_t *audio_pkt = nullptr;
  for (size_t i = 0; i < out.size(); i += 188) {
    if (pid_of(&out[i]) == 0x0101 && pusi(&out[i])) {
      audio_pkt = &out[i];
      break;
    }
  }
  REQUIRE(audio_pkt != nullptr);

  // Payload starts after the 4-byte TS header (payload-only packet, afc=01).
  const uint8_t *pes = audio_pkt + 4;
  CHECK(pes[0] == 0x00);
  CHECK(pes[1] == 0x00);
  CHECK(pes[2] == 0x01);   // PES start code
  CHECK(pes[3] == 0xC0);   // audio stream id
  // 6-byte PES header + 3 (flags/flags/hdrlen) + 5-byte PTS = 14.
  const uint8_t *payload = pes + 14;
  // ADTS sync word: 0xFFF in the top 12 bits.
  CHECK(payload[0] == 0xFF);
  CHECK((payload[1] & 0xF0) == 0xF0);
}

TEST_CASE("nal_util splits Annex-B and captures parameter sets",
          "[unit][muxer]") {
  std::vector<uint8_t> annexb;
  push_nal(annexb, 0x67, 10); // SPS
  push_nal(annexb, 0x68, 5);  // PPS
  push_nal(annexb, 0x65, 50); // IDR

  ParamSets ps;
  std::vector<uint8_t> out;
  annexb_to_length_prefixed(annexb.data(), annexb.size(), false, ps, out);

  CHECK(ps.sps.size() == 10);
  CHECK(ps.pps.size() == 5);
  CHECK(ps.complete(false));

  // Output holds only the IDR, 4-byte length-prefixed (no start codes).
  REQUIRE(out.size() == 4 + 50);
  const uint32_t len =
      (out[0] << 24) | (out[1] << 16) | (out[2] << 8) | out[3];
  CHECK(len == 50);
  CHECK(out[4] == 0x65); // IDR NAL header
}

TEST_CASE("nal_util builds a well-formed avcC record", "[unit][muxer]") {
  ParamSets ps;
  push_nal(ps.sps, 0x67, 8); // borrow push_nal to get a header+filler...
  // push_nal added a start code; strip it for a raw NAL.
  ps.sps.erase(ps.sps.begin(), ps.sps.begin() + 4);
  ps.pps = {0x68, 0x01, 0x02};

  std::vector<uint8_t> avcc = build_avcc(ps);
  REQUIRE(avcc.size() > 8);
  CHECK(avcc[0] == 0x01);             // configurationVersion
  CHECK(avcc[4] == 0xFF);             // lengthSizeMinusOne = 3
  CHECK((avcc[5] & 0x1F) == 1);       // numOfSequenceParameterSets = 1
  const uint16_t sps_len = (avcc[6] << 8) | avcc[7];
  CHECK(sps_len == ps.sps.size());
}

// --- MP4 muxer: parse the file back and assert the box structure. ---

namespace {

using Bytes = std::vector<uint8_t>;
using Range = std::pair<size_t, size_t>; // [content_start, content_end)

uint32_t rd_u32(const Bytes &b, size_t p) {
  return (uint32_t(b[p]) << 24) | (uint32_t(b[p + 1]) << 16) |
         (uint32_t(b[p + 2]) << 8) | uint32_t(b[p + 3]);
}
uint64_t rd_u64(const Bytes &b, size_t p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i)
    v = (v << 8) | b[p + i];
  return v;
}

// Every direct child box of `type` in [beg,end), returned as content ranges.
std::vector<Range> find_all(const Bytes &b, size_t beg, size_t end,
                            const char *type) {
  std::vector<Range> r;
  size_t p = beg;
  while (p + 8 <= end) {
    uint64_t sz = rd_u32(b, p);
    size_t hdr = 8;
    if (sz == 1) {
      sz = rd_u64(b, p + 8);
      hdr = 16;
    } else if (sz == 0) {
      sz = end - p;
    }
    if (sz < hdr || p + sz > end)
      break;
    if (std::memcmp(&b[p + 4], type, 4) == 0)
      r.emplace_back(p + hdr, p + sz);
    p += sz;
  }
  return r;
}

// First direct child box of `type`; aborts the test if absent.
Range find_box(const Bytes &b, Range in, const char *type) {
  auto all = find_all(b, in.first, in.second, type);
  REQUIRE_FALSE(all.empty());
  return all.front();
}

Range descend(const Bytes &b, Range in, std::initializer_list<const char *> path) {
  Range cur = in;
  for (const char *t : path)
    cur = find_box(b, cur, t);
  return cur;
}

Bytes read_file(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  return Bytes((std::istreambuf_iterator<char>(f)),
               std::istreambuf_iterator<char>());
}

std::string temp_mp4(const char *name) {
  return (std::filesystem::temp_directory_path() / name).string();
}

// One Annex-B access unit: SPS (0x67) + PPS (0x68) + a slice NAL.
Bytes make_au(uint8_t slice_hdr, int slice_len, bool with_param_sets) {
  Bytes au;
  if (with_param_sets) {
    push_nal(au, 0x67, 12); // SPS
    push_nal(au, 0x68, 6);  // PPS
  }
  push_nal(au, slice_hdr, slice_len);
  return au;
}

} // namespace

TEST_CASE("MP4 muxer writes a parseable single video track", "[unit][muxer]") {
  const std::string path = temp_mp4("frametap_test_video.mp4");
  {
    Mp4Muxer m;
    m.open(path, /*hevc=*/false, 1280, 720, 60);
    Bytes key = make_au(0x65, 200, /*with_param_sets=*/true); // IDR
    Bytes inter = make_au(0x41, 80, false);
    m.write_access_unit(key.data(), key.size(), true, 0);
    m.write_access_unit(inter.data(), inter.size(), false, 1500);
    m.write_access_unit(inter.data(), inter.size(), false, 3000);
    m.close();
  }
  Bytes b = read_file(path);
  std::filesystem::remove(path);
  REQUIRE(b.size() > 64);

  Range whole{0, b.size()};
  // Top-level structure.
  (void)find_box(b, whole, "ftyp");
  Range mdat = find_box(b, whole, "mdat");
  Range moov = find_box(b, whole, "moov");

  // moov/trak/.../stsd holds avc1 -> avcC; parameter sets were lifted out of
  // the samples into the config record.
  Range stbl = descend(b, moov, {"trak", "mdia", "minf", "stbl"});
  Range stsd = find_box(b, stbl, "stsd");
  Range avc1 = find_box(b, {stsd.first + 8, stsd.second}, "avc1"); // skip vhdr
  // width @ +24 (u16), height @ +26 (u16) within the 78-byte sample-entry body.
  CHECK(((b[avc1.first + 24] << 8) | b[avc1.first + 25]) == 1280);
  CHECK(((b[avc1.first + 26] << 8) | b[avc1.first + 27]) == 720);
  Range avcc = find_box(b, {avc1.first + 78, avc1.second}, "avcC");
  CHECK(b[avcc.first] == 0x01); // avcC configurationVersion

  // stsz: three samples, the IDR first (4-byte length prefix + 200 payload).
  Range stsz = find_box(b, stbl, "stsz");
  CHECK(rd_u32(b, stsz.first + 8) == 3u); // sample_count
  CHECK(rd_u32(b, stsz.first + 12) == 204u);

  // stco points at the start of the mdat payload, where the first sample's
  // 4-byte length prefix (== 200) lives.
  Range stco = find_box(b, stbl, "stco");
  const uint32_t chunk_off = rd_u32(b, stco.first + 8);
  CHECK(chunk_off == mdat.first); // mdat content == first chunk
  CHECK(rd_u32(b, chunk_off) == 200u);

  // stts: real per-frame durations -> one run of three 1500-tick gaps.
  Range stts = find_box(b, stbl, "stts");
  CHECK(rd_u32(b, stts.first + 4) == 1u);  // one run
  CHECK(rd_u32(b, stts.first + 8) == 3u);  // count
  CHECK(rd_u32(b, stts.first + 12) == 1500u); // delta
}

TEST_CASE("MP4 muxer adds an audio track with an empty-edit delay",
          "[unit][muxer]") {
  const std::string path = temp_mp4("frametap_test_av.mp4");
  std::vector<uint8_t> asc = {0x11, 0x90}; // 48 kHz stereo AAC-LC
  const uint64_t delay_90k = 4500;         // 50 ms after the first frame
  {
    Mp4Muxer m;
    m.open(path, false, 640, 480, 60);
    Bytes key = make_au(0x65, 120, true);
    m.write_access_unit(key.data(), key.size(), true, 0);
    m.write_access_unit(key.data(), key.size(), true, 1500);
    m.set_audio(48000, 2, asc.data(), asc.size(), 1024, delay_90k);
    Bytes frame(100, 0xAB);
    for (int i = 0; i < 5; ++i)
      m.write_audio_sample(frame.data(), frame.size());
    m.close();
  }
  Bytes b = read_file(path);
  std::filesystem::remove(path);

  Range whole{0, b.size()};
  Range moov = find_box(b, whole, "moov");
  auto traks = find_all(b, moov.first, moov.second, "trak");
  REQUIRE(traks.size() == 2); // video + audio
  Range atrak = traks[1];

  // Audio track is AAC: stsd -> mp4a -> esds.
  Range astbl = descend(b, atrak, {"mdia", "minf", "stbl"});
  Range astsd = find_box(b, astbl, "stsd");
  Range mp4a = find_box(b, {astsd.first + 8, astsd.second}, "mp4a");
  (void)find_box(b, {mp4a.first + 28, mp4a.second}, "esds");

  // Five AAC frames buffered.
  Range astsz = find_box(b, astbl, "stsz");
  CHECK(rd_u32(b, astsz.first + 8) == 5u);

  // edts/elst: an empty edit (media_time == -1) lasting the capture delay.
  Range elst = descend(b, atrak, {"edts", "elst"});
  CHECK(rd_u32(b, elst.first + 4) == 2u); // empty edit + media
  CHECK(rd_u32(b, elst.first + 8) == static_cast<uint32_t>(delay_90k));
  CHECK(rd_u32(b, elst.first + 12) == 0xFFFFFFFFu); // media_time = -1
}
