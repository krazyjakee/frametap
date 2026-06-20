#include "encode/rtmp_sink.h"

#include "encode/net_compat.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace frametap::enc {
namespace {

void amf_str(std::vector<uint8_t> &v, const std::string &s) {
  v.push_back(0x02);
  v.push_back(static_cast<uint8_t>(s.size() >> 8));
  v.push_back(static_cast<uint8_t>(s.size()));
  v.insert(v.end(), s.begin(), s.end());
}
void amf_key(std::vector<uint8_t> &v, const std::string &s) {
  v.push_back(static_cast<uint8_t>(s.size() >> 8));
  v.push_back(static_cast<uint8_t>(s.size()));
  v.insert(v.end(), s.begin(), s.end());
}
void amf_num(std::vector<uint8_t> &v, double d) {
  v.push_back(0x00);
  uint64_t u;
  std::memcpy(&u, &d, 8);
  for (int i = 7; i >= 0; --i)
    v.push_back(static_cast<uint8_t>(u >> (i * 8)));
}
void amf_null(std::vector<uint8_t> &v) { v.push_back(0x05); }
void amf_obj_end(std::vector<uint8_t> &v) {
  v.push_back(0x00);
  v.push_back(0x00);
  v.push_back(0x09);
}

double read_be_double(const uint8_t *p) {
  uint64_t u = 0;
  for (int i = 0; i < 8; ++i)
    u = (u << 8) | p[i];
  double d;
  std::memcpy(&d, &u, 8);
  return d;
}

// Advance `pos` past one AMF0 value. Returns false if malformed/truncated.
bool amf_skip(const std::vector<uint8_t> &b, size_t &pos) {
  if (pos >= b.size())
    return false;
  uint8_t marker = b[pos++];
  switch (marker) {
  case 0x00: // number
    pos += 8;
    return pos <= b.size();
  case 0x01: // boolean
    pos += 1;
    return pos <= b.size();
  case 0x02: { // string
    if (pos + 2 > b.size())
      return false;
    size_t len = (b[pos] << 8) | b[pos + 1];
    pos += 2 + len;
    return pos <= b.size();
  }
  case 0x05: // null
  case 0x06: // undefined
    return true;
  case 0x03:    // object
  case 0x08: {  // ecma array
    if (marker == 0x08)
      pos += 4; // associative-count (advisory)
    while (pos + 2 <= b.size()) {
      size_t klen = (b[pos] << 8) | b[pos + 1];
      pos += 2 + klen;
      if (klen == 0) { // object end marker follows
        if (pos < b.size() && b[pos] == 0x09) {
          pos += 1;
          return true;
        }
        return false;
      }
      if (!amf_skip(b, pos))
        return false;
    }
    return false;
  }
  default:
    return false;
  }
}

} // namespace

RtmpSink::~RtmpSink() { finish(); }

bool RtmpSink::send_all(const uint8_t *data, size_t len) {
  size_t off = 0;
  while (off < len) {
    long n = sock_send(fd_, data + off, len - off);
    if (n <= 0)
      return false;
    off += static_cast<size_t>(n);
  }
  return true;
}

bool RtmpSink::recv_all(uint8_t *data, size_t len) {
  size_t off = 0;
  while (off < len) {
    long n = sock_recv(fd_, data + off, len - off);
    if (n <= 0)
      return false;
    off += static_cast<size_t>(n);
  }
  return true;
}

bool RtmpSink::handshake(std::string &err) {
  uint8_t c0c1[1 + 1536];
  c0c1[0] = 0x03; // RTMP version
  std::memset(c0c1 + 1, 0, 8); // time + zero
  for (int i = 9; i < 1 + 1536; ++i)
    c0c1[i] = static_cast<uint8_t>(std::rand() & 0xFF);
  if (!send_all(c0c1, sizeof(c0c1))) {
    err = "rtmp: handshake send failed";
    return false;
  }
  uint8_t s0s1s2[1 + 1536 + 1536];
  if (!recv_all(s0s1s2, sizeof(s0s1s2))) {
    err = "rtmp: handshake recv failed";
    return false;
  }
  // C2 echoes S1.
  if (!send_all(s0s1s2 + 1, 1536)) {
    err = "rtmp: handshake C2 failed";
    return false;
  }
  return true;
}

