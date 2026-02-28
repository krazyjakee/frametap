#include "cli_args.h"
#include <frametap/frametap.h>
#include <frametap/version.h>

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

// --- Interactive helpers ---

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

// --- Usage ---

static void print_usage(const char *prog) {
  std::printf(
      "frametap %s\n"
      "\n"
      "Usage: %s [options]\n"
      "\n"
      "Capture modes (mutually exclusive):\n"
      "  --monitor <id>                   Capture a monitor by ID\n"
      "  --window <id>                    Capture a window by ID\n"
      "  --region <x>,<y>,<w>,<h>         Capture a screen region\n"
      "  --interactive                    Interactive mode (menu-driven)\n"
      "\n"
      "Options:\n"
      "  -o, --output <file>              Output file (default: screenshot.bmp)\n"
      "  --list-monitors                  List available monitors and exit\n"
      "  --list-windows                   List available windows and exit\n"
      "  --check-permissions              Check capture permissions and exit\n"
      "  -v, --version                    Show version and exit\n"
      "  -h, --help                       Show this help\n",
      FRAMETAP_VERSION, prog);
}

// --- Interactive mode (original behavior) ---

static int run_interactive(const char *output) {
  auto perms = frametap::check_permissions();
  std::printf("%s\n", perms.summary.c_str());
  for (const auto &d : perms.details)
    std::printf("  %s\n", d.c_str());

  if (perms.status == frametap::PermissionStatus::error) {
    std::fprintf(stderr, "Cannot proceed -- fix the issues above.\n");
    return 1;
  }

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

  if (save_bmp(output, image)) {
    std::printf("Saved %zux%zu screenshot to %s\n", image.width, image.height,
                output);
  } else {
    std::fprintf(stderr, "Failed to write %s\n", output);
    return 1;
  }

  return 0;
}

// --- Main ---

int main(int argc, char *argv[]) {
  auto args = cli::parse_args(argc, argv);

  if (!args.error.empty()) {
    std::fprintf(stderr, "Error: %s\n", args.error.c_str());
    std::fprintf(stderr, "Run '%s --help' for usage.\n", argv[0]);
    return 1;
  }

  if (args.action == cli::Action::help) {
    print_usage(argv[0]);
    return 0;
  }

  if (args.action == cli::Action::version) {
    std::printf("frametap %s\n", FRAMETAP_VERSION);
    return 0;
  }

  if (args.action == cli::Action::list_monitors) {
    auto monitors = frametap::get_monitors();
    if (monitors.empty()) {
      std::printf("No monitors found.\n");
      return 0;
    }
    for (const auto &m : monitors) {
      std::printf("[%d] %s  %dx%d @ %d,%d  scale=%.1f\n", m.id,
                  m.name.c_str(), m.width, m.height, m.x, m.y, m.scale);
    }
    return 0;
  }

  if (args.action == cli::Action::list_windows) {
    auto windows = frametap::get_windows();
    if (windows.empty()) {
      std::printf("No windows found.\n");
      return 0;
    }
    for (const auto &w : windows) {
      std::printf("[%llu] %s  %dx%d @ %d,%d\n",
                  static_cast<unsigned long long>(w.id), w.name.c_str(),
                  w.width, w.height, w.x, w.y);
    }
    return 0;
  }

  if (args.action == cli::Action::check_permissions) {
    auto perms = frametap::check_permissions();
    std::printf("%s\n", perms.summary.c_str());
    for (const auto &d : perms.details)
      std::printf("  %s\n", d.c_str());
    return perms.status == frametap::PermissionStatus::error ? 1 : 0;
  }

  if (args.mode == cli::CaptureMode::interactive)
    return run_interactive(args.output.c_str());

  // Permission check
  auto perms = frametap::check_permissions();
  if (perms.status == frametap::PermissionStatus::error) {
    std::fprintf(stderr, "%s\n", perms.summary.c_str());
    for (const auto &d : perms.details)
      std::fprintf(stderr, "  %s\n", d.c_str());
    return 1;
  }

  frametap::ImageData image;

  if (args.mode == cli::CaptureMode::monitor) {
    auto monitors = frametap::get_monitors();
    const frametap::Monitor *found = nullptr;
    for (const auto &m : monitors) {
      if (m.id == args.monitor_id) {
        found = &m;
        break;
      }
    }
    if (!found) {
      std::fprintf(stderr, "Error: monitor ID %d not found.\n",
                   args.monitor_id);
      std::fprintf(stderr, "Run '%s --list-monitors' to see available IDs.\n",
                   argv[0]);
      return 1;
    }
    frametap::FrameTap tap(*found);
    image = tap.screenshot();

  } else if (args.mode == cli::CaptureMode::window) {
    auto windows = frametap::get_windows();
    const frametap::Window *found = nullptr;
    for (const auto &w : windows) {
      if (w.id == args.window_id) {
        found = &w;
        break;
      }
    }
    if (!found) {
      std::fprintf(stderr, "Error: window ID %llu not found.\n",
                   static_cast<unsigned long long>(args.window_id));
      std::fprintf(stderr, "Run '%s --list-windows' to see available IDs.\n",
                   argv[0]);
      return 1;
    }
    frametap::FrameTap tap(*found);
    image = tap.screenshot();

  } else if (args.mode == cli::CaptureMode::region) {
    frametap::Rect rect{args.region.x, args.region.y, args.region.w,
                        args.region.h};
    frametap::FrameTap tap(rect);
    image = tap.screenshot();
  }

  if (image.data.empty()) {
    std::fprintf(stderr, "Screenshot returned no data.\n");
    return 1;
  }

  if (save_bmp(args.output.c_str(), image)) {
    std::printf("Saved %zux%zu screenshot to %s\n", image.width, image.height,
                args.output.c_str());
  } else {
    std::fprintf(stderr, "Failed to write %s\n", args.output.c_str());
    return 1;
  }

  return 0;
}
