#include <frametap/frametap.h>
#include <frametap/queue.h>

#include <chrono>
#include <cstdio>
#include <thread>

int main() {
  // --- Permission check ---
  {
    auto perms = frametap::check_permissions();
    std::printf("Platform: %s\n", perms.summary.c_str());
    for (const auto &detail : perms.details)
      std::printf("  %s\n", detail.c_str());

    if (perms.status == frametap::PermissionStatus::error) {
      std::fprintf(stderr, "Cannot proceed — fix the issues above.\n");
      return 1;
    }
  }

  // --- Monitor and window enumeration ---
  {
    auto monitors = frametap::get_monitors();
    std::printf("\nMonitors (%zu):\n", monitors.size());
    for (const auto &m : monitors) {
      std::printf("  [%d] %s  %dx%d+%d+%d  scale=%.1f\n", m.id,
                  m.name.c_str(), m.width, m.height, m.x, m.y, m.scale);
    }

    auto windows = frametap::get_windows();
    std::printf("\nWindows (%zu):\n", windows.size());
    for (const auto &w : windows) {
      std::printf("  [%llu] %s  %dx%d+%d+%d\n",
                  static_cast<unsigned long long>(w.id), w.name.c_str(),
                  w.width, w.height, w.x, w.y);
    }
  }

  // --- Screenshot ---
  {
    std::printf("\nTaking screenshot...\n");
    frametap::FrameTap tap;
    auto image = tap.screenshot();
    std::printf("Screenshot: %zux%zu (%zu bytes RGBA)\n", image.width,
                image.height, image.data.size());
  }

  // --- Streaming capture ---
  {
    std::printf("\nStreaming for 2 seconds...\n");
    frametap::ThreadSafeQueue<frametap::Frame> queue;

    frametap::FrameTap tap;
    tap.on_frame([&queue](const frametap::Frame &frame) { queue.push(frame); });

    tap.start_async();

    auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    int count = 0;
    while (std::chrono::steady_clock::now() < deadline) {
      if (auto frame = queue.try_pop()) {
        ++count;
        if (count <= 5 || count % 10 == 0) {
          std::printf("  Frame %d: %zux%zu  %.1f ms\n", count,
                      frame->image.width, frame->image.height,
                      frame->duration_ms);
        }
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    }

    // --- Pause / resume ---
    std::printf("\nPausing capture...\n");
    tap.pause();
    std::printf("  is_paused = %s\n", tap.is_paused() ? "true" : "false");

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    int paused_count = 0;
    while (auto frame = queue.try_pop())
      ++paused_count;

    std::printf("Resuming capture...\n");
    tap.resume();

    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < deadline) {
      if (auto frame = queue.try_pop()) {
        ++count;
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    }

    tap.stop();
    std::printf("Captured %d total frames (drained %d during pause)\n", count,
                paused_count);
  }

  std::printf("\nDone.\n");
  return 0;
}
