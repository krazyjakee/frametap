#include <catch2/catch_test_macros.hpp>

#include "decode/ts_demux.h"
#include "encode/ts_muxer.h"

#include <cstdint>
#include <vector>

using frametap::dec::TsDemuxer;
using frametap::enc::TsMuxer;

namespace {

struct VideoAu {
  std::vector<uint8_t> data;
  bool keyframe;
  uint64_t pts;
};

// Build a fake Annex-B access unit: a 4-byte start code, a NAL header byte, then
// `len` bytes of incrementing filler. Content is opaque to the muxer/demuxer.
std::vector<uint8_t> make_au(uint8_t nal_header, size_t len, uint8_t seed) {
  std::vector<uint8_t> au = {0x00, 0x00, 0x00, 0x01, nal_header};
  for (size_t i = 0; i < len; ++i)
    au.push_back(static_cast<uint8_t>(seed + i));
  return au;
}

} // namespace

TEST_CASE("TS mux -> demux round-trips a video access unit",
          "[unit][decode][ts]") {
  std::vector<uint8_t> ts;
  TsMuxer mux;
  mux.init(/*hevc=*/false, /*has_audio=*/false, 0, 0,
           [&](const uint8_t *d, size_t n) { ts.insert(ts.end(), d, d + n); });

  const auto au = make_au(0x65, 200, 7); // IDR-ish payload
  mux.write_video(au.data(), au.size(), /*keyframe=*/true, /*pts=*/90000);

  std::vector<VideoAu> out;
  TsDemuxer demux;
  demux.init([&](const uint8_t *d, size_t n, bool kf, uint64_t pts) {
    out.push_back({std::vector<uint8_t>(d, d + n), kf, pts});
  });
  demux.feed(ts.data(), ts.size());
  demux.flush();

  REQUIRE(out.size() == 1);
  CHECK(out[0].data == au);
  CHECK(out[0].keyframe);
  CHECK(out[0].pts == 90000);
  CHECK(demux.codec_known());
  CHECK_FALSE(demux.is_hevc());
}

TEST_CASE("TS demux reassembles a large multi-packet access unit",
          "[unit][decode][ts]") {
  std::vector<uint8_t> ts;
  TsMuxer mux;
  mux.init(true, false, 0, 0,
           [&](const uint8_t *d, size_t n) { ts.insert(ts.end(), d, d + n); });

  // Much larger than one 188-byte TS packet, forcing PES fragmentation.
  const auto au = make_au(0x26, 5000, 1);
  mux.write_video(au.data(), au.size(), true, 12345);

  std::vector<VideoAu> out;
  TsDemuxer demux;
  demux.init([&](const uint8_t *d, size_t n, bool kf, uint64_t pts) {
    out.push_back({std::vector<uint8_t>(d, d + n), kf, pts});
  });
  demux.feed(ts.data(), ts.size());
  demux.flush();

  REQUIRE(out.size() == 1);
  CHECK(out[0].data == au);
  CHECK(out[0].pts == 12345);
  CHECK(demux.is_hevc());
}

TEST_CASE("TS demux delimits a sequence of frames", "[unit][decode][ts]") {
  std::vector<uint8_t> ts;
  TsMuxer mux;
  mux.init(false, false, 0, 0,
           [&](const uint8_t *d, size_t n) { ts.insert(ts.end(), d, d + n); });

  std::vector<std::vector<uint8_t>> aus;
  for (int i = 0; i < 5; ++i) {
    auto au = make_au(i == 0 ? 0x65 : 0x41, 100 + i * 40,
                      static_cast<uint8_t>(i));
    mux.write_video(au.data(), au.size(), /*keyframe=*/i == 0,
                    /*pts=*/90000ull * (i + 1));
    aus.push_back(std::move(au));
  }

  std::vector<VideoAu> out;
  TsDemuxer demux;
  demux.init([&](const uint8_t *d, size_t n, bool kf, uint64_t pts) {
    out.push_back({std::vector<uint8_t>(d, d + n), kf, pts});
  });
  demux.feed(ts.data(), ts.size());
  demux.flush();

  REQUIRE(out.size() == aus.size());
  for (size_t i = 0; i < aus.size(); ++i) {
    CHECK(out[i].data == aus[i]);
    CHECK(out[i].pts == 90000ull * (i + 1));
    CHECK(out[i].keyframe == (i == 0));
  }
}

TEST_CASE("TS demux survives byte-misaligned feeds", "[unit][decode][ts]") {
  std::vector<uint8_t> ts;
  TsMuxer mux;
  mux.init(false, false, 0, 0,
           [&](const uint8_t *d, size_t n) { ts.insert(ts.end(), d, d + n); });
  const auto au = make_au(0x65, 1500, 3);
  mux.write_video(au.data(), au.size(), true, 4242);

  std::vector<VideoAu> out;
  TsDemuxer demux;
  demux.init([&](const uint8_t *d, size_t n, bool kf, uint64_t pts) {
    out.push_back({std::vector<uint8_t>(d, d + n), kf, pts});
  });
  // Feed in odd-sized chunks that don't align to 188 bytes.
  for (size_t i = 0; i < ts.size(); i += 100)
    demux.feed(ts.data() + i, std::min<size_t>(100, ts.size() - i));
  demux.flush();

  REQUIRE(out.size() == 1);
  CHECK(out[0].data == au);
  CHECK(out[0].pts == 4242);
}

TEST_CASE("TS mux -> demux round-trips audio frames", "[unit][decode][ts]") {
  std::vector<uint8_t> ts;
  TsMuxer mux;
  mux.init(false, /*has_audio=*/true, 48000, 2,
           [&](const uint8_t *d, size_t n) { ts.insert(ts.end(), d, d + n); });

  // A video keyframe first so PAT/PMT (which carry the audio PID) are emitted.
  const auto vau = make_au(0x65, 80, 9);
  mux.write_video(vau.data(), vau.size(), true, 90000);

  std::vector<uint8_t> aac;
  for (int i = 0; i < 64; ++i)
    aac.push_back(static_cast<uint8_t>(i * 3));
  mux.write_audio(aac.data(), aac.size(), 90000);

  std::vector<std::vector<uint8_t>> audio_out;
  TsDemuxer demux;
  demux.init(nullptr, [&](const uint8_t *d, size_t n, uint64_t) {
    audio_out.push_back(std::vector<uint8_t>(d, d + n));
  });
  demux.feed(ts.data(), ts.size());
  demux.flush();

  REQUIRE(audio_out.size() == 1);
  // The muxer prepends a 7-byte ADTS header to the raw AAC payload.
  REQUIRE(audio_out[0].size() == aac.size() + 7);
  std::vector<uint8_t> payload(audio_out[0].begin() + 7, audio_out[0].end());
  CHECK(payload == aac);
}
