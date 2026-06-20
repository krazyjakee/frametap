#include <catch2/catch_test_macros.hpp>

#include "encode/nal_util.h"
#include "encode/ts_muxer.h"

#include <cstdint>
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
