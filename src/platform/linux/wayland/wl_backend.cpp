#include "wl_backend.h"
#include "../../../util/color.h"
#include "../../../util/safe_alloc.h"

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/format.h>

#include <cstring>
#include <unistd.h>

namespace frametap::internal {

// ---------------------------------------------------------------------------
// PipeWire stream callbacks (C trampolines -> backend methods)
// ---------------------------------------------------------------------------

namespace {

void on_process(void *data) {
  auto *sd = static_cast<StreamData *>(data);
  sd->backend->on_process();
}

void on_param_changed(void *data, uint32_t id, const struct spa_pod *param) {
  auto *sd = static_cast<StreamData *>(data);
  sd->backend->on_param_changed(id, param);
}

// Static PipeWire stream event table
const struct pw_stream_events stream_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .param_changed = on_param_changed,
    .process = on_process,
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

WaylandBackend::WaylandBackend() { pw_init(nullptr, nullptr); }

WaylandBackend::WaylandBackend(Rect region) : region_(region) {
  pw_init(nullptr, nullptr);
}

WaylandBackend::WaylandBackend(frametap::Monitor /*monitor*/) {
  // On Wayland, monitor selection happens via the portal picker dialog.
  // We store the request but can't target a specific monitor programmatically.
  pw_init(nullptr, nullptr);
}

WaylandBackend::WaylandBackend(frametap::Window /*window*/)
    : capture_window_(true) {
  pw_init(nullptr, nullptr);
}

WaylandBackend::~WaylandBackend() {
  stop();
  teardown_pipewire();
  if (portal_open_)
    close_screencast_session(portal_);
}

// ---------------------------------------------------------------------------
// PipeWire setup / teardown
// ---------------------------------------------------------------------------

void WaylandBackend::setup_pipewire() {
  if (!portal_open_) {
    portal_ = open_screencast_session(capture_window_);
    portal_open_ = true;
  }

  pw_loop_ = pw_main_loop_new(nullptr);
  if (!pw_loop_)
    throw CaptureError("Failed to create PipeWire main loop");

  pw_context_ = pw_context_new(pw_main_loop_get_loop(pw_loop_), nullptr, 0);
  if (!pw_context_) {
    pw_main_loop_destroy(pw_loop_);
    pw_loop_ = nullptr;
    throw CaptureError("Failed to create PipeWire context");
  }

  // M1: dup() the FD first, close it on connect failure
  int connect_fd = dup(portal_.pw_fd);
  if (connect_fd < 0) {
    pw_context_destroy(pw_context_);
    pw_context_ = nullptr;
    pw_main_loop_destroy(pw_loop_);
    pw_loop_ = nullptr;
    throw CaptureError("Failed to duplicate PipeWire FD");
  }

  pw_core_ = pw_context_connect_fd(pw_context_, connect_fd, nullptr, 0);
  if (!pw_core_) {
    close(connect_fd);
    pw_context_destroy(pw_context_);
    pw_context_ = nullptr;
    pw_main_loop_destroy(pw_loop_);
    pw_loop_ = nullptr;
    throw CaptureError("Failed to connect to PipeWire (bad FD?)");
  }

  auto *props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Video",
                                  PW_KEY_MEDIA_CATEGORY, "Capture",
                                  PW_KEY_MEDIA_ROLE, "Screen", nullptr);

  pw_stream_ = pw_stream_new(pw_core_, "frametap", props);
  if (!pw_stream_)
    throw CaptureError("Failed to create PipeWire stream");

  // H3: Use per-instance stream data instead of thread_local global
  stream_data_.backend = this;
  // L2: Allocated here, freed in teardown_pipewire()
  stream_listener_ = new spa_hook{};
  spa_zero(*stream_listener_);
  pw_stream_add_listener(pw_stream_, stream_listener_, &stream_events,
                         &stream_data_);

  // Build format parameters — accept common screen-capture formats
  uint8_t buffer[1024];
  struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

  // Named rectangle structs — SPA_RECTANGLE() compound literals can't have
  // their address taken in C++
  struct spa_rectangle size_default = {1920, 1080};
  struct spa_rectangle size_min = {1, 1};
  struct spa_rectangle size_max = {8192, 8192};

  const struct spa_pod *params[1];
  params[0] = static_cast<const spa_pod *>(spa_pod_builder_add_object(
      &builder, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
      SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
      SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
      SPA_FORMAT_VIDEO_format,
      SPA_POD_CHOICE_ENUM_Id(5, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx,
                             SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBx,
                             SPA_VIDEO_FORMAT_RGBA),
      SPA_FORMAT_VIDEO_size,
      SPA_POD_CHOICE_RANGE_Rectangle(&size_default, &size_min, &size_max)));

