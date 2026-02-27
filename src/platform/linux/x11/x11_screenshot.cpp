#include "x11_backend.h"
#include "x11_error.h"
#include "../../../util/color.h"
#include "../../../util/safe_alloc.h"

#include <X11/Xutil.h>

#include <cstring>

namespace frametap::internal {

ImageData x11_take_screenshot(::Window target, Rect region,
                              bool capture_window) {
  x11_err::install();

  Display *dpy = XOpenDisplay(nullptr);
  if (!dpy)
    throw CaptureError(
        "Failed to open X11 display. Check that $DISPLAY is set correctly "
        "and X11 authorization (xauth) allows connections.");

  int screen = DefaultScreen(dpy);
  ::Window root = RootWindow(dpy, screen);
  ::Window drawable = capture_window ? target : root;

  // Determine capture area
  int cap_x, cap_y, cap_w, cap_h;

  if (capture_window) {
    XWindowAttributes attrs;
    x11_err::g_code = 0;
    if (!XGetWindowAttributes(dpy, target, &attrs) || x11_err::g_code != 0) {
      XCloseDisplay(dpy);
      throw CaptureError("Failed to get window attributes (window may have been closed)");
    }
    cap_x = 0;
    cap_y = 0;
    cap_w = attrs.width;
    cap_h = attrs.height;
  } else if (region.width > 0 && region.height > 0) {
    cap_x = static_cast<int>(region.x);
    cap_y = static_cast<int>(region.y);
    cap_w = static_cast<int>(region.width);
    cap_h = static_cast<int>(region.height);
  } else {
    cap_x = 0;
    cap_y = 0;
    cap_w = DisplayWidth(dpy, screen);
    cap_h = DisplayHeight(dpy, screen);
  }

  // H4: Clamp to drawable bounds (including negative coordinates)
  if (!capture_window) {
    int dw = DisplayWidth(dpy, screen);
    int dh = DisplayHeight(dpy, screen);

    // Clamp negative coordinates
    if (cap_x < 0) {
      cap_w += cap_x;
      cap_x = 0;
    }
    if (cap_y < 0) {
      cap_h += cap_y;
      cap_y = 0;
    }

    // Clamp to upper bounds
    if (cap_x + cap_w > dw)
      cap_w = dw - cap_x;
    if (cap_y + cap_h > dh)
      cap_h = dh - cap_y;
  }

  if (cap_w <= 0 || cap_h <= 0) {
    XCloseDisplay(dpy);
    return {};
  }

  // Try XShm first for better performance
  bool use_shm = XShmQueryExtension(dpy);
  XImage *img = nullptr;
  XShmSegmentInfo shm_info{};

  if (use_shm) {
    img = XShmCreateImage(dpy, DefaultVisual(dpy, screen),
                          static_cast<unsigned>(DefaultDepth(dpy, screen)),
                          ZPixmap, nullptr, &shm_info, cap_w, cap_h);
    if (img) {
      shm_info.shmid = shmget(IPC_PRIVATE,
                               static_cast<size_t>(img->bytes_per_line) * cap_h,
                               IPC_CREAT | 0600);
      if (shm_info.shmid < 0) {
        XDestroyImage(img);
        img = nullptr;
        use_shm = false;
      } else {
        // M3: shmat() returns (void*)-1 on failure, not nullptr
        void *shm_addr = shmat(shm_info.shmid, nullptr, 0);
        if (shm_addr == reinterpret_cast<void *>(-1)) {
          shmctl(shm_info.shmid, IPC_RMID, nullptr);
          XDestroyImage(img);
          img = nullptr;
          use_shm = false;
        } else {
          shm_info.shmaddr = img->data = static_cast<char *>(shm_addr);
          shm_info.readOnly = False;
          XShmAttach(dpy, &shm_info);
          // Mark for removal once all processes detach
          shmctl(shm_info.shmid, IPC_RMID, nullptr);

          x11_err::g_code = 0;
          if (!XShmGetImage(dpy, drawable, img, cap_x, cap_y, AllPlanes)) {
            // SHM capture failed, fall back
            XShmDetach(dpy, &shm_info);
            img->data = nullptr;
            XDestroyImage(img);
            shmdt(shm_info.shmaddr);
            img = nullptr;
            use_shm = false;
          } else {
            XSync(dpy, False);
            if (x11_err::g_code != 0) {
              XShmDetach(dpy, &shm_info);
              img->data = nullptr;
              XDestroyImage(img);
              shmdt(shm_info.shmaddr);
              img = nullptr;
              use_shm = false;
            }
          }
        }
      }
    } else {
      use_shm = false;
    }
  }

  // Fallback to XGetImage
  if (!img) {
    x11_err::g_code = 0;
    img = XGetImage(dpy, drawable, cap_x, cap_y, cap_w, cap_h, AllPlanes,
                    ZPixmap);
    if (img) {
      XSync(dpy, False);
      if (x11_err::g_code != 0) {
        XDestroyImage(img);
        img = nullptr;
      }
    }
  }

  if (!img) {
    XCloseDisplay(dpy);
    throw CaptureError(
        "Failed to capture X11 image. The window may have been closed or "
        "the capture region may be outside screen bounds.");
  }

  // Convert to RGBA
  ImageData result;
  result.width = static_cast<size_t>(cap_w);
  result.height = static_cast<size_t>(cap_h);
  // H6: Overflow-checked allocation
  result.data.resize(checked_rgba_size(result.width, result.height));

  // Handle stride (bytes_per_line may differ from width * 4)
  int bpp = img->bits_per_pixel / 8;
  int depth = img->depth;
  for (int y = 0; y < cap_h; y++) {
    const auto *src =
        reinterpret_cast<const uint8_t *>(img->data) + y * img->bytes_per_line;
    auto *dst = result.data.data() + y * cap_w * 4;

    if (img->byte_order == LSBFirst && bpp == 4) {
      // BGRA format (most common on X11)
      bgra_to_rgba(src, dst, static_cast<size_t>(cap_w));
    } else {
      // Direct copy for RGBA or other formats
      std::memcpy(dst, src, static_cast<size_t>(cap_w) * 4);
    }

    // On 24-bit depth displays the alpha byte is unused (0); set to opaque.
    if (depth <= 24 && bpp == 4) {
      for (int x = 0; x < cap_w; x++)
        dst[x * 4 + 3] = 0xFF;
    }
  }

  // Cleanup
  if (use_shm) {
    XShmDetach(dpy, &shm_info);
    img->data = nullptr;
    XDestroyImage(img);
    shmdt(shm_info.shmaddr);
  } else {
    XDestroyImage(img);
  }

  XCloseDisplay(dpy);
  return result;
}

} // namespace frametap::internal
