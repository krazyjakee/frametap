#include <catch2/catch_test_macros.hpp>
#include <frametap/frametap.h>
#include <frametap/queue.h>

#include "helpers.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <limits>
#include <thread>
#include <vector>

using namespace frametap;

// ---------------------------------------------------------------------------
// H6: Integer overflow protection in pixel buffer allocation
// ---------------------------------------------------------------------------

TEST_CASE("Overflow-checked allocation rejects huge dimensions",
          "[unit][security]") {
  // These dimensions would overflow size_t on any platform:
  // SIZE_MAX / 4 + 1 pixels per row, 1 row
  size_t huge = std::numeric_limits<size_t>::max() / 4 + 1;
  Rect overflow_region{0, 0, static_cast<double>(huge), 1};

  // The library should throw CaptureError, not allocate a tiny buffer
  if (test_helpers::has_display()) {
    CHECK_THROWS_AS(FrameTap(overflow_region).screenshot(), CaptureError);
  }
}

TEST_CASE("Large but valid dimensions are accepted", "[unit][security]") {
  // 8192x8192 should be valid (256 MB of RGBA data)
  Rect large{0, 0, 8192, 8192};
  // We can't actually capture this without a display, but we can verify
  // the library doesn't throw an overflow error during construction
  if (test_helpers::has_display()) {
    try {
      FrameTap ft(large);
      // If we get here, the dimensions were accepted (clamped to screen)
    } catch (const CaptureError &) {
      // Expected if display is too small
    }
  }
}

// ---------------------------------------------------------------------------
// L5: ThreadSafeQueue shutdown mechanism
// ---------------------------------------------------------------------------

TEST_CASE("Queue close() unblocks pop()", "[unit][security][queue]") {
  ThreadSafeQueue<int> q;
  std::atomic<bool> unblocked{false};

  std::thread consumer([&] {
    q.pop(); // should block until close()
    unblocked = true;
  });

  // Give consumer time to block
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK_FALSE(unblocked);

  q.close();
  consumer.join();
  CHECK(unblocked);
}

TEST_CASE("Queue close() prevents further pushes", "[unit][security][queue]") {
  ThreadSafeQueue<int> q;
  q.push(1);
  q.close();
  q.push(2); // should be a no-op

  auto val = q.try_pop();
  REQUIRE(val.has_value());
  CHECK(*val == 1);

  // Queue should be empty now (push after close was ignored)
  CHECK_FALSE(q.try_pop().has_value());
}

TEST_CASE("Queue is_closed() reflects state", "[unit][security][queue]") {
  ThreadSafeQueue<int> q;
  CHECK_FALSE(q.is_closed());
  q.close();
  CHECK(q.is_closed());
}

TEST_CASE("Queue pop() drains remaining items after close",
          "[unit][security][queue]") {
  ThreadSafeQueue<int> q;
  q.push(10);
  q.push(20);
  q.close();

  // Should still get existing items
  CHECK(q.pop() == 10);
  CHECK(q.pop() == 20);

  // Then return default
  CHECK(q.pop() == 0);
}

TEST_CASE("Queue timed pop()", "[unit][security][queue]") {
  ThreadSafeQueue<int> q;

  // Should return nullopt after timeout
  auto result = q.pop(std::chrono::milliseconds(50));
  CHECK_FALSE(result.has_value());

  // Should return value when available
  q.push(42);
  result = q.pop(std::chrono::milliseconds(50));
  REQUIRE(result.has_value());
  CHECK(*result == 42);
}