bool RtmpSink::send_message(uint8_t msg_type, uint32_t msg_stream_id,
                            uint32_t ts, uint8_t csid, const uint8_t *data,
                            size_t len) {
  std::vector<uint8_t> buf;
  const bool ext = ts >= 0xFFFFFF;
  buf.push_back(static_cast<uint8_t>(csid & 0x3F)); // fmt 0
  const uint32_t t3 = ext ? 0xFFFFFF : ts;
  buf.push_back((t3 >> 16) & 0xFF);
  buf.push_back((t3 >> 8) & 0xFF);
  buf.push_back(t3 & 0xFF);
  buf.push_back((len >> 16) & 0xFF);
  buf.push_back((len >> 8) & 0xFF);
  buf.push_back(len & 0xFF);
  buf.push_back(msg_type);
  buf.push_back(msg_stream_id & 0xFF); // little-endian
  buf.push_back((msg_stream_id >> 8) & 0xFF);
  buf.push_back((msg_stream_id >> 16) & 0xFF);
  buf.push_back((msg_stream_id >> 24) & 0xFF);
  if (ext) {
    buf.push_back((ts >> 24) & 0xFF);
    buf.push_back((ts >> 16) & 0xFF);
    buf.push_back((ts >> 8) & 0xFF);
    buf.push_back(ts & 0xFF);
  }

  size_t off = 0;
  size_t first = std::min(len, static_cast<size_t>(out_chunk_size_));
  buf.insert(buf.end(), data, data + first);
  off = first;
  while (off < len) {
    buf.push_back(static_cast<uint8_t>(0xC0 | (csid & 0x3F))); // fmt 3
    if (ext) {
      buf.push_back((ts >> 24) & 0xFF);
      buf.push_back((ts >> 16) & 0xFF);
      buf.push_back((ts >> 8) & 0xFF);
      buf.push_back(ts & 0xFF);
    }
    size_t take = std::min(len - off, static_cast<size_t>(out_chunk_size_));
    buf.insert(buf.end(), data + off, data + off + take);
    off += take;
  }
  return send_all(buf.data(), buf.size());
}

// Minimal chunk de-multiplexer: returns one fully-assembled message.
namespace {
struct CsState {
  uint32_t ts = 0, length = 0, stream_id = 0;
  uint8_t type = 0;
  bool ext = false;
  std::vector<uint8_t> payload;
};
} // namespace

