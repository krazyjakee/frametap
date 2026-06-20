#include <frametap/receiving.h>

#include <frametap/frametap.h>

#include "decode/nvdec_decoder.h"
#include "decode/ts_demux.h"
#include "encode/net_transport.h"

#include <atomic>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

namespace frametap {

struct StreamReceiver::Impl {
  ReceiverConfig config;
  FrameCallback frame_cb;

  std::thread worker;
  std::atomic<bool> running{false};
  std::atomic<bool> connected{false};
  std::atomic<bool> stop_flag{false};

  mutable std::mutex mtx;          // guards error_, stats, transport
  std::string error_;
  Stats stats;
  std::unique_ptr<enc::Transport> transport;

  void set_error(const std::string &e) {
    std::lock_guard<std::mutex> lk(mtx);
    if (error_.empty())
      error_ = e;
  }

  void run();
};

void StreamReceiver::Impl::run() {
  running = true;

  enc::ParsedUrl purl;
  if (!enc::parse_url(config.url, purl)) {
    set_error("receive: invalid URL '" + config.url + "'");
    running = false;
    return;
  }
  if (purl.scheme != "srt") {
    set_error("receive: only srt:// URLs are supported");
    running = false;
    return;
  }

  {
    std::lock_guard<std::mutex> lk(mtx);
    transport = enc::make_srt_transport();
  }
  if (!transport) {
    set_error("receive: SRT support is not built into this binary");
    running = false;
    return;
  }

  std::string err;
  if (!transport->open(purl, err)) {
    // A stop() during the listener's accept() also lands here; only report a
    // genuine failure.
    if (!stop_flag)
      set_error(err);
    running = false;
    connected = false;
    return;
  }
  connected = true;

  dec::NvdecDecoder decoder;
  bool decoder_open = false;

  dec::TsDemuxer demux;
  demux.init([&](const uint8_t *d, size_t n, bool /*keyframe*/, uint64_t pts) {
    if (!config.decode)
      return;
    if (!decoder_open) {
      try {
        decoder.open(demux.is_hevc() ? dec::Codec::hevc : dec::Codec::h264,
                     [&](const uint8_t *rgba, int w, int h, uint64_t /*pts*/) {
                       ImageData img;
                       img.width = static_cast<size_t>(w);
                       img.height = static_cast<size_t>(h);
                       img.data.assign(rgba, rgba + static_cast<size_t>(w) * h *
                                                         4);
                       {
                         std::lock_guard<std::mutex> lk(mtx);
                         ++stats.frames_decoded;
                       }
                       if (frame_cb)
                         frame_cb(img);
                     });
        decoder_open = true;
      } catch (const std::exception &e) {
        set_error(e.what());
        stop_flag = true;
        return;
      }
    }
    decoder.decode(d, n, pts);
  });

  std::ofstream file;
  if (!config.out_path.empty()) {
    file.open(config.out_path, std::ios::binary | std::ios::trunc);
    if (!file)
      set_error("receive: cannot open output file '" + config.out_path + "'");
  }

  std::vector<uint8_t> buf(64 * 1024);
  while (!stop_flag) {
    const long n = transport->recv(buf.data(), buf.size());
    if (n > 0) {
      {
        std::lock_guard<std::mutex> lk(mtx);
        stats.bytes_received += static_cast<uint64_t>(n);
      }
      if (file.is_open())
        file.write(reinterpret_cast<const char *>(buf.data()), n);
      if (config.decode)
        demux.feed(buf.data(), static_cast<size_t>(n));
    } else if (n == 0) {
      break; // peer closed
    } else {
      if (!stop_flag)
        set_error("receive: transport read failed");
      break;
    }
  }

  if (config.decode) {
    demux.flush();
    if (decoder_open)
      decoder.flush();
  }
  if (file.is_open())
    file.close();

  connected = false;
  running = false;
}

StreamReceiver::StreamReceiver(ReceiverConfig config)
    : impl_(std::make_unique<Impl>()) {
  if (config.url.empty())
    throw CaptureError("StreamReceiver: empty URL");
  impl_->config = std::move(config);
}

StreamReceiver::~StreamReceiver() { stop(); }

void StreamReceiver::on_frame(FrameCallback cb) {
  impl_->frame_cb = std::move(cb);
}

void StreamReceiver::start() {
  if (impl_->running || impl_->worker.joinable())
    return;
  impl_->stop_flag = false;
  // Mark running synchronously so callers that poll running()/connected()
  // immediately after start() don't race the worker's startup and tear it down.
  impl_->running = true;
  impl_->worker = std::thread([this] { impl_->run(); });
}

void StreamReceiver::stop() {
  impl_->stop_flag = true;
  {
    // Unblock a recv()/accept() in progress on the worker thread.
    std::lock_guard<std::mutex> lk(impl_->mtx);
    if (impl_->transport)
      impl_->transport->close();
  }
  if (impl_->worker.joinable())
    impl_->worker.join();
  {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->transport.reset();
  }
}

bool StreamReceiver::running() const { return impl_->running; }
bool StreamReceiver::connected() const { return impl_->connected; }

std::string StreamReceiver::error() const {
  std::lock_guard<std::mutex> lk(impl_->mtx);
  return impl_->error_;
}

StreamReceiver::Stats StreamReceiver::stats() const {
  std::lock_guard<std::mutex> lk(impl_->mtx);
  return impl_->stats;
}

} // namespace frametap
