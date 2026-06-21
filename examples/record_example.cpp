// Hardware screen recorder demo (Linux / NVIDIA NVENC).
//
//   scons record
//   ./examples/record_example --seconds 8 --codec h264 --bitrate 20000
//   mpv ~/Videos/Screencasts/frametap-*.mp4   # or: ffplay <file>
//
// Captures the primary monitor (or --monitor <id>), encodes each frame on the
// GPU via NVENC, muxes video + audio, and writes a directly-playable .mp4
// (or streams it over the network with --stream).

#include <frametap/frametap.h>
#include <frametap/recording.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace {

struct Options {
  frametap::Codec codec = frametap::Codec::h264;
  int bitrate_kbps = 20000;
  int fps = 60;
  int seconds = 8;
  int monitor_id = -1;
  uint64_t audio_pid = 0;
  frametap::AdaptPriority priority = frametap::AdaptPriority::fps;
  std::string output;
  bool stream = false;
  frametap::StreamProtocol protocol = frametap::StreamProtocol::srt;
  std::string stream_url;
  bool no_file = false;
  bool help = false;
  std::string error;
};

void usage(const char *prog) {
  std::printf(
      "Usage: %s [options]\n"
      "\n"
      "  --codec h264|hevc        Video codec (default: h264)\n"
      "  --bitrate <kbps>         Starting bitrate (default: 20000)\n"
      "  --fps <n>                Target fps for pacing/adaptation (default: 60)\n"
      "  --seconds <n>            Capture duration (default: 8)\n"
      "  --monitor <id>           Monitor to capture (default: primary)\n"
      "  --audio-pid <pid>        Capture only this process's audio (default:\n"
      "                           all system audio)\n"
      "  --priority fps|quality|none  Adaptation target (default: fps)\n"
      "  -o, --output <file>      Output file (default: a timestamped file in\n"
      "                           ~/Videos/Screencasts)\n"
      "  --stream <srt|udp|rtmp>  Also stream over the network\n"
      "  --url <url>              Stream destination. Defaults per protocol:\n"
      "                           srt  -> srt://0.0.0.0:9000?mode=listener\n"
      "                                   (you serve; viewer: srt://your-ip:9000)\n"
      "                           udp  -> udp://127.0.0.1:1234\n"
      "                           rtmp -> rtmp://127.0.0.1/live/stream\n"
      "  --no-file                Stream only; don't write a local file\n"
      "  -h, --help               Show this help\n",
      prog);
}

Options parse(int argc, char **argv) {
  Options o;
  auto need = [&](int &i) -> const char * {
    if (i + 1 >= argc) {
      o.error = std::string(argv[i]) + " requires an argument";
      return nullptr;
    }
    return argv[++i];
  };
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "-h" || a == "--help") {
      o.help = true;
    } else if (a == "--codec") {
      const char *v = need(i);
      if (!v)
        break;
      if (std::strcmp(v, "h264") == 0)
        o.codec = frametap::Codec::h264;
      else if (std::strcmp(v, "hevc") == 0)
        o.codec = frametap::Codec::hevc;
      else
        o.error = "invalid --codec (expected h264 or hevc)";
    } else if (a == "--bitrate") {
      const char *v = need(i);
      if (v)
        o.bitrate_kbps = std::atoi(v);
    } else if (a == "--fps") {
      const char *v = need(i);
      if (v)
        o.fps = std::atoi(v);
    } else if (a == "--seconds") {
      const char *v = need(i);
      if (v)
        o.seconds = std::atoi(v);
    } else if (a == "--monitor") {
      const char *v = need(i);
      if (v)
        o.monitor_id = std::atoi(v);
    } else if (a == "--audio-pid") {
      const char *v = need(i);
      if (v)
        o.audio_pid = std::strtoull(v, nullptr, 10);
    } else if (a == "--priority") {
      const char *v = need(i);
      if (!v)
        break;
      if (std::strcmp(v, "fps") == 0)
        o.priority = frametap::AdaptPriority::fps;
      else if (std::strcmp(v, "quality") == 0)
        o.priority = frametap::AdaptPriority::quality;
      else if (std::strcmp(v, "none") == 0)
        o.priority = frametap::AdaptPriority::none;
      else
        o.error = "invalid --priority (expected fps, quality, or none)";
    } else if (a == "-o" || a == "--output") {
      const char *v = need(i);
      if (v)
        o.output = v;
    } else if (a == "--stream") {
      const char *v = need(i);
      if (!v)
        break;
      o.stream = true;
      if (std::strcmp(v, "srt") == 0)
        o.protocol = frametap::StreamProtocol::srt;
      else if (std::strcmp(v, "udp") == 0)
        o.protocol = frametap::StreamProtocol::udp_ts;
      else if (std::strcmp(v, "rtmp") == 0)
        o.protocol = frametap::StreamProtocol::rtmp;
      else
        o.error = "invalid --stream (expected srt, udp, or rtmp)";
    } else if (a == "--url") {
      const char *v = need(i);
      if (v)
        o.stream_url = v;
    } else if (a == "--no-file") {
      o.no_file = true;
    } else {
      o.error = "unknown option '" + a + "'";
    }
    if (!o.error.empty())
      break;
  }
  if (o.no_file && !o.stream)
    o.error = "--no-file requires --stream";
  if (o.stream && o.stream_url.empty()) {
    switch (o.protocol) {
    case frametap::StreamProtocol::srt:
      o.stream_url = "srt://0.0.0.0:9000?mode=listener";
      break;
    case frametap::StreamProtocol::udp_ts:
      o.stream_url = "udp://127.0.0.1:1234";
      break;
    case frametap::StreamProtocol::rtmp:
      o.stream_url = "rtmp://127.0.0.1/live/stream";
      break;
    }
  }
  if (o.output.empty() && !o.no_file)
    o.output = frametap::default_recording_path(o.codec);
  return o;
}

} // namespace