  pw_stream_connect(
      pw_stream_, PW_DIRECTION_INPUT, portal_.pw_node,
      static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                   PW_STREAM_FLAG_MAP_BUFFERS),
      params, 1);
}

void WaylandBackend::teardown_pipewire() {
  if (pw_stream_) {
    pw_stream_destroy(pw_stream_);
    pw_stream_ = nullptr;
  }
  delete stream_listener_;
  stream_listener_ = nullptr;

  if (pw_core_) {
    pw_core_disconnect(pw_core_);
    pw_core_ = nullptr;
  }
  if (pw_context_) {
    pw_context_destroy(pw_context_);
    pw_context_ = nullptr;
  }
  if (pw_loop_) {
    pw_main_loop_destroy(pw_loop_);
    pw_loop_ = nullptr;
  }
}

// ---------------------------------------------------------------------------
// PipeWire callbacks
// ---------------------------------------------------------------------------

void WaylandBackend::on_param_changed(uint32_t id, const ::spa_pod *param) {
  if (!param || id != SPA_PARAM_Format)
    return;

  struct spa_video_info_raw info;
  if (spa_format_video_raw_parse(param, &info) < 0)
    return;

  video_format_ = info.format;
  frame_width_ = info.size.width;
  frame_height_ = info.size.height;
}

void WaylandBackend::on_process() {
  if (paused_.load())
    return;

  struct pw_buffer *buf = pw_stream_dequeue_buffer(pw_stream_);
  if (!buf)
    return;

  struct spa_buffer *spa_buf = buf->buffer;
  if (!spa_buf->datas[0].data) {
    pw_stream_queue_buffer(pw_stream_, buf);
    return;
  }

  auto *src = static_cast<const uint8_t *>(spa_buf->datas[0].data);
  int stride = spa_buf->datas[0].chunk->stride;
  int w = frame_width_;
  int h = frame_height_;

  if (w <= 0 || h <= 0) {
    pw_stream_queue_buffer(pw_stream_, buf);
    return;
  }

  // Convert to RGBA
  ImageData image;
  image.width = static_cast<size_t>(w);
  image.height = static_cast<size_t>(h);
  // H6: Overflow-checked allocation
  image.data.resize(checked_rgba_size(image.width, image.height));

  for (int y = 0; y < h; y++) {
    const uint8_t *row = src + y * stride;
    uint8_t *dst = image.data.data() + y * w * 4;

    switch (video_format_) {
    case SPA_VIDEO_FORMAT_BGRx:
    case SPA_VIDEO_FORMAT_BGRA:
      bgra_to_rgba(row, dst, static_cast<size_t>(w));
      break;
    case SPA_VIDEO_FORMAT_RGBA:
      std::memcpy(dst, row, static_cast<size_t>(w) * 4);
      break;
    case SPA_VIDEO_FORMAT_RGBx:
      std::memcpy(dst, row, static_cast<size_t>(w) * 4);
      // Set alpha channel to 255
      for (int x = 0; x < w; x++)
        dst[x * 4 + 3] = 255;
      break;
    default:
      // Unknown format — try BGRA conversion as best guess
      bgra_to_rgba(row, dst, static_cast<size_t>(w));
      break;
    }
  }

  // H2: Apply region crop — snapshot region under lock
  Rect region_copy;
  {
    std::lock_guard lock(state_mutex_);
    region_copy = region_;
  }

  if (region_copy.width > 0 && region_copy.height > 0) {
    int rx = static_cast<int>(region_copy.x);
    int ry = static_cast<int>(region_copy.y);
    int rw = static_cast<int>(region_copy.width);
    int rh = static_cast<int>(region_copy.height);

    // Clamp negatives
    if (rx < 0) { rw += rx; rx = 0; }
    if (ry < 0) { rh += ry; ry = 0; }

    // Clamp to frame bounds
    if (rx + rw > w)
      rw = w - rx;
    if (ry + rh > h)
      rh = h - ry;

    if (rx >= 0 && ry >= 0 && rw > 0 && rh > 0 &&
        (rw != w || rh != h || rx != 0 || ry != 0)) {
      ImageData cropped;
      cropped.width = static_cast<size_t>(rw);
      cropped.height = static_cast<size_t>(rh);
      cropped.data.resize(checked_rgba_size(cropped.width, cropped.height));

      for (int cy = 0; cy < rh; cy++) {
        const uint8_t *srow =
            image.data.data() + (ry + cy) * w * 4 + rx * 4;
        uint8_t *drow = cropped.data.data() + cy * rw * 4;
        std::memcpy(drow, srow, static_cast<size_t>(rw) * 4);
      }
      image = std::move(cropped);
    }
  }

  pw_stream_queue_buffer(pw_stream_, buf);

  // M6: Compute frame duration under lock
  auto now = std::chrono::steady_clock::now();
  double duration;
  {
    std::lock_guard lock(state_mutex_);
    duration =
        std::chrono::duration<double, std::milli>(now - last_frame_time_).count();
    last_frame_time_ = now;
  }

  Frame frame{std::move(image), duration};
  if (callback_)
    callback_(frame);
}

