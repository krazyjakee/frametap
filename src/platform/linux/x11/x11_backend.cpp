#include "x11_backend.h"
#include "../../../util/color.h"
#include "../../../util/safe_alloc.h"

#include <X11/Xutil.h>

#include <chrono>
#include <cstring>
#include <mutex>

namespace frametap::internal {

// ---------------------------------------------------------------------------
// Custom X11 error handler — prevents exit() on X errors (H5)
// ---------------------------------------------------------------------------

namespace {

thread_local int g_x11_error_code = 0;

int x11_error_handler(Display * /*display*/, XErrorEvent *event) {
  g_x11_error_code = event->error_code;
  return 0; // non-fatal: do not call exit()
}

std::once_flag g_error_handler_installed;

void install_x11_error_handler() {
  std::call_once(g_error_handler_installed,
                 [] { XSetErrorHandler(x11_error_handler); });
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

X11Backend::X11Backend() { open_display(); }

X11Backend::X11Backend(Rect region) : region_(region) { open_display(); }

X11Backend::X11Backend(frametap::Monitor monitor) {
  region_ = {static_cast<double>(monitor.x), static_cast<double>(monitor.y),
             static_cast<double>(monitor.width),
             static_cast<double>(monitor.height)};
  open_display();
}

X11Backend::X11Backend(frametap::Window window) : capture_window_(true) {
  target_ = static_cast<::Window>(window.id);
  open_display();
}

X11Backend::~X11Backend() {
  stop();
  cleanup_shm();
  if (display_)
    XCloseDisplay(display_);
}

// ---------------------------------------------------------------------------
// Display / SHM setup
// ---------------------------------------------------------------------------

void X11Backend::open_display() {
  install_x11_error_handler();

  display_ = XOpenDisplay(nullptr);
  if (!display_)
    throw CaptureError(
        "Failed to open X11 display. Check that $DISPLAY is set correctly "
        "and X11 authorization (xauth) allows connections.");

  screen_ = DefaultScreen(display_);
  root_ = RootWindow(display_, screen_);

  if (!capture_window_)
    target_ = root_;

  use_shm_ = XShmQueryExtension(display_);
  compute_capture_area();
}

void X11Backend::compute_capture_area() {
  if (capture_window_) {
    XWindowAttributes attrs;
    g_x11_error_code = 0;
    if (XGetWindowAttributes(display_, target_, &attrs) && g_x11_error_code == 0) {
      cap_x_ = 0;
      cap_y_ = 0;
      cap_w_ = attrs.width;
      cap_h_ = attrs.height;
    } else {
      throw CaptureError("Failed to get window attributes (window may not exist)");
    }
  } else if (region_.width > 0 && region_.height > 0) {
    cap_x_ = static_cast<int>(region_.x);
    cap_y_ = static_cast<int>(region_.y);
    cap_w_ = static_cast<int>(region_.width);
    cap_h_ = static_cast<int>(region_.height);
  } else {
    cap_x_ = 0;
    cap_y_ = 0;
    cap_w_ = DisplayWidth(display_, screen_);
    cap_h_ = DisplayHeight(display_, screen_);
  }

  // Clamp to screen bounds when capturing root (H4: clamp negatives too)
  if (!capture_window_) {
    int sw = DisplayWidth(display_, screen_);
    int sh = DisplayHeight(display_, screen_);

    // Clamp negative coordinates
    if (cap_x_ < 0) {
      cap_w_ += cap_x_; // shrink width by the negative offset
      cap_x_ = 0;
    }
    if (cap_y_ < 0) {
      cap_h_ += cap_y_;
      cap_y_ = 0;
    }

    // Clamp to upper bounds
    if (cap_x_ + cap_w_ > sw)
      cap_w_ = sw - cap_x_;
    if (cap_y_ + cap_h_ > sh)
      cap_h_ = sh - cap_y_;
  }
}

void X11Backend::init_shm(int width, int height) {
  cleanup_shm();

  if (!use_shm_)
    return;

  shm_image_ = XShmCreateImage(
      display_, DefaultVisual(display_, screen_),
      static_cast<unsigned>(DefaultDepth(display_, screen_)), ZPixmap, nullptr,
      &shm_info_, width, height);
  if (!shm_image_) {
    use_shm_ = false;
    return;
  }

  shm_info_.shmid = shmget(
      IPC_PRIVATE,
      static_cast<size_t>(shm_image_->bytes_per_line) * height,
      IPC_CREAT | 0600);
  if (shm_info_.shmid < 0) {
    XDestroyImage(shm_image_);
    shm_image_ = nullptr;
    use_shm_ = false;
    return;
  }

  // M3: shmat() returns (void*)-1 on failure, not nullptr
  void *shm_addr = shmat(shm_info_.shmid, nullptr, 0);
  if (shm_addr == reinterpret_cast<void *>(-1)) {
    shmctl(shm_info_.shmid, IPC_RMID, nullptr);
    XDestroyImage(shm_image_);
    shm_image_ = nullptr;
    use_shm_ = false;
    return;
  }
  shm_info_.shmaddr = shm_image_->data = static_cast<char *>(shm_addr);
  shm_info_.readOnly = False;

  XShmAttach(display_, &shm_info_);
  XSync(display_, False);
  shmctl(shm_info_.shmid, IPC_RMID, nullptr);

  shm_attached_ = true;
}

void X11Backend::cleanup_shm() {
  if (!shm_attached_)
    return;

  XShmDetach(display_, &shm_info_);
  shm_image_->data = nullptr;
  XDestroyImage(shm_image_);
  shmdt(shm_info_.shmaddr);

  shm_image_ = nullptr;
  shm_attached_ = false;
}

// ---------------------------------------------------------------------------
// Screenshot (delegates to standalone helper)
// ---------------------------------------------------------------------------

ImageData X11Backend::screenshot(Rect region) {
  Rect effective = (region.width > 0 && region.height > 0) ? region : region_;
  return x11_take_screenshot(target_, effective, capture_window_);
}

// ---------------------------------------------------------------------------
// Streaming
// ---------------------------------------------------------------------------

void X11Backend::start(FrameCallback cb) {
  callback_ = std::move(cb);
  init_shm(cap_w_, cap_h_);
  capture_thread_ = std::jthread([this](std::stop_token token) {
    capture_loop(token);
  });
}

void X11Backend::stop() {
  if (capture_thread_.joinable()) {
    capture_thread_.request_stop();
    capture_thread_.join();
  }
}

void X11Backend::pause() { paused_.store(true); }

void X11Backend::resume() { paused_.store(false); }

bool X11Backend::is_paused() const { return paused_.load(); }

void X11Backend::set_region(Rect region) {
  // H1: Protect shared state with mutex
  std::lock_guard lock(state_mutex_);
  region_ = region;
  compute_capture_area();

  // Reinitialise SHM if streaming is active
  if (shm_attached_)
    init_shm(cap_w_, cap_h_);
}

// ---------------------------------------------------------------------------
// Frame capture (used by streaming loop)
// ---------------------------------------------------------------------------

ImageData X11Backend::capture_frame() {
  // H1: Hold lock for the entire capture to prevent set_region() from
  // reallocating SHM while we are reading from it.
  std::lock_guard lock(state_mutex_);

  int cx = cap_x_, cy = cap_y_, cw = cap_w_, ch = cap_h_;

  if (cw <= 0 || ch <= 0)
    return {};

  ImageData result;
  result.width = static_cast<size_t>(cw);
  result.height = static_cast<size_t>(ch);
  // H6: Overflow-checked allocation
  result.data.resize(checked_rgba_size(result.width, result.height));

  if (use_shm_ && shm_image_) {
    g_x11_error_code = 0;
    if (!XShmGetImage(display_, target_, shm_image_, cx, cy,
                      AllPlanes)) {
      return {};
    }
    XSync(display_, False);
    if (g_x11_error_code != 0)
      return {};

    int bpp = shm_image_->bits_per_pixel / 8;
    int depth = shm_image_->depth;
    for (int y = 0; y < ch; y++) {
      const auto *src = reinterpret_cast<const uint8_t *>(shm_image_->data) +
                        y * shm_image_->bytes_per_line;
      auto *dst = result.data.data() + y * cw * 4;

      if (shm_image_->byte_order == LSBFirst && bpp == 4) {
        bgra_to_rgba(src, dst, static_cast<size_t>(cw));
      } else {
        std::memcpy(dst, src, static_cast<size_t>(cw) * 4);
      }

      if (depth <= 24 && bpp == 4) {
        for (int x = 0; x < cw; x++)
          dst[x * 4 + 3] = 0xFF;
      }
    }
  } else {
    // Fallback: XGetImage per frame (slow)
    g_x11_error_code = 0;
    XImage *img = XGetImage(display_, target_, cx, cy, cw, ch,
                            AllPlanes, ZPixmap);
    if (!img || g_x11_error_code != 0) {
      if (img)
        XDestroyImage(img);
      return {};
    }

    int bpp = img->bits_per_pixel / 8;
    int img_depth = img->depth;
    for (int y = 0; y < ch; y++) {
      const auto *src =
          reinterpret_cast<const uint8_t *>(img->data) + y * img->bytes_per_line;
      auto *dst = result.data.data() + y * cw * 4;

      if (img->byte_order == LSBFirst && bpp == 4) {
        bgra_to_rgba(src, dst, static_cast<size_t>(cw));
      } else {
        std::memcpy(dst, src, static_cast<size_t>(cw) * 4);
      }

      if (img_depth <= 24 && bpp == 4) {
        for (int x = 0; x < cw; x++)
          dst[x * 4 + 3] = 0xFF;
      }
    }
    XDestroyImage(img);
  }

  return result;
}

void X11Backend::capture_loop(std::stop_token token) {
  using clock = std::chrono::steady_clock;
  constexpr auto interval = std::chrono::milliseconds(16); // ~60 fps

  auto last_time = clock::now();

  while (!token.stop_requested()) {
    if (paused_.load()) {
      std::this_thread::sleep_for(interval);
      last_time = clock::now();
      continue;
    }

    auto frame_data = capture_frame();
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

} // namespace frametap::internal
