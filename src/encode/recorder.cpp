#include <frametap/recording.h>
#include <frametap/frametap.h> // CaptureError

#include "audio/pw_capture.h"
#include "encode/aac_encoder.h"
#include "encode/mp4_muxer.h"
#include "encode/net_stream.h"
#include "encode/nvenc_encoder.h"

#include <cstdio>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <utility>

namespace frametap {

namespace {

enc::Codec to_enc_codec(Codec c) {
  return c == Codec::hevc ? enc::Codec::hevc : enc::Codec::h264;
}

namespace fs = std::filesystem;

std::string home_dir() {
#ifdef _WIN32
  if (const char *p = std::getenv("USERPROFILE"))
    return p;
  const char *drive = std::getenv("HOMEDRIVE");
  const char *path = std::getenv("HOMEPATH");
  if (drive && path)
    return std::string(drive) + path;
#else
  if (const char *p = std::getenv("HOME"))
    return p;
#endif
  return {};
}

#if !defined(__APPLE__) && !defined(_WIN32)
// Resolve $XDG_VIDEOS_DIR, then ~/.config/user-dirs.dirs, expanding $HOME.
std::string xdg_videos_dir() {
  if (const char *v = std::getenv("XDG_VIDEOS_DIR"); v && *v)
    return v;

  std::string config;
  if (const char *c = std::getenv("XDG_CONFIG_HOME"); c && *c)
    config = c;
  else if (std::string h = home_dir(); !h.empty())
    config = h + "/.config";
  if (config.empty())
    return {};

  std::ifstream f(config + "/user-dirs.dirs");
  std::string line;
  while (std::getline(f, line)) {
    if (line.find("XDG_VIDEOS_DIR") == std::string::npos)
      continue;
    const auto q1 = line.find('"');
    const auto q2 = (q1 == std::string::npos) ? q1 : line.find('"', q1 + 1);
    if (q1 == std::string::npos || q2 == std::string::npos)
      continue;
    std::string val = line.substr(q1 + 1, q2 - q1 - 1);
    if (const std::string tok = "$HOME"; val.compare(0, tok.size(), tok) == 0)
      val = home_dir() + val.substr(tok.size());
    return val;
  }
  return {};
}
#endif

// Platform "Videos"/"Movies" base directory (no Screencasts suffix).
std::string videos_base_dir() {
  const std::string h = home_dir();
#if defined(__APPLE__)
  return h.empty() ? std::string() : h + "/Movies";
#elif defined(_WIN32)
  return h.empty() ? std::string() : h + "\\Videos";
#else
  if (std::string v = xdg_videos_dir(); !v.empty())
    return v;
  return h.empty() ? std::string() : h + "/Videos";
#endif
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
  enc::NvencEncoder encoder;
  enc::Mp4Muxer muxer;
  Controller controller;

  // Whether a local .mp4 is written. False = stream only.
  bool save_file = true;
  // Optional network sink, created when config.stream.enabled.
  std::unique_ptr<enc::NetworkStreamer> stream;

  // System-audio capture -> AAC -> muxer audio track. Best-effort: if any of
  // this fails the recording continues video-only.
  audio::PwCapture audio;
  enc::AacEncoder aac;
  bool audio_inited = false;

  int width = 0;
  int height = 0;
  bool finished = false;

  // Wall-clock presentation time of the current frame (90 kHz, first frame =
  // 0), so the muxer can tag real per-frame durations and keep A/V in sync.
  std::chrono::steady_clock::time_point start_tp;
  bool have_start = false;
  uint64_t cur_pts_90k = 0;

  // submit() runs on the FrameTap capture thread, so a thrown encoder error
  // (e.g. NVENC rejecting a frame larger than the codec's max dimension) would
  // otherwise escape the thread and call std::terminate(). We stash it here and
  // rethrow from finish(), which the owning thread calls.
  std::mutex error_mutex;
  std::exception_ptr error;

  Stats stats;
  double encode_ms_total = 0;

  Impl(std::string p, EncoderConfig c) : path(std::move(p)), config(c) {
    save_file = !config.stream.enabled || config.stream.also_save_file;
    if (config.stream.enabled)
      stream = std::make_unique<enc::NetworkStreamer>();

    // Verify the path is writable up front; the muxer (re)opens it lazily once
    // frame dimensions are known.
    if (save_file) {
      std::ofstream probe(path, std::ios::binary | std::ios::trunc);
      if (!probe)
        throw CaptureError("VideoRecorder: cannot open output file: " + path);
    }
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
    const bool hevc = config.codec == Codec::hevc;
    if (save_file)
      muxer.open(path, hevc, w, h, config.fps);
    if (stream)
      stream->open(config.stream.protocol, config.stream.url, hevc, w, h,
                   config.fps);

    enc::NvencParams p;
    p.codec = to_enc_codec(config.codec);
    p.width = w;
    p.height = h;
    p.fps = config.fps;
    p.bitrate_kbps = config.bitrate_kbps;

    encoder.open(p, [this](const uint8_t *data, size_t size, bool key) {
      if (save_file)
        muxer.write_access_unit(data, size, key, cur_pts_90k);
      if (stream)
        stream->write_access_unit(data, size, key, cur_pts_90k);
      stats.bytes_written += size;
      ++stats.frames_encoded;
    });

    // Start system-audio capture. Non-fatal on failure (e.g. no PipeWire).
    try {
      audio.start(
          [this](const float *pcm, uint32_t frames) { on_pcm(pcm, frames); },
          config.audio_source_pid);
    } catch (const std::exception &e) {
      std::fprintf(stderr, "frametap: audio capture unavailable (%s); "
                           "recording video only\n",
                   e.what());
      if (stream)
        stream->no_audio(); // don't make the stream wait for audio that won't come
    }
  }

  // Runs on the PipeWire capture thread.
  void on_pcm(const float *pcm, uint32_t frames) {
    try {
      if (!audio_inited) {
        aac.open(audio.rate(), audio.channels(), 160000,
                 [this](const uint8_t *data, size_t size) {
                   if (save_file)
                     muxer.write_audio_sample(data, size);
                   if (stream)
                     stream->write_audio_sample(data, size);
                 });
        if (save_file)
          muxer.set_audio(aac.sample_rate(), aac.channels(), aac.asc().data(),
                          aac.asc().size(), 1024);
        if (stream)
          stream->set_audio(aac.sample_rate(), aac.channels(),
                            aac.asc().data(), aac.asc().size());
        audio_inited = true;
      }
      aac.encode(pcm, frames);
    } catch (...) {
      // Swallow on the audio thread; video recording is unaffected.
    }
  }

  void submit(const Frame &frame) {
    if (finished)
      return;
    try {
      do_submit(frame);
    } catch (...) {
      std::lock_guard<std::mutex> lock(error_mutex);
      if (!error)
        error = std::current_exception();
      finished = true; // stop encoding; the error is surfaced by finish()
    }
  }

  void do_submit(const Frame &frame) {
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

    // Presentation time of this frame relative to the first one.
    const auto now = std::chrono::steady_clock::now();
    if (!have_start) {
      start_tp = now;
      have_start = true;
    }
    cur_pts_90k = static_cast<uint64_t>(
        std::chrono::duration<double>(now - start_tp).count() * 90000.0);

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
    // Stop audio first so no PCM arrives while we drain, then flush the AAC
    // tail into the muxer's audio buffer before the moov is written.
    audio.stop();
    if (aac.is_open()) {
      aac.flush();
      aac.close();
    }
    if (encoder.is_open()) {
      encoder.flush();
      encoder.close();
    }
    if (save_file)
      muxer.close();
    if (stream)
      stream->close();
  }

  // Rethrows any error captured on the capture thread. Called from the public
  // finish() (owning thread), never from the destructor.
  void rethrow_error() {
    std::exception_ptr e;
    {
      std::lock_guard<std::mutex> lock(error_mutex);
      e = error;
      error = nullptr;
    }
    if (e)
      std::rethrow_exception(e);
  }
};

std::string default_recording_dir() {
  const std::string base = videos_base_dir();
  fs::path dir = base.empty() ? fs::path(".") : fs::path(base) / "Screencasts";
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) {
    const std::string h = home_dir();
    return h.empty() ? std::string(".") : h;
  }
  return dir.string();
}

std::string default_recording_path(Codec codec) {
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &now);
#else
  localtime_r(&now, &tm);
#endif
  char stamp[32];
  std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
  (void)codec; // both codecs are wrapped in an .mp4 container
  fs::path p = fs::path(default_recording_dir()) /
               (std::string("frametap-") + stamp + ".mp4");
  return p.string();
}

VideoRecorder::VideoRecorder(std::string out_path, EncoderConfig config)
    : impl_(std::make_unique<Impl>(std::move(out_path), config)) {}

VideoRecorder::~VideoRecorder() {
  if (impl_)
    impl_->finish();
}

VideoRecorder::VideoRecorder(VideoRecorder &&) noexcept = default;
VideoRecorder &VideoRecorder::operator=(VideoRecorder &&) noexcept = default;

void VideoRecorder::submit(const Frame &frame) { impl_->submit(frame); }
void VideoRecorder::finish() {
  impl_->finish();
  impl_->rethrow_error();
}
VideoRecorder::Stats VideoRecorder::stats() const { return impl_->stats; }

std::string VideoRecorder::stream_error() const {
  return impl_->stream ? impl_->stream->last_error() : std::string();
}

} // namespace frametap
