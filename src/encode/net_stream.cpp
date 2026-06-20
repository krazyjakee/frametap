#include "encode/net_stream.h"

#include "encode/rtmp_sink.h"
#include "encode/ts_sink.h"

#include <vector>

namespace frametap::enc {

namespace {

// Backpressure tuning. The producer keeps roughly one GOP buffered while the
// stream isn't draining (e.g. a viewer hasn't connected yet); on a new keyframe
// it discards anything older so memory stays bounded and a late viewer joins at
// a fresh decodable point. The hard cap is a final safety valve.
constexpr size_t kSoftCapPackets = 256;
constexpr size_t kHardCapPackets = 1024;

// AAC-LC frame size, in samples. The recorder feeds one such frame per call.
constexpr uint64_t kAacFrameSamples = 1024;

// Give up waiting for audio after this many video frames if no AAC config has
// arrived (audio capture started but produced nothing), and stream video-only.
constexpr uint64_t kAudioWaitFrames = 150;

int nal_type(uint8_t b, bool hevc) {
  return hevc ? (b >> 1) & 0x3F : b & 0x1F;
}

bool is_param_set(int t, bool hevc) {
  if (hevc)
    return t == 32 || t == 33 || t == 34; // VPS, SPS, PPS
  return t == 7 || t == 8;                // SPS, PPS
}

} // namespace

NetworkStreamer::NetworkStreamer() = default;

NetworkStreamer::~NetworkStreamer() { close(); }

std::string NetworkStreamer::last_error() const {
  std::lock_guard<std::mutex> lk(m_);
  return error_;
}

void NetworkStreamer::set_error(const std::string &msg) {
  std::lock_guard<std::mutex> lk(m_);
  if (error_.empty())
    error_ = msg;
  failed_ = true;
}

void NetworkStreamer::open(StreamProtocol proto, const std::string &url,
                           bool hevc, int width, int height, int fps) {
  if (started_)
    return;
  proto_ = proto;
  url_ = url;
  hevc_ = hevc;
  width_ = width;
  height_ = height;
  fps_ = fps > 0 ? fps : 60;
  started_ = true;
  worker_ = std::thread([this] { run(); });
}

void NetworkStreamer::extract_extradata(const uint8_t *data, size_t size) {
  // Collect parameter-set NALs (with 4-byte start codes) so the sink has the
  // decoder configuration before the first frame.
  std::vector<uint8_t> out;
  size_t i = 0;
  auto start_code = [&](size_t p, size_t &len) -> bool {
    if (p + 3 < size && data[p] == 0 && data[p + 1] == 0 && data[p + 2] == 0 &&
        data[p + 3] == 1) {
      len = 4;
      return true;
    }
    if (p + 2 < size && data[p] == 0 && data[p + 1] == 0 && data[p + 2] == 1) {
      len = 3;
      return true;
    }
    return false;
  };

  while (i < size) {
    size_t sc = 0;
    if (!start_code(i, sc)) {
      ++i;
      continue;
    }
    size_t nal_start = i + sc;
    if (nal_start >= size)
      break;
    size_t j = nal_start;
    size_t next = size;
    while (j < size) {
      size_t sc2 = 0;
      if (start_code(j, sc2)) {
        next = j;
        break;
      }
      ++j;
    }
    const int t = nal_type(data[nal_start], hevc_);
    if (is_param_set(t, hevc_)) {
      static const uint8_t sc4[4] = {0, 0, 0, 1};
      out.insert(out.end(), sc4, sc4 + 4);
      out.insert(out.end(), data + nal_start, data + next);
    }
    i = next;
  }

  if (!out.empty()) {
    extradata_ = std::move(out);
    have_extradata_ = true;
  }
}

void NetworkStreamer::write_access_unit(const uint8_t *data, size_t size,
                                        bool keyframe, uint64_t pts_90k) {
  if (!started_ || size == 0)
    return;
  std::lock_guard<std::mutex> lk(m_);
  if (failed_ || eos_)
    return;

  if (!have_extradata_)
    extract_extradata(data, size);

  if (!audio_decided_ && ++video_seen_ >= kAudioWaitFrames) {
    has_audio_ = false;
    audio_decided_ = true;
  }

  // Bound memory while the queue isn't draining or under congestion: a fresh
  // keyframe is a clean restart point, so drop everything before it.
  if (keyframe && (!streaming_ || q_.size() > kSoftCapPackets))
    q_.clear();
  while (q_.size() > kHardCapPackets)
    q_.pop_front();

  Packet p;
  p.audio = false;
  p.keyframe = keyframe;
  p.pts = pts_90k;
  p.data.assign(data, data + size);
  q_.push_back(std::move(p));
  cv_.notify_all();
}

void NetworkStreamer::set_audio(int sample_rate, int channels,
                                const uint8_t *asc, size_t asc_len,
                                uint64_t start_delay_90k) {
  if (!started_)
    return;
  std::lock_guard<std::mutex> lk(m_);
  if (audio_decided_)
    return;
  audio_rate_ = sample_rate > 0 ? sample_rate : 48000;
  audio_channels_ = channels > 0 ? channels : 2;
  audio_start_delay_90k_ = start_delay_90k;
  asc_.assign(asc, asc + asc_len);
  has_audio_ = true;
  audio_decided_ = true;
  cv_.notify_all();
}

void NetworkStreamer::no_audio() {
  if (!started_)
    return;
  std::lock_guard<std::mutex> lk(m_);
  if (audio_decided_)
    return;
  has_audio_ = false;
  audio_decided_ = true;
  cv_.notify_all();
}

void NetworkStreamer::write_audio_sample(const uint8_t *data, size_t size) {
  if (!started_ || size == 0)
    return;
  std::lock_guard<std::mutex> lk(m_);
  if (failed_ || eos_ || !has_audio_)
    return;
  while (q_.size() > kHardCapPackets)
    q_.pop_front();
  Packet p;
  p.audio = true;
  p.pts = audio_start_delay_90k_ +
          audio_samples_ * 90000 / static_cast<uint64_t>(audio_rate_);
  audio_samples_ += kAacFrameSamples;
  p.data.assign(data, data + size);
  q_.push_back(std::move(p));
  cv_.notify_all();
}

void NetworkStreamer::run() {
  StreamSinkParams params;
  {
    std::unique_lock<std::mutex> lk(m_);
    cv_.wait(lk, [&] { return (have_extradata_ && audio_decided_) || eos_; });
    if (!have_extradata_)
      return; // nothing was ever encoded
    params.url = url_;
    params.hevc = hevc_;
    params.width = width_;
    params.height = height_;
    params.fps = fps_;
    params.extradata = extradata_;
    params.has_audio = has_audio_;
    params.audio_rate = audio_rate_;
    params.audio_channels = audio_channels_;
    params.asc = asc_;
    params.cancel = &cancel_;
  }

  if (proto_ == StreamProtocol::rtmp)
    sink_ = std::make_unique<RtmpSink>();
  else
    sink_ = std::make_unique<TsSink>(proto_);

  std::string err;
  if (!sink_->start(params, err)) {
    // A cancelled accept (close() during the wait for a viewer) is not an
    // error; only report genuine transport failures.
    if (!cancel_.load() && !err.empty())
      set_error(err);
    sink_.reset();
    return;
  }

  {
    std::lock_guard<std::mutex> lk(m_);
    streaming_ = true;
  }

  for (;;) {
    Packet p;
    {
      std::unique_lock<std::mutex> lk(m_);
      cv_.wait(lk, [&] { return !q_.empty() || eos_; });
      if (q_.empty()) {
        if (eos_)
          break;
        continue;
      }
      p = std::move(q_.front());
      q_.pop_front();
    }
    if (p.audio)
      sink_->write_audio(p.data.data(), p.data.size(), p.pts);
    else
      sink_->write_video(p.data.data(), p.data.size(), p.keyframe, p.pts);
    if (sink_->failed()) {
      set_error("stream transport failed");
      break;
    }
  }

  sink_->finish();
  sink_.reset();
}

void NetworkStreamer::close() {
  if (!started_)
    return;
  // cancel_ unblocks a worker waiting in the SRT listener accept; eos_ unblocks
  // the queue wait and ends the drain loop. Both are needed so join() can't hang
  // whether or not a viewer ever connected.
  cancel_.store(true);
  {
    std::lock_guard<std::mutex> lk(m_);
    eos_ = true;
    cv_.notify_all();
  }
  if (worker_.joinable())
    worker_.join();
  started_ = false;
}

} // namespace frametap::enc