bool RtmpSink::read_until_result(double want_txn, double *out_number,
                                 std::string &err) {
  std::map<int, CsState> cs;
  uint32_t in_chunk = 128;

  for (int guard = 0; guard < 4096; ++guard) {
    uint8_t b0;
    if (!recv_all(&b0, 1)) {
      err = "rtmp: connection closed during handshake exchange";
      return false;
    }
    int fmt = b0 >> 6;
    int csid = b0 & 0x3F;
    if (csid == 0) {
      uint8_t e;
      if (!recv_all(&e, 1)) return false;
      csid = 64 + e;
    } else if (csid == 1) {
      uint8_t e[2];
      if (!recv_all(e, 2)) return false;
      csid = 64 + e[0] + e[1] * 256;
    }
    CsState &st = cs[csid];

    auto read_ts3 = [&](uint32_t &ts) -> bool {
      uint8_t h[3];
      if (!recv_all(h, 3)) return false;
      ts = (h[0] << 16) | (h[1] << 8) | h[2];
      return true;
    };

    if (fmt <= 2) {
      uint32_t tsfield = 0;
      if (!read_ts3(tsfield)) return false;
      if (fmt <= 1) {
        uint8_t h[4];
        if (!recv_all(h, 4)) return false; // length(3) + type(1)
        st.length = (h[0] << 16) | (h[1] << 8) | h[2];
        st.type = h[3];
        if (fmt == 0) {
          uint8_t s[4];
          if (!recv_all(s, 4)) return false; // stream id (LE)
          st.stream_id = s[0] | (s[1] << 8) | (s[2] << 16) | (s[3] << 24);
        }
      }
      st.ext = (tsfield == 0xFFFFFF);
      if (st.ext) {
        uint8_t e[4];
        if (!recv_all(e, 4)) return false;
        tsfield = (e[0] << 24) | (e[1] << 16) | (e[2] << 8) | e[3];
      }
      st.ts = (fmt == 0) ? tsfield : st.ts + tsfield;
      st.payload.clear();
    } else { // fmt == 3
      if (st.ext) {
        uint8_t e[4];
        if (!recv_all(e, 4)) return false; // repeated extended timestamp
      }
    }

    size_t remaining = st.length - st.payload.size();
    size_t take = std::min(remaining, static_cast<size_t>(in_chunk));
    size_t old = st.payload.size();
    st.payload.resize(old + take);
    if (!recv_all(st.payload.data() + old, take))
      return false;
    if (st.payload.size() < st.length)
      continue; // message not complete yet

    const uint8_t type = st.type;
    std::vector<uint8_t> msg = std::move(st.payload);
    st.payload.clear();

    if (type == 0x01 && msg.size() >= 4) { // Set Chunk Size
      in_chunk = (msg[0] << 24) | (msg[1] << 16) | (msg[2] << 8) | msg[3];
      if (in_chunk == 0)
        in_chunk = 128;
      continue;
    }
    if (type != 0x14) // not an AMF0 command
      continue;

    size_t pos = 0;
    if (pos >= msg.size() || msg[pos] != 0x02)
      continue;
    pos++;
    size_t nlen = (msg[pos] << 8) | msg[pos + 1];
    pos += 2;
    std::string name(reinterpret_cast<char *>(msg.data() + pos), nlen);
    pos += nlen;
    if (pos >= msg.size() || msg[pos] != 0x00)
      continue;
    double txn = read_be_double(msg.data() + pos + 1);
    pos += 9;

    if (name == "_error") {
      err = "rtmp: server returned _error";
      return false;
    }
    if (name == "_result" && txn == want_txn) {
      if (out_number) {
        amf_skip(msg, pos); // command object / null
        if (pos < msg.size() && msg[pos] == 0x00)
          *out_number = read_be_double(msg.data() + pos + 1);
      }
      return true;
    }
  }
  err = "rtmp: no _result from server";
  return false;
}

bool RtmpSink::connect_publish(std::string &err) {
  out_chunk_size_ = 128; // until we raise it below

  // connect
  {
    std::vector<uint8_t> cmd;
    amf_str(cmd, "connect");
    amf_num(cmd, 1);
    cmd.push_back(0x03); // command object
    amf_key(cmd, "app");
    amf_str(cmd, app_);
    amf_key(cmd, "type");
    amf_str(cmd, "nonprivate");
    amf_key(cmd, "flashVer");
    amf_str(cmd, "FMLE/3.0 (compatible; frametap)");
    amf_key(cmd, "tcUrl");
    amf_str(cmd, tc_url_);
    amf_obj_end(cmd);
    if (!send_message(0x14, 0, 0, 3, cmd.data(), cmd.size())) {
      err = "rtmp: connect send failed";
      return false;
    }
  }
  if (!read_until_result(1, nullptr, err))
    return false;

  // Raise our outbound chunk size.
  {
    uint8_t cs[4] = {0, 0, 0x10, 0x00}; // 4096
    if (!send_message(0x01, 0, 0, 2, cs, 4)) {
      err = "rtmp: set chunk size failed";
      return false;
    }
    out_chunk_size_ = 4096;
  }

  // createStream
  {
    std::vector<uint8_t> cmd;
    amf_str(cmd, "createStream");
    amf_num(cmd, 2);
    amf_null(cmd);
    if (!send_message(0x14, 0, 0, 3, cmd.data(), cmd.size())) {
      err = "rtmp: createStream send failed";
      return false;
    }
  }
  double stream_id = 1;
  if (!read_until_result(2, &stream_id, err))
    return false;
  msg_stream_id_ = static_cast<uint32_t>(stream_id);

  // publish
  {
    std::vector<uint8_t> cmd;
    amf_str(cmd, "publish");
    amf_num(cmd, 0);
    amf_null(cmd);
    amf_str(cmd, stream_key_);
    amf_str(cmd, "live");
    if (!send_message(0x14, msg_stream_id_, 0, 3, cmd.data(), cmd.size())) {
      err = "rtmp: publish send failed";
      return false;
    }
  }
  send_metadata();
  published_ = true;
  return true;
}