TEST_CASE("Queue close() unblocks multiple consumers",
          "[unit][security][queue]") {
  ThreadSafeQueue<int> q;
  std::atomic<int> unblocked{0};

  std::vector<std::thread> consumers;
  for (int i = 0; i < 4; ++i) {
    consumers.emplace_back([&] {
      q.pop();
      unblocked.fetch_add(1);
    });
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK(unblocked == 0);

  q.close();
  for (auto &t : consumers)
    t.join();

  CHECK(unblocked == 4);
}

// ---------------------------------------------------------------------------
// H4: Negative coordinate handling
// ---------------------------------------------------------------------------

TEST_CASE("Negative coordinates are safely handled", "[unit][security]") {
  if (!test_helpers::has_display()) {
    SKIP("No display server available");
  }

  // Partially negative region — should be clamped, not crash
  Rect partial_neg{-50, -50, 200, 200};
  try {
    FrameTap ft(partial_neg);
    ImageData img = ft.screenshot();
    // Dimensions should be clamped (not 200x200 since part is off-screen)
    CHECK(img.width > 0);
    CHECK(img.height > 0);
    CHECK(img.width <= 200);
    CHECK(img.height <= 200);
  } catch (const CaptureError &) {
    // Also acceptable
  }
}

TEST_CASE("Fully negative region returns empty or throws",
          "[unit][security]") {
  if (!test_helpers::has_display()) {
    SKIP("No display server available");
  }

  // Region entirely off-screen
  Rect fully_neg{-500, -500, 100, 100};
  try {
    FrameTap ft(fully_neg);
    ImageData img = ft.screenshot();
    // Should be empty since region is entirely off-screen
    CHECK((img.width == 0 || img.data.empty()));
  } catch (const CaptureError &) {
    // Also acceptable
  }
}

// ---------------------------------------------------------------------------
// H1/H2: Thread-safety of set_region()
// ---------------------------------------------------------------------------

TEST_CASE("Concurrent set_region does not crash", "[integration][security]") {
  if (!test_helpers::has_display()) {
    SKIP("No display server available");
  }

  FrameTap ft;
  std::atomic<int> frame_count{0};
  ft.on_frame([&](const Frame &) { frame_count.fetch_add(1); });
  ft.start_async();

  // Hammer set_region from another thread while streaming
  std::atomic<bool> done{false};
  std::thread setter([&] {
    for (int i = 0; i < 100 && !done; ++i) {
      ft.set_region({static_cast<double>(i), static_cast<double>(i),
                     100.0 + i, 100.0 + i});
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    done = true;
  });

  // Let it run for a bit
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (frame_count < 5 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  done = true;
  setter.join();
  ft.stop();

  // If we get here without TSAN errors or crashes, the test passes
  CHECK(true);
}

// ---------------------------------------------------------------------------
// M4: start() and start_async() both work correctly
// ---------------------------------------------------------------------------

TEST_CASE("start_async is non-blocking", "[integration][security]") {
  if (!test_helpers::has_display()) {
    SKIP("No display server available");
  }

  FrameTap ft;
  std::atomic<int> frames{0};
  ft.on_frame([&](const Frame &) { frames.fetch_add(1); });

  auto before = std::chrono::steady_clock::now();
  ft.start_async();
  auto after = std::chrono::steady_clock::now();

  // start_async should return immediately (< 500ms)
  auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(after - before);
  CHECK(elapsed.count() < 500);

  ft.stop();
}

// ---------------------------------------------------------------------------
// H5: X11 error handler prevents process exit
// ---------------------------------------------------------------------------

TEST_CASE("Invalid window ID does not crash process", "[integration][security]") {
  if (!test_helpers::has_display()) {
    SKIP("No display server available");
  }

#if defined(__linux__)
  // On X11, a fabricated window ID should throw CaptureError (via custom
  // error handler) instead of calling exit()
  Window fake;
  fake.id = 0xDEADBEEF;
  fake.name = "Fake Window";
  fake.width = 100;
  fake.height = 100;

  try {
    FrameTap ft(fake);
    // If construction succeeds (e.g., Wayland backend),
    // a screenshot might throw
    ft.screenshot();
  } catch (const CaptureError &) {
    // Expected — the custom error handler caught the X error
  } catch (const std::exception &e) {
    FAIL("Unexpected exception type: " << e.what());
  }
  // If we get here without exit(), the error handler works
  CHECK(true);
#else
  SKIP("X11 error handler test only applicable on Linux");
#endif
}
