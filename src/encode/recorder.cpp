#include <frametap/recording.h>
#include <frametap/frametap.h> // CaptureError

#include "encode/nvenc_encoder.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <utility>

namespace frametap {

namespace {

enc::Codec to_enc_codec(Codec c) {
  return c == Codec::hevc ? enc::Codec::hevc : enc::Codec::h264;
}

// Nudges the encoder bitrate to defend the chosen target under load.
//
// Signal: an EMA of per-frame GPU encode wall time vs the frame budget
// (1000/fps ms). In fps-priority mode we shed bitrate when encode time eats
// into the budget and recover toward the configured target when there's
// headroom. In quality-priority mode bitrate is held and fps is allowed to
// dip. Resolution scaling -- the stronger lever -- is the next slice.
struct Controller {
  AdaptPriority priority = AdaptPriority::fps;
  int min_kbps = 4000;
  int target_kbps = 20000; // configured start; recovery ceiling
  int cur_kbps = 20000;
  double budget_ms = 1000.0 / 60.0;

  double ema_ms = 0;
  bool have_ema = false;
  int since_change = 0;
  static constexpr int kCooldown = 30; // frames between adjustments

  // Returns a new bitrate (kbps) if it changed this frame, else -1.
  int update(double encode_ms) {
    constexpr double alpha = 0.2;
    ema_ms = have_ema ? (alpha * encode_ms + (1.0 - alpha) * ema_ms)
                      : encode_ms;
    have_ema = true;

    if (priority != AdaptPriority::fps)
      return -1; // quality/none: bitrate is fixed

    if (++since_change < kCooldown)
      return -1;

    int next = cur_kbps;
    if (ema_ms > budget_ms * 0.85)
      next = std::max(min_kbps, cur_kbps - cur_kbps / 5); // shed 20%
    else if (ema_ms < budget_ms * 0.5)
      next = std::min(target_kbps, cur_kbps + cur_kbps / 10); // recover 10%

    if (next != cur_kbps) {
      cur_kbps = next;
      since_change = 0;
      return next;
    }
    return -1;
  }
};

} // namespace

struct VideoRecorder::Impl {
  std::string path;
  EncoderConfig config;
  std::ofstream file;
  enc::NvencEncoder encoder;
  Controller controller;

  int width = 0;
  int height = 0;
  bool finished = false;

  Stats stats;
  double encode_ms_total = 0;

  Impl(std::string p, EncoderConfig c)
      : path(std::move(p)), config(c),
        file(path, std::ios::binary | std::ios::trunc) {
    if (!file)
      throw CaptureError("VideoRecorder: cannot open output file: " + path);
    controller.priority = config.priority;
    controller.min_kbps = config.min_bitrate_kbps;
    controller.target_kbps = config.bitrate_kbps;
    controller.cur_kbps = config.bitrate_kbps;
    controller.budget_ms = 1000.0 / (config.fps > 0 ? config.fps : 60);
    stats.current_bitrate_kbps = config.bitrate_kbps;
  }

  void lazy_open(int w, int h) {
    width = w;
    height = h;
    enc::NvencParams p;
    p.codec = to_enc_codec(config.codec);
    p.width = w;
    p.height = h;
    p.fps = config.fps;
    p.bitrate_kbps = config.bitrate_kbps;

    encoder.open(p, [this](const uint8_t *data, size_t size, bool /*key*/) {
      file.write(reinterpret_cast<const char *>(data),
                 static_cast<std::streamsize>(size));
      stats.bytes_written += size;
      ++stats.frames_encoded;
    });
  }

  void submit(const Frame &frame) {
    if (finished)
      return;
    const auto &img = frame.image;
    if (img.width == 0 || img.height == 0 || img.data.empty())
      return;

    const int w = static_cast<int>(img.width);
    const int h = static_cast<int>(img.height);

    // First frame fixes the encode resolution.
    if (!encoder.is_open())
      lazy_open(w, h);
    if (w != width || h != height)
      return; // resolution change mid-stream not handled in this slice

    ++stats.frames_in;

    const auto t0 = std::chrono::steady_clock::now();
    encoder.encode(img.data.data(), w, h, static_cast<size_t>(w) * 4,
                   static_cast<int64_t>(stats.frames_in));
    const auto t1 = std::chrono::steady_clock::now();

    const double ms =
        std::chrono::duration<double, std::milli>(t1 - t0).count();
    stats.last_encode_ms = ms;
    encode_ms_total += ms;
    stats.avg_encode_ms =
        encode_ms_total / static_cast<double>(stats.frames_in);

    const int new_kbps = controller.update(ms);
    if (new_kbps > 0) {
      encoder.set_bitrate(new_kbps);
      stats.current_bitrate_kbps = new_kbps;
      ++stats.adaptations;
    }
  }

  void finish() {
    if (finished)
      return;
    finished = true;
    if (encoder.is_open()) {
      encoder.flush();
      encoder.close();
    }
    file.flush();
    file.close();
  }
};

VideoRecorder::VideoRecorder(std::string out_path, EncoderConfig config)
    : impl_(std::make_unique<Impl>(std::move(out_path), config)) {}

VideoRecorder::~VideoRecorder() {
  if (impl_)
    impl_->finish();
}

VideoRecorder::VideoRecorder(VideoRecorder &&) noexcept = default;
VideoRecorder &VideoRecorder::operator=(VideoRecorder &&) noexcept = default;

void VideoRecorder::submit(const Frame &frame) { impl_->submit(frame); }
void VideoRecorder::finish() { impl_->finish(); }
VideoRecorder::Stats VideoRecorder::stats() const { return impl_->stats; }

} // namespace frametap