void RtmpSink::send_metadata() {
  std::vector<uint8_t> d;
  amf_str(d, "@setDataFrame");
  amf_str(d, "onMetaData");
  d.push_back(0x08); // ecma array
  d.push_back(0);
  d.push_back(0);
  d.push_back(0);
  d.push_back(5); // entry count (advisory)
  amf_key(d, "width");
  amf_num(d, params_.width);
  amf_key(d, "height");
  amf_num(d, params_.height);
  amf_key(d, "framerate");
  amf_num(d, params_.fps);
  amf_key(d, "videocodecid");
  amf_num(d, params_.hevc ? 12 : 7);
  amf_key(d, "audiocodecid");
  amf_num(d, 10);
  amf_obj_end(d);
  send_message(0x12, msg_stream_id_, 0, 5, d.data(), d.size());
}

void RtmpSink::send_video_seq_header() {
  tag_.clear();
  if (!params_.hevc) {
    std::vector<uint8_t> cfg = build_avcc(ps_);
    tag_.push_back(0x17); // keyframe + AVC
    tag_.push_back(0x00); // AVC sequence header
    tag_.push_back(0);
    tag_.push_back(0);
    tag_.push_back(0); // composition time
    tag_.insert(tag_.end(), cfg.begin(), cfg.end());
  } else {
    std::vector<uint8_t> cfg = build_hvcc(ps_);
    tag_.push_back(0x90); // ex-header | keyframe | PacketTypeSequenceStart
    tag_.push_back('h');
    tag_.push_back('v');
    tag_.push_back('c');
    tag_.push_back('1');
    tag_.insert(tag_.end(), cfg.begin(), cfg.end());
  }
  send_message(0x09, msg_stream_id_, 0, 6, tag_.data(), tag_.size());
}

bool RtmpSink::start(const StreamSinkParams &p, std::string &err) {
  params_ = p;

  // Parse rtmp://host[:port]/app/stream_key
  std::string u = p.url;
  const std::string scheme = "rtmp://";
  if (u.compare(0, scheme.size(), scheme) != 0) {
    err = "rtmp: url must start with rtmp://";
    return false;
  }
  u = u.substr(scheme.size());
  size_t slash = u.find('/');
  std::string authority = u.substr(0, slash);
  std::string path = (slash == std::string::npos) ? "" : u.substr(slash + 1);
  size_t colon = authority.find(':');
  if (colon == std::string::npos) {
    host_ = authority;
    port_ = 1935;
  } else {
    host_ = authority.substr(0, colon);
    port_ = std::atoi(authority.substr(colon + 1).c_str());
  }
  size_t pslash = path.find('/');
  if (pslash == std::string::npos) {
    app_ = path;
    stream_key_ = "stream";
  } else {
    app_ = path.substr(0, pslash);
    stream_key_ = path.substr(pslash + 1);
  }
  tc_url_ = scheme + authority + "/" + app_;

  // TCP connect.
  net_global_init();
  char port[16];
  std::snprintf(port, sizeof(port), "%d", port_);
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo *res = nullptr;
  if (int r = getaddrinfo(host_.c_str(), port, &hints, &res); r != 0) {
    err = std::string("rtmp: getaddrinfo: ") + gai_strerror(r);
    return false;
  }
  for (addrinfo *ai = res; ai; ai = ai->ai_next) {
    fd_ = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd_ == kInvalidSocket)
      continue;
    if (::connect(fd_, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0)
      break;
    close_socket(fd_);
    fd_ = kInvalidSocket;
  }
  freeaddrinfo(res);
  if (fd_ == kInvalidSocket) {
    err = "rtmp: cannot connect to " + host_ + ":" + port;
    return false;
  }

  if (!handshake(err))
    return false;
  if (!connect_publish(err))
    return false;

  // Seed parameter sets from extradata if present (in-band sets at the first
  // keyframe will fill any gaps).
  if (!params_.extradata.empty()) {
    std::vector<uint8_t> ignore;
    annexb_to_length_prefixed(params_.extradata.data(),
                              params_.extradata.size(), params_.hevc, ps_,
                              ignore);
  }
  return true;
}

