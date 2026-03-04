#include "android_backend.h"
#include "jni_bridge.h"
#include "pixel_convert.h"
#include "../../util/safe_alloc.h"

#include <sys/wait.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace frametap::internal {

using namespace frametap::internal::android;

namespace {

// Read exactly `count` bytes from `fp`, handling partial reads.
// Returns true on success, false on EOF/error.
bool read_fully(FILE *fp, void *buf, size_t count) {
  auto *dst = static_cast<uint8_t *>(buf);
  size_t remaining = count;
  while (remaining > 0) {
    size_t n = std::fread(dst, 1, remaining, fp);
    if (n == 0)
      return false; // EOF or error
    dst += n;
    remaining -= n;
  }
  return true;
}

// Parse the output of `wm size` to get screen dimensions.
// Expected format: "Physical size: WxH\n"
// Returns {width, height} or {0, 0} on failure.
std::pair<int, int> parse_wm_size() {
  FILE *fp = popen("wm size 2>/dev/null", "r");
  if (!fp)
    return {0, 0};

  char line[256];
  int w = 0, h = 0;
  while (std::fgets(line, sizeof(line), fp)) {
    // Look for "Physical size:" (skip "Override size:" lines)
    if (std::sscanf(line, " Physical size: %dx%d", &w, &h) == 2)
      break;
  }
  pclose(fp);
  return {w, h};
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

AndroidBackend::AndroidBackend() = default;

AndroidBackend::AndroidBackend(Rect region) : region_(region) {}

AndroidBackend::AndroidBackend(frametap::Monitor monitor) {
  region_ = {static_cast<double>(monitor.x), static_cast<double>(monitor.y),
             static_cast<double>(monitor.width),
             static_cast<double>(monitor.height)};
}

AndroidBackend::AndroidBackend(frametap::Window /*window*/) {
  throw CaptureError("Window capture is not supported on Android");
}

AndroidBackend::~AndroidBackend() {
  stop();
  if (jni::jni_is_initialized())
    jni::jni_stop_projection();
}

// ---------------------------------------------------------------------------
// Full-screen capture — JNI (MediaProjection) with screencap fallback
// ---------------------------------------------------------------------------

ImageData AndroidBackend::capture_full_screen() {
  // Prefer MediaProjection via JNI when initialized
  if (jni::jni_is_initialized()) {
    if (ensure_projection_active())
      return capture_via_projection();
    // If consent was denied or projection failed, fall through to screencap
  }

  // Legacy path: screencap (works from adb shell / root)
  return capture_via_screencap();
}

bool AndroidBackend::ensure_projection_active() {
  if (jni::jni_is_projection_active())
    return true;

  // Request user consent (blocks until dialog is dismissed)
  return jni::jni_request_consent();
}

ImageData AndroidBackend::capture_via_projection() {
  // ImageReader may not have a frame ready immediately after projection starts.
  // Retry a few times with short sleeps.
  constexpr int max_attempts = 10;
  constexpr auto retry_delay = std::chrono::milliseconds(50);

  for (int attempt = 0; attempt < max_attempts; ++attempt) {
    ImageData frame = jni::jni_capture_frame();
    if (!frame.data.empty())
      return frame;
    std::this_thread::sleep_for(retry_delay);
  }

  throw CaptureError(
      "MediaProjection: no frame available after " +
      std::to_string(max_attempts) + " attempts");
}

// ---------------------------------------------------------------------------
// Legacy full-screen capture via screencap
// ---------------------------------------------------------------------------

ImageData AndroidBackend::capture_via_screencap() {
  FILE *fp = popen("screencap", "r");
  if (!fp)
    throw CaptureError(
        "Failed to run 'screencap'. Ensure you are running as the shell "
        "user (adb shell) or have appropriate permissions.");

  // screencap writes a 12-byte header: uint32_t width, height, format
  // (all little-endian, native on ARM/Android)
  uint32_t header[3];
  if (!read_fully(fp, header, sizeof(header))) {
    pclose(fp);
    throw CaptureError("Failed to read screencap header");
  }

  uint32_t width = header[0];
  uint32_t height = header[1];
  uint32_t format = header[2];

  if (width == 0 || height == 0) {
    pclose(fp);
    throw CaptureError("screencap returned zero dimensions");
  }

  int bpp = bytes_per_pixel(format);
  if (bpp == 0) {
    pclose(fp);
    throw CaptureError("screencap returned unsupported pixel format: " +
                       std::to_string(format));
  }

  // Read raw pixel data (overflow-check the allocation)
  size_t rgba_size = checked_rgba_size(width, height); // validates width*height*4
  size_t pixel_count = static_cast<size_t>(width) * height;
  size_t raw_size = pixel_count * static_cast<size_t>(bpp);
  std::vector<uint8_t> raw(raw_size);

  if (!read_fully(fp, raw.data(), raw_size)) {
    pclose(fp);
    throw CaptureError("Failed to read screencap pixel data (expected " +
                       std::to_string(raw_size) + " bytes)");
  }

  int status = pclose(fp);
  if (status != 0 && WIFEXITED(status) && WEXITSTATUS(status) != 0)
    throw CaptureError("screencap exited with status " +
                       std::to_string(WEXITSTATUS(status)));

  // Convert to RGBA
  ImageData result;
  result.width = width;
  result.height = height;
  result.data.resize(rgba_size);

  convert_to_rgba(raw.data(), result.data.data(), pixel_count, format);
  return result;
}

// ---------------------------------------------------------------------------
// Region cropping
// ---------------------------------------------------------------------------

ImageData AndroidBackend::crop(const ImageData &full, Rect region) {
  // Determine crop bounds, clamped to the full image
  int sx = static_cast<int>(region.x);
  int sy = static_cast<int>(region.y);
  int sw = static_cast<int>(region.width);
  int sh = static_cast<int>(region.height);

  if (sx < 0) {
    sw += sx;
    sx = 0;
  }
  if (sy < 0) {
    sh += sy;
    sy = 0;
  }
  if (sx + sw > static_cast<int>(full.width))
    sw = static_cast<int>(full.width) - sx;
  if (sy + sh > static_cast<int>(full.height))
    sh = static_cast<int>(full.height) - sy;

  if (sw <= 0 || sh <= 0)
    return {};

  ImageData result;
  result.width = static_cast<size_t>(sw);
  result.height = static_cast<size_t>(sh);
  result.data.resize(checked_rgba_size(result.width, result.height));

  for (int y = 0; y < sh; y++) {
    const uint8_t *src_row =
        full.data.data() + (static_cast<size_t>(sy + y) * full.width +
                            static_cast<size_t>(sx)) *
                               4;
    uint8_t *dst_row =
        result.data.data() + static_cast<size_t>(y) * result.width * 4;
    std::memcpy(dst_row, src_row, result.width * 4);
  }

  return result;
}

// ---------------------------------------------------------------------------
// Screenshot (one-shot)
// ---------------------------------------------------------------------------

ImageData AndroidBackend::screenshot(Rect region) {
  Rect effective = (region.width > 0 && region.height > 0) ? region : region_;
  auto full = capture_full_screen();

  if (effective.width > 0 && effective.height > 0)
    return crop(full, effective);
  return full;
}

// ---------------------------------------------------------------------------
// Streaming
// ---------------------------------------------------------------------------

void AndroidBackend::start(FrameCallback cb) {
  callback_ = std::move(cb);
  capture_thread_ = std::jthread([this](std::stop_token token) {
    capture_loop(token);
  });
}

void AndroidBackend::stop() {
  if (capture_thread_.joinable()) {
    capture_thread_.request_stop();
    capture_thread_.join();
  }
}

void AndroidBackend::pause() { paused_.store(true); }

void AndroidBackend::resume() { paused_.store(false); }

bool AndroidBackend::is_paused() const { return paused_.load(); }

void AndroidBackend::set_region(Rect region) {
  std::lock_guard lock(state_mutex_);
  region_ = region;
}

// ---------------------------------------------------------------------------
// Capture loop (streaming, ~60 fps target)
// ---------------------------------------------------------------------------

void AndroidBackend::capture_loop(std::stop_token token) {
  using clock = std::chrono::steady_clock;
  constexpr auto interval = std::chrono::milliseconds(16); // ~60 fps

  auto last_time = clock::now();

  while (!token.stop_requested()) {
    if (paused_.load()) {
      std::this_thread::sleep_for(interval);
      last_time = clock::now();
      continue;
    }

    ImageData frame_data;
    try {
      auto full = capture_full_screen();
      std::lock_guard lock(state_mutex_);
      if (region_.width > 0 && region_.height > 0)
        frame_data = crop(full, region_);
      else
        frame_data = std::move(full);
    } catch (...) {
      // screencap can transiently fail — skip frame
      std::this_thread::sleep_for(interval);
      continue;
    }

    if (frame_data.data.empty()) {
      std::this_thread::sleep_for(interval);
      continue;
    }

    auto now = clock::now();
    double duration =
        std::chrono::duration<double, std::milli>(now - last_time).count();
    last_time = now;

    Frame frame{std::move(frame_data), duration};
    callback_(frame);

    // Sleep for remainder of interval
    auto elapsed = clock::now() - now;
    if (elapsed < interval)
      std::this_thread::sleep_for(interval - elapsed);
  }
}

// ---------------------------------------------------------------------------
// Factory functions (called by frametap.cpp via backend.h)
// ---------------------------------------------------------------------------

std::unique_ptr<Backend> make_backend() {
  return std::make_unique<AndroidBackend>();
}

std::unique_ptr<Backend> make_backend(Rect region) {
  return std::make_unique<AndroidBackend>(region);
}

std::unique_ptr<Backend> make_backend(Monitor monitor) {
  return std::make_unique<AndroidBackend>(monitor);
}

std::unique_ptr<Backend> make_backend(Window window) {
  return std::make_unique<AndroidBackend>(window);
}

// ---------------------------------------------------------------------------
// Monitor enumeration
// ---------------------------------------------------------------------------

std::vector<Monitor> enumerate_monitors() {
  int w = 0, h = 0;

  // Try JNI display metrics first (works from APK context)
  if (jni::jni_is_initialized()) {
    auto [jw, jh] = jni::jni_get_display_size();
    w = jw;
    h = jh;
  }

  // Fall back to wm size (works from adb shell)
  if (w <= 0 || h <= 0)
    std::tie(w, h) = parse_wm_size();

  if (w <= 0 || h <= 0)
    return {};

  Monitor m;
  m.id = 0;
  m.name = "Android Display";
  m.x = 0;
  m.y = 0;
  m.width = w;
  m.height = h;
  m.scale = 1.0f;
  return {m};
}

// ---------------------------------------------------------------------------
// Window enumeration (not supported on Android)
// ---------------------------------------------------------------------------

std::vector<Window> enumerate_windows() { return {}; }

// ---------------------------------------------------------------------------
// Permission diagnostics
// ---------------------------------------------------------------------------

PermissionCheck check_platform_permissions() {
  PermissionCheck result;

  // Check JNI / MediaProjection path first
  if (jni::jni_is_initialized()) {
    if (jni::jni_is_projection_active()) {
      result.status = PermissionStatus::ok;
      result.summary = "MediaProjection active";
      result.details.push_back(
          "MediaProjection is initialized and capturing.");
      return result;
    }
    // JNI initialized but projection not yet active — consent not yet granted
    result.status = PermissionStatus::warning;
    result.summary = "MediaProjection initialized, consent pending";
    result.details.push_back(
        "android_init() was called but MediaProjection consent has not "
        "been granted yet. Capture will request consent on first use.");
    return result;
  }

  // Legacy path: test if screencap is available and functional
  FILE *fp = popen("screencap 2>&1", "r");
  if (!fp) {
    result.status = PermissionStatus::error;
    result.summary = "Cannot execute screencap";
    result.details.push_back(
        "Failed to invoke the 'screencap' command. "
        "This tool requires an Android environment. "
        "Call android_init() to use MediaProjection instead.");
    return result;
  }

  // Read just the header to verify it works
  uint32_t header[3];
  bool ok = read_fully(fp, header, sizeof(header));
  pclose(fp);

  if (!ok) {
    result.status = PermissionStatus::error;
    result.summary = "screencap failed";
    result.details.push_back(
        "The 'screencap' command did not produce output. "
        "Ensure you are running as the shell user (adb shell) "
        "or have screen capture permissions.");
    result.details.push_back(
        "For APK usage, call android_init() to use MediaProjection.");
    return result;
  }

  uint32_t width = header[0];
  uint32_t height = header[1];
  uint32_t format = header[2];

  if (width == 0 || height == 0 || bytes_per_pixel(format) == 0) {
    result.status = PermissionStatus::warning;
    result.summary = "screencap returned unexpected data";
    result.details.push_back(
        "screencap ran but returned unexpected header values "
        "(width=" +
        std::to_string(width) + ", height=" + std::to_string(height) +
        ", format=" + std::to_string(format) + ").");
    return result;
  }

  result.status = PermissionStatus::ok;
  result.summary = "Android screencap ready";
  result.details.push_back("screencap works (" + std::to_string(width) + "x" +
                           std::to_string(height) + ", format " +
                           std::to_string(format) + ").");
  return result;
}

} // namespace frametap::internal