// ---------------------------------------------------------------------------
// Capture control
// ---------------------------------------------------------------------------

ImageData WaylandBackend::screenshot(Rect region) {
  // Use PipeWire stream: open, grab one frame, close.
  // This reuses the portal session if already open.
  if (!portal_open_) {
    portal_ = open_screencast_session(capture_window_);
    portal_open_ = true;
  }

  // Set up a temporary PipeWire stream to grab one frame
  struct pw_main_loop *loop = pw_main_loop_new(nullptr);
  struct pw_context *ctx =
      pw_context_new(pw_main_loop_get_loop(loop), nullptr, 0);

  // M1: dup() FD first, close on failure
  int connect_fd = dup(portal_.pw_fd);
  if (connect_fd < 0) {
    pw_context_destroy(ctx);
    pw_main_loop_destroy(loop);
    throw CaptureError("Failed to duplicate PipeWire FD for screenshot");
  }

  struct pw_core *core = pw_context_connect_fd(ctx, connect_fd, nullptr, 0);
  if (!core) {
    close(connect_fd);
    pw_context_destroy(ctx);
    pw_main_loop_destroy(loop);
    throw CaptureError("Failed to connect PipeWire for screenshot");
  }

  struct pw_stream *stream =
      pw_stream_new(core, "frametap-screenshot",
                    pw_properties_new(PW_KEY_MEDIA_TYPE, "Video",
                                      PW_KEY_MEDIA_CATEGORY, "Capture",
                                      PW_KEY_MEDIA_ROLE, "Screen", nullptr));

  // Screenshot state
  struct ShotState {
    ImageData result;
    uint32_t format = 0;
    int32_t width = 0, height = 0;
    bool got_frame = false;
    pw_main_loop *loop = nullptr;
    Rect region{};
  } state;
  state.loop = loop;
  state.region = (region.width > 0 && region.height > 0) ? region : region_;

  struct spa_hook listener{};
  spa_zero(listener);

  static const struct pw_stream_events shot_events = {
      .version = PW_VERSION_STREAM_EVENTS,
      .param_changed =
          [](void *data, uint32_t id, const struct spa_pod *param) {
            if (!param || id != SPA_PARAM_Format)
              return;
            auto *s = static_cast<ShotState *>(data);
            struct spa_video_info_raw info;
            if (spa_format_video_raw_parse(param, &info) >= 0) {
              s->format = info.format;
              s->width = info.size.width;
              s->height = info.size.height;
            }
          },
      .process =
          [](void *data) {
            auto *s = static_cast<ShotState *>(data);
            s->got_frame = true;
            pw_main_loop_quit(s->loop);
          },
  };

  pw_stream_add_listener(stream, &listener, &shot_events, &state);

  uint8_t buffer[1024];
  struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

  struct spa_rectangle shot_size_default = {1920, 1080};
  struct spa_rectangle shot_size_min = {1, 1};
  struct spa_rectangle shot_size_max = {8192, 8192};

  const struct spa_pod *params[1];
  params[0] = static_cast<const spa_pod *>(spa_pod_builder_add_object(
      &builder, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
      SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
      SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
      SPA_FORMAT_VIDEO_format,
      SPA_POD_CHOICE_ENUM_Id(5, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx,
                             SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBx,
                             SPA_VIDEO_FORMAT_RGBA),
      SPA_FORMAT_VIDEO_size,
      SPA_POD_CHOICE_RANGE_Rectangle(&shot_size_default, &shot_size_min,
                                     &shot_size_max)));

  pw_stream_connect(
      stream, PW_DIRECTION_INPUT, portal_.pw_node,
      static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                   PW_STREAM_FLAG_MAP_BUFFERS),
      params, 1);

  // Run until we get one frame (or timeout via a timer)
  // Add a timeout source
  auto *timer = pw_loop_add_timer(
      pw_main_loop_get_loop(loop),
      [](void *data, uint64_t /*expirations*/) {
        auto *s = static_cast<ShotState *>(data);
        pw_main_loop_quit(s->loop);
      },
      &state);

  struct timespec timeout = {.tv_sec = 5, .tv_nsec = 0};
  pw_loop_update_timer(pw_main_loop_get_loop(loop), timer, &timeout, nullptr,
                       false);

  pw_main_loop_run(loop);

  // If we got a frame, dequeue it now
  ImageData result;
  if (state.got_frame && state.width > 0 && state.height > 0) {
    struct pw_buffer *buf = pw_stream_dequeue_buffer(stream);
    if (buf && buf->buffer->datas[0].data) {
      auto *src = static_cast<const uint8_t *>(buf->buffer->datas[0].data);
      int stride = buf->buffer->datas[0].chunk->stride;
      int w = state.width, h = state.height;

      result.width = static_cast<size_t>(w);
      result.height = static_cast<size_t>(h);
      // H6: Overflow-checked allocation
      result.data.resize(checked_rgba_size(result.width, result.height));

      for (int y = 0; y < h; y++) {
        const uint8_t *row = src + y * stride;
        uint8_t *dst = result.data.data() + y * w * 4;
        switch (state.format) {
        case SPA_VIDEO_FORMAT_BGRx:
        case SPA_VIDEO_FORMAT_BGRA:
          bgra_to_rgba(row, dst, static_cast<size_t>(w));
          break;
        default:
          std::memcpy(dst, row, static_cast<size_t>(w) * 4);
          break;
        }
      }
      pw_stream_queue_buffer(stream, buf);

      // Apply region crop
      Rect &r = state.region;
      if (r.width > 0 && r.height > 0) {
        int rx = static_cast<int>(r.x), ry = static_cast<int>(r.y);
        int rw = static_cast<int>(r.width), rh = static_cast<int>(r.height);

        // Clamp negatives
        if (rx < 0) { rw += rx; rx = 0; }
        if (ry < 0) { rh += ry; ry = 0; }

        if (rx + rw > w)
          rw = w - rx;
        if (ry + rh > h)
          rh = h - ry;
        if (rx >= 0 && ry >= 0 && rw > 0 && rh > 0) {
          ImageData cropped;
          cropped.width = static_cast<size_t>(rw);
          cropped.height = static_cast<size_t>(rh);
          cropped.data.resize(checked_rgba_size(cropped.width, cropped.height));
          for (int cy = 0; cy < rh; cy++) {
            std::memcpy(cropped.data.data() + cy * rw * 4,
                        result.data.data() + (ry + cy) * w * 4 + rx * 4,
                        static_cast<size_t>(rw) * 4);
          }
          result = std::move(cropped);
        }
      }
    }
  }

  // Cleanup
  pw_stream_destroy(stream);
  pw_core_disconnect(core);
  pw_context_destroy(ctx);
  pw_main_loop_destroy(loop);

  return result;
}