void RtmpSink::write_video(const uint8_t *annexb, size_t size, bool keyframe,
                           uint64_t pts_90k) {
  if (failed_ || fd_ == kInvalidSocket)
    return;
  vbuf_.clear();
  annexb_to_length_prefixed(annexb, size, params_.hevc, ps_, vbuf_);

  if (!seq_sent_ && ps_.complete(params_.hevc)) {
    send_video_seq_header();
    seq_sent_ = true;
  }
  if (!seq_sent_ || vbuf_.empty())
    return;

  const uint32_t ts = static_cast<uint32_t>(pts_90k / 90);
  tag_.clear();
  if (!params_.hevc) {
    tag_.push_back(static_cast<uint8_t>((keyframe ? 0x10 : 0x20) | 0x07));
    tag_.push_back(0x01); // NALU
    tag_.push_back(0);
    tag_.push_back(0);
    tag_.push_back(0); // composition time
  } else {
    tag_.push_back(static_cast<uint8_t>(0x80 | (keyframe ? 0x10 : 0x20) | 0x01));
    tag_.push_back('h');
    tag_.push_back('v');
    tag_.push_back('c');
    tag_.push_back('1');
    tag_.push_back(0);
    tag_.push_back(0);
    tag_.push_back(0); // composition time
  }
  tag_.insert(tag_.end(), vbuf_.begin(), vbuf_.end());
  if (!send_message(0x09, msg_stream_id_, ts, 6, tag_.data(), tag_.size()))
    failed_ = true;
}

void RtmpSink::write_audio(const uint8_t *aac, size_t size, uint64_t pts_90k) {
  if (failed_ || fd_ == kInvalidSocket || !params_.has_audio || size == 0)
    return;
  const uint32_t ts = static_cast<uint32_t>(pts_90k / 90);

  if (!asc_sent_) {
    tag_.clear();
    tag_.push_back(0xAF); // AAC, 44k/16-bit/stereo (ignored for AAC)
    tag_.push_back(0x00); // AAC sequence header
    tag_.insert(tag_.end(), params_.asc.begin(), params_.asc.end());
    send_message(0x08, msg_stream_id_, 0, 4, tag_.data(), tag_.size());
    asc_sent_ = true;
  }

  tag_.clear();
  tag_.push_back(0xAF);
  tag_.push_back(0x01); // AAC raw
  tag_.insert(tag_.end(), aac, aac + size);
  if (!send_message(0x08, msg_stream_id_, ts, 4, tag_.data(), tag_.size()))
    failed_ = true;
}

void RtmpSink::finish() {
  // Politely unpublish so the server doesn't log a broken-pipe demux error.
  if (fd_ != kInvalidSocket && published_ && !failed_) {
    std::vector<uint8_t> cmd;
    amf_str(cmd, "FCUnpublish");
    amf_num(cmd, 0);
    amf_null(cmd);
    amf_str(cmd, stream_key_);
    send_message(0x14, msg_stream_id_, 0, 3, cmd.data(), cmd.size());

    cmd.clear();
    amf_str(cmd, "deleteStream");
    amf_num(cmd, 0);
    amf_null(cmd);
    amf_num(cmd, msg_stream_id_);
    send_message(0x14, 0, 0, 3, cmd.data(), cmd.size());
    published_ = false;
  }
  if (fd_ != kInvalidSocket) {
    close_socket(fd_);
    fd_ = kInvalidSocket;
  }
}

} // namespace frametap::enc
