#pragma once

#include "encode/net_transport.h"
#include "encode/stream_sink.h"
#include "encode/ts_muxer.h"

#include <frametap/recording.h> // StreamProtocol

#include <memory>
#include <vector>

// StreamSink for MPEG-TS over UDP or SRT. The TsMuxer produces 188-byte TS
// packets; this batches 7 of them into a 1316-byte datagram (the conventional
// MPEG-TS payload size) and pushes each over the chosen transport.

namespace frametap::enc {

class TsSink : public StreamSink {
public:
  explicit TsSink(StreamProtocol proto) : proto_(proto) {}

  bool start(const StreamSinkParams &p, std::string &err) override;
  void write_video(const uint8_t *annexb, size_t size, bool keyframe,
                   uint64_t pts_90k) override;
  void write_audio(const uint8_t *aac, size_t size, uint64_t pts_90k) override;
  void finish() override;
  bool failed() const override { return failed_; }

private:
  void on_packet(const uint8_t *data, size_t size);
  void flush_datagram();

  StreamProtocol proto_;
  std::unique_ptr<Transport> transport_;
  TsMuxer muxer_;
  std::vector<uint8_t> datagram_;
  bool failed_ = false;
};

} // namespace frametap::enc
