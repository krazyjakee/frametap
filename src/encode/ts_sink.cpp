#include "encode/ts_sink.h"

namespace frametap::enc {

namespace {
constexpr size_t kDatagramBytes = 7 * 188; // 1316
}

bool TsSink::start(const StreamSinkParams &p, std::string &err) {
  transport_ = (proto_ == StreamProtocol::srt) ? make_srt_transport()
                                               : make_udp_transport();
  if (!transport_) {
    err = "srt requested but this build has no libsrt support";
    return false;
  }

  ParsedUrl url;
  if (!parse_url(p.url, url)) {
    err = "could not parse stream url: " + p.url;
    return false;
  }
  if (!transport_->open(url, err))
    return false;

  datagram_.reserve(kDatagramBytes);
  muxer_.init(p.hevc, p.has_audio, p.audio_rate, p.audio_channels,
              [this](const uint8_t *d, size_t n) { on_packet(d, n); });
  return true;
}

void TsSink::on_packet(const uint8_t *data, size_t size) {
  if (failed_)
    return;
  datagram_.insert(datagram_.end(), data, data + size);
  if (datagram_.size() >= kDatagramBytes)
    flush_datagram();
}

void TsSink::flush_datagram() {
  if (datagram_.empty())
    return;
  if (!transport_->send(datagram_.data(), datagram_.size()))
    failed_ = true;
  datagram_.clear();
}

void TsSink::write_video(const uint8_t *annexb, size_t size, bool keyframe,
                         uint64_t pts_90k) {
  if (failed_)
    return;
  muxer_.write_video(annexb, size, keyframe, pts_90k);
}

void TsSink::write_audio(const uint8_t *aac, size_t size, uint64_t pts_90k) {
  if (failed_)
    return;
  muxer_.write_audio(aac, size, pts_90k);
}

void TsSink::finish() {
  flush_datagram();
  if (transport_)
    transport_->close();
}

} // namespace frametap::enc