int main(int argc, char **argv) {
  Options opt = parse(argc, argv);
  if (opt.help) {
    usage(argv[0]);
    return 0;
  }
  if (!opt.error.empty()) {
    std::fprintf(stderr, "Error: %s\n", opt.error.c_str());
    usage(argv[0]);
    return 1;
  }

  auto perms = frametap::check_permissions();
  if (perms.status == frametap::PermissionStatus::error) {
    std::fprintf(stderr, "%s\n", perms.summary.c_str());
    for (const auto &d : perms.details)
      std::fprintf(stderr, "  %s\n", d.c_str());
    return 1;
  }

  frametap::EncoderConfig cfg;
  cfg.codec = opt.codec;
  cfg.fps = opt.fps;
  cfg.bitrate_kbps = opt.bitrate_kbps;
  cfg.priority = opt.priority;
  cfg.audio_source_pid = opt.audio_pid;
  if (opt.stream) {
    cfg.stream.enabled = true;
    cfg.stream.protocol = opt.protocol;
    cfg.stream.url = opt.stream_url;
    cfg.stream.also_save_file = !opt.no_file;
    std::printf("Streaming to %s%s\n", opt.stream_url.c_str(),
                opt.no_file ? " (no local file)" : "");
  }

  try {
    frametap::VideoRecorder recorder(opt.output, cfg);

    // Pick the capture target.
    auto make_tap = [&]() {
      auto monitors = frametap::get_monitors();
      if (opt.monitor_id >= 0) {
        for (const auto &m : monitors)
          if (m.id == opt.monitor_id)
            return frametap::FrameTap(m);
        std::fprintf(stderr,
                     "Monitor %d not found; using first monitor instead.\n",
                     opt.monitor_id);
      }
      // No explicit monitor: capture the first one rather than the default
      // FrameTap(), which spans the whole multi-monitor virtual desktop and
      // can exceed the codec's max frame dimension (4096 for H.264).
      if (!monitors.empty()) {
        const auto &m = monitors.front();
        std::printf("Capturing monitor %d (%dx%d) \"%s\".\n", m.id, m.width,
                    m.height, m.name.c_str());
        return frametap::FrameTap(m);
      }
      return frametap::FrameTap();
    };

    frametap::FrameTap tap = make_tap();
    tap.on_frame(
        [&recorder](const frametap::Frame &f) { recorder.submit(f); });

    std::printf("Recording %s for %d s (codec=%s, start bitrate=%d kbps, "
                "priority=%s)...\n",
                opt.output.c_str(), opt.seconds,
                opt.codec == frametap::Codec::hevc ? "hevc" : "h264",
                opt.bitrate_kbps,
                opt.priority == frametap::AdaptPriority::quality ? "quality"
                : opt.priority == frametap::AdaptPriority::none   ? "none"
                                                                  : "fps");

    tap.start_async();
    std::this_thread::sleep_for(std::chrono::seconds(opt.seconds));
    tap.stop();
    recorder.finish();

    if (opt.stream) {
      const std::string serr = recorder.stream_error();
      if (serr.empty())
        std::printf("Stream finished cleanly.\n");
      else
        std::fprintf(stderr, "Stream error: %s\n", serr.c_str());
    }

    auto s = recorder.stats();
    std::printf("\nDone.\n");
    std::printf("  frames in/encoded : %llu / %llu\n",
                static_cast<unsigned long long>(s.frames_in),
                static_cast<unsigned long long>(s.frames_encoded));
    std::printf("  bytes written     : %llu (%.1f MB)\n",
                static_cast<unsigned long long>(s.bytes_written),
                s.bytes_written / (1024.0 * 1024.0));
    std::printf("  avg encode time   : %.2f ms/frame\n", s.avg_encode_ms);
    std::printf("  bitrate (final)   : %d kbps\n", s.current_bitrate_kbps);
    std::printf("  adaptations       : %d\n", s.adaptations);
    if (!opt.output.empty())
      std::printf("\nPlay it:  mpv %s   (or: ffplay %s)\n", opt.output.c_str(),
                  opt.output.c_str());
  } catch (const std::exception &e) {
    std::fprintf(stderr, "Recording failed: %s\n", e.what());
    return 1;
  }

  return 0;
}
