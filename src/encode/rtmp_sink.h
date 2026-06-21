#pragma once

#include "encode/nal_util.h"
#include "encode/net_compat.h"
#include "encode/stream_sink.h"

#include <cstdint>
#include <string>
#include <vector>

// StreamSink that pushes an FLV stream to an RTMP ingest (e.g. nginx-rtmp,
// Twitch, YouTube). Implements the RTMP handshake, chunk protocol and the AMF0
// connect/createStream/publish exchange directly over TCP -- no librtmp/ffmpeg.
//
// H.264 uses the classic FLV AVC packaging; H.265 uses enhanced-RTMP (FourCC)
// video tags. Audio is AAC in FLV audio tags.

namespace frametap::enc {

class RtmpSink : public StreamSink {
public:
  ~RtmpSink() override;

  bool start(const StreamSinkParams &p, std::string &err) override;
  void write_video(const uint8_t *annexb, size_t size, bool keyframe,
                   uint64_t pts_90k) override;
  void write_audio(const uint8_t *aac, size_t size, uint64_t pts_90k) override;
  void finish() override;
  bool failed() const override { return failed_; }

private:
  bool handshake(std::string &err);
  bool connect_publish(std::string &err);
  bool read_until_result(double want_txn, double *out_number, std::string &err);

  // Send one RTMP message, fragmenting into chunks of out_chunk_size_.
  bool send_message(uint8_t msg_type, uint32_t msg_stream_id, uint32_t ts,
                    uint8_t csid, const uint8_t *data, size_t len);
  bool send_all(const uint8_t *data, size_t len);
  bool recv_all(uint8_t *data, size_t len);

  void send_video_seq_header();
  void send_metadata();

  socket_t fd_ = kInvalidSocket;
  bool failed_ = false;
  bool seq_sent_ = false;
  bool asc_sent_ = false;
  bool published_ = false;

  StreamSinkParams params_;
  std::string host_;
  int port_ = 1935;
  std::string app_;        // e.g. "live"
  std::string stream_key_; // e.g. "stream"
  std::string tc_url_;

  uint32_t out_chunk_size_ = 4096;
  uint32_t msg_stream_id_ = 1;

  ParamSets ps_;
  std::vector<uint8_t> vbuf_; // reused length-prefixed NAL buffer
  std::vector<uint8_t> tag_;  // reused FLV tag body
};

} // namespace frametap::enc
