#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>
#include <frametap/frametap.h>
#include <frametap/queue.h>

#include "helpers.h"

// Internal header for color conversion benchmark
#include "util/color.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace frametap;

// --- Section 6: Performance Tests ---
// These use Catch2 BENCHMARK macros. Run with: ./test_runner [benchmark]

TEST_CASE("BGRA to RGBA throughput", "[benchmark]") {
  constexpr size_t width = 1920;
  constexpr size_t height = 1080;
  constexpr size_t pixels = width * height;
  std::vector<uint8_t> buf(pixels * 4, 128);

  BENCHMARK("1080p in-place conversion") {
    internal::bgra_to_rgba(buf.data(), pixels);
    return buf[0]; // prevent optimization
  };

  // Src-dst variant
  std::vector<uint8_t> dst(pixels * 4);
  BENCHMARK("1080p src-dst conversion") {
    internal::bgra_to_rgba(buf.data(), dst.data(), pixels);
    return dst[0];
  };
}

TEST_CASE("Queue throughput", "[benchmark]") {
  BENCHMARK("100k push/pop single-threaded") {
    ThreadSafeQueue<int> q;
    for (int i = 0; i < 100000; ++i) {
      q.push(i);
    }
    for (int i = 0; i < 100000; ++i) {
      q.pop();
    }
    return 0;
  };
}

TEST_CASE("Screenshot latency", "[benchmark][integration]") {
  if (!test_helpers::has_display()) {
    SKIP("No display server available");
  }

  FrameTap ft;

  BENCHMARK("Full screen screenshot") { return ft.screenshot(); };
}

TEST_CASE("Streaming throughput", "[benchmark][integration]") {
  if (!test_helpers::has_display()) {
    SKIP("No display server available");
  }

  FrameTap ft;
  std::atomic<int> frame_count{0};

  ft.on_frame([&](const Frame &) { frame_count.fetch_add(1); });
  ft.start_async();

  // Measure for 3 seconds
  std::this_thread::sleep_for(std::chrono::seconds(3));
  ft.stop();

  int total = frame_count.load();
  double fps = total / 3.0;

  // Report FPS (not a pass/fail, just informational)
  WARN("Streaming FPS: " << fps << " (" << total << " frames in 3s)");
  CHECK(total > 0); // At least some frames received
}

TEST_CASE("Start-to-first-frame latency", "[benchmark][integration]") {
  if (!test_helpers::has_display()) {
    SKIP("No display server available");
  }

  FrameTap ft;
  std::atomic<bool> received{false};

  auto start_time = std::chrono::steady_clock::now();

  ft.on_frame([&](const Frame &) { received.store(true); });
  ft.start_async();

  // Wait up to 2 seconds
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!received &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  auto elapsed = std::chrono::steady_clock::now() - start_time;
  auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

  ft.stop();

  WARN("Start-to-first-frame: " << ms << " ms");
  CHECK(received);
  CHECK(ms < 2000); // Should be well under 2 seconds
}

TEST_CASE("Memory steady state", "[benchmark][integration]") {
  if (!test_helpers::has_display()) {
    SKIP("No display server available");
  }

  FrameTap ft;
  std::atomic<int> frame_count{0};

  ft.on_frame([&](const Frame &) { frame_count.fetch_add(1); });
  ft.start_async();

  // Stream for 5 seconds — if there's a memory leak, sanitizers will catch it
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  ft.stop();

  WARN("Received " << frame_count.load() << " frames during 5s memory test");
  CHECK(frame_count > 0);
}
