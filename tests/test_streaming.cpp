#include <catch2/catch_test_macros.hpp>
#include <frametap/frametap.h>
#include <frametap/queue.h>

#include "helpers.h"

#include <atomic>
#include <chrono>
#include <thread>

using namespace frametap;

// --- Section 3.3: Streaming capture ---

TEST_CASE("Start and receive frames", "[integration][streaming]") {
  if (!test_helpers::has_display()) {
    SKIP("No display server available");
  }

  FrameTap ft;
  std::atomic<int> frame_count{0};

  ft.on_frame([&](const Frame &) { frame_count.fetch_add(1); });
  ft.start_async();

  // Wait up to 2 seconds for at least 1 frame
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (frame_count == 0 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  ft.stop();
  CHECK(frame_count > 0);
}

TEST_CASE("Frame dimensions valid", "[integration][streaming]") {
  if (!test_helpers::has_display()) {
    SKIP("No display server available");
  }

  FrameTap ft;
  ThreadSafeQueue<Frame> frames;

  ft.on_frame([&](const Frame &f) {
    // Copy frame data since callback must be fast
    Frame copy;
    copy.image.width = f.image.width;
    copy.image.height = f.image.height;
    copy.image.data = f.image.data;
    copy.duration_ms = f.duration_ms;
    frames.push(std::move(copy));
  });
  ft.start_async();

  // Wait for at least one frame
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (frames.empty() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  ft.stop();

  auto frame = frames.try_pop();
  REQUIRE(frame.has_value());
  CHECK(frame->image.width > 0);
  CHECK(frame->image.height > 0);
  CHECK(frame->image.data.size() ==
        frame->image.width * frame->image.height * 4);
}

TEST_CASE("Frame duration positive", "[integration][streaming]") {
  if (!test_helpers::has_display()) {
    SKIP("No display server available");
  }

  FrameTap ft;
  std::atomic<double> last_duration{0};
  std::atomic<int> frame_count{0};

  ft.on_frame([&](const Frame &f) {
    if (frame_count.fetch_add(1) > 0) {
      // duration_ms is meaningful after the first frame
      last_duration.store(f.duration_ms);
    }
  });
  ft.start_async();

  // Wait for at least 2 frames
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (frame_count < 2 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  ft.stop();

  if (frame_count >= 2) {
    CHECK(last_duration.load() > 0);
  }
}

TEST_CASE("Stop halts delivery", "[integration][streaming]") {
  if (!test_helpers::has_display()) {
    SKIP("No display server available");
  }

  FrameTap ft;
  std::atomic<int> frame_count{0};

  ft.on_frame([&](const Frame &) { frame_count.fetch_add(1); });
  ft.start_async();

  // Wait for some frames
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (frame_count == 0 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  ft.stop();
  int count_at_stop = frame_count.load();

  // Wait 500ms after stop — no new frames should arrive
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  CHECK(frame_count.load() == count_at_stop);
}

TEST_CASE("Pause suspends", "[integration][streaming]") {
  if (!test_helpers::has_display()) {
    SKIP("No display server available");
  }

  FrameTap ft;
  std::atomic<int> frame_count{0};

  ft.on_frame([&](const Frame &) { frame_count.fetch_add(1); });
  ft.start_async();

  // Wait for some frames
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (frame_count == 0 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  ft.pause();
  // Allow any in-flight frames to arrive after pause (longer under sanitizers)
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  int count_after_pause = frame_count.load();

  // Wait 500ms — no new frames should arrive during pause
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  CHECK(frame_count.load() == count_after_pause);

  ft.stop();
}

TEST_CASE("Resume restarts", "[integration][streaming]") {
  if (!test_helpers::has_display()) {
    SKIP("No display server available");
  }

  FrameTap ft;
  std::atomic<int> frame_count{0};

  ft.on_frame([&](const Frame &) { frame_count.fetch_add(1); });
  ft.start_async();

  // Wait for at least 2 frames to confirm streaming is active
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (frame_count.load() < 2 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  ft.pause();
  // Let any in-flight callbacks settle before reading the count
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  int count_at_pause = frame_count.load();

  ft.resume();

  // Wait up to 3 seconds for new frames after resume
  deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (frame_count.load() <= count_at_pause &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  CHECK(frame_count.load() > count_at_pause);

  ft.stop();
}

TEST_CASE("is_paused state", "[integration][streaming]") {
  if (!test_helpers::has_display()) {
    SKIP("No display server available");
  }

  FrameTap ft;
  ft.on_frame([](const Frame &) {});
  ft.start_async();

  CHECK_FALSE(ft.is_paused());

  ft.pause();
  CHECK(ft.is_paused());

  ft.resume();
  CHECK_FALSE(ft.is_paused());

  ft.stop();
}

TEST_CASE("start_async non-blocking", "[integration][streaming]") {
  if (!test_helpers::has_display()) {
    SKIP("No display server available");
  }

  FrameTap ft;
  ft.on_frame([](const Frame &) {});

  auto start = std::chrono::steady_clock::now();
  ft.start_async();
  auto elapsed = std::chrono::steady_clock::now() - start;

  // start_async should return within 500ms (generous threshold)
  CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)
            .count() < 500);

  ft.stop();
}

TEST_CASE("Callback thread safety", "[integration][streaming]") {
  if (!test_helpers::has_display()) {
    SKIP("No display server available");
  }

  FrameTap ft;
  std::atomic<std::thread::id> callback_thread_id{};
  std::atomic<bool> received{false};

  ft.on_frame([&](const Frame &) {
    if (!received.exchange(true)) {
      callback_thread_id.store(std::this_thread::get_id());
    }
  });
  ft.start_async();

  // Wait for a frame
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!received &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  ft.stop();

  if (received) {
    // Callback should fire on a non-main thread
    CHECK(callback_thread_id.load() != std::this_thread::get_id());
  }
}
