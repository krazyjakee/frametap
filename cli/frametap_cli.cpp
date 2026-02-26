#include <frametap/frametap.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

static bool save_bmp(const char *path, const frametap::ImageData &img) {
  const uint32_t w = static_cast<uint32_t>(img.width);
  const uint32_t h = static_cast<uint32_t>(img.height);
  const uint32_t row_bytes = w * 3;
  const uint32_t pad = (4 - (row_bytes % 4)) % 4;
  const uint32_t stride = row_bytes + pad;
  const uint32_t pixel_size = stride * h;
  const uint32_t file_size = 54 + pixel_size;

  std::ofstream out(path, std::ios::binary);
  if (!out)
    return false;

  // BMP file header (14 bytes)
  uint8_t hdr[54] = {};
  hdr[0] = 'B';
  hdr[1] = 'M';
  std::memcpy(hdr + 2, &file_size, 4);
  uint32_t offset = 54;
  std::memcpy(hdr + 10, &offset, 4);

  // DIB header (BITMAPINFOHEADER, 40 bytes)
  uint32_t dib_size = 40;
  std::memcpy(hdr + 14, &dib_size, 4);
  std::memcpy(hdr + 18, &w, 4);
  std::memcpy(hdr + 22, &h, 4);
  uint16_t planes = 1;
  std::memcpy(hdr + 26, &planes, 2);
  uint16_t bpp = 24;
  std::memcpy(hdr + 28, &bpp, 2);
  std::memcpy(hdr + 34, &pixel_size, 4);

  out.write(reinterpret_cast<const char *>(hdr), 54);

  // Pixel data: BMP stores rows bottom-to-top, BGR order
  const uint8_t zero[3] = {};
  for (uint32_t y = h; y-- > 0;) {
    for (uint32_t x = 0; x < w; ++x) {
      size_t i = (static_cast<size_t>(y) * w + x) * 4; // RGBA offset
      uint8_t bgr[3] = {img.data[i + 2], img.data[i + 1], img.data[i]};
      out.write(reinterpret_cast<const char *>(bgr), 3);
    }
    if (pad > 0)
      out.write(reinterpret_cast<const char *>(zero), pad);
  }

  return out.good();
}

static int read_int(const char *prompt) {
  std::printf("%s", prompt);
  std::fflush(stdout);
  std::string line;
  if (!std::getline(std::cin, line))
    return -1;
  try {
    return std::stoi(line);
  } catch (...) {
    return -1;
  }
}

static double read_double(const char *prompt) {
  std::printf("%s", prompt);
  std::fflush(stdout);
  std::string line;
  if (!std::getline(std::cin, line))
    return -1;
  try {
    return std::stod(line);
  } catch (...) {
    return -1;
  }
}

int main() {
  // Permission check
  auto perms = frametap::check_permissions();
  std::printf("%s\n", perms.summary.c_str());
  for (const auto &d : perms.details)
    std::printf("  %s\n", d.c_str());

  if (perms.status == frametap::PermissionStatus::error) {
    std::fprintf(stderr, "Cannot proceed -- fix the issues above.\n");
    return 1;
  }

  // Interactive menu
  std::printf("\nCapture mode:\n");
  std::printf("  1) Screen (pick a monitor)\n");
  std::printf("  2) Window (pick a window)\n");
  std::printf("  3) Region (enter coordinates)\n");
  int mode = read_int("\nChoice [1-3]: ");

  frametap::ImageData image;

  if (mode == 1) {
    auto monitors = frametap::get_monitors();
    if (monitors.empty()) {
      std::fprintf(stderr, "No monitors found.\n");
      return 1;
    }
    std::printf("\nMonitors:\n");
    for (size_t i = 0; i < monitors.size(); ++i) {
      const auto &m = monitors[i];
      std::printf("  %zu) [%d] %s  %dx%d  scale=%.1f\n", i + 1, m.id,
                  m.name.c_str(), m.width, m.height, m.scale);
    }
    int pick = read_int("\nMonitor number: ");
    if (pick < 1 || pick > static_cast<int>(monitors.size())) {
      std::fprintf(stderr, "Invalid selection.\n");
      return 1;
    }
    frametap::FrameTap tap(monitors[pick - 1]);
    image = tap.screenshot();

  } else if (mode == 2) {
    auto windows = frametap::get_windows();
    if (windows.empty()) {
      std::fprintf(stderr, "No windows found.\n");
      return 1;
    }
    std::printf("\nWindows:\n");
    for (size_t i = 0; i < windows.size(); ++i) {
      const auto &w = windows[i];
      std::printf("  %zu) %s  %dx%d\n", i + 1, w.name.c_str(), w.width,
                  w.height);
    }
    int pick = read_int("\nWindow number: ");
    if (pick < 1 || pick > static_cast<int>(windows.size())) {
      std::fprintf(stderr, "Invalid selection.\n");
      return 1;
    }
    frametap::FrameTap tap(windows[pick - 1]);
    image = tap.screenshot();

  } else if (mode == 3) {
    double x = read_double("x: ");
    double y = read_double("y: ");
    double w = read_double("width: ");
    double h = read_double("height: ");
    if (w <= 0 || h <= 0) {
      std::fprintf(stderr, "Invalid region.\n");
      return 1;
    }
    frametap::FrameTap tap(frametap::Rect{x, y, w, h});
    image = tap.screenshot();

  } else {
    std::fprintf(stderr, "Invalid choice.\n");
    return 1;
  }

  if (image.data.empty()) {
    std::fprintf(stderr, "Screenshot returned no data.\n");
    return 1;
  }

  const char *filename = "screenshot.bmp";
  if (save_bmp(filename, image)) {
    std::printf("Saved %zux%zu screenshot to %s\n", image.width, image.height,
                filename);
  } else {
    std::fprintf(stderr, "Failed to write %s\n", filename);
    return 1;
  }

  return 0;
}