void WaylandBackend::start(FrameCallback cb) {
  callback_ = std::move(cb);
  {
    std::lock_guard lock(state_mutex_);
    last_frame_time_ = std::chrono::steady_clock::now();
  }

  setup_pipewire();

  // Run PipeWire main loop on a dedicated thread
  pw_thread_ = std::jthread([this](std::stop_token token) {
    // Install a stop callback that quits the PipeWire loop
    std::stop_callback stop_cb(token, [this] {
      if (pw_loop_)
        pw_main_loop_quit(pw_loop_);
    });

    pw_main_loop_run(pw_loop_);
  });
}

void WaylandBackend::stop() {
  if (pw_thread_.joinable()) {
    pw_thread_.request_stop();
    if (pw_loop_)
      pw_main_loop_quit(pw_loop_);
    pw_thread_.join();
  }
  teardown_pipewire();
}

void WaylandBackend::pause() {
  // M6: Only set the atomic flag — don't call pw_stream_set_active from the
  // wrong thread. The on_process callback checks paused_ and discards frames.
  paused_.store(true);
}

void WaylandBackend::resume() {
  paused_.store(false);
  // M6: Update last_frame_time_ under lock
  std::lock_guard lock(state_mutex_);
  last_frame_time_ = std::chrono::steady_clock::now();
}

bool WaylandBackend::is_paused() const { return paused_.load(); }

void WaylandBackend::set_region(Rect region) {
  // H2: Protect region_ with mutex
  std::lock_guard lock(state_mutex_);
  region_ = region;
}

} // namespace frametap::internal
