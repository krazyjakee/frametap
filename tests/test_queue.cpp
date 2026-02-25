#include <catch2/catch_test_macros.hpp>
#include <frametap/queue.h>

#include <algorithm>
#include <atomic>
#include <numeric>
#include <set>
#include <thread>
#include <vector>

using namespace frametap;

// --- Section 2.2: ThreadSafeQueue ---

TEST_CASE("push and pop", "[unit][queue]") {
  ThreadSafeQueue<int> q;
  q.push(42);
  CHECK(q.pop() == 42);
}

TEST_CASE("FIFO order", "[unit][queue]") {
  ThreadSafeQueue<int> q;
  q.push(1);
  q.push(2);
  q.push(3);

  CHECK(q.pop() == 1);
  CHECK(q.pop() == 2);
  CHECK(q.pop() == 3);
}

TEST_CASE("pop blocks until push", "[unit][queue]") {
  ThreadSafeQueue<int> q;
  std::atomic<bool> received{false};

  std::thread consumer([&] {
    int val = q.pop(); // should block
    CHECK(val == 99);
    received = true;
  });

  // Give consumer time to block
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK_FALSE(received);

  q.push(99);
  consumer.join();
  CHECK(received);
}

TEST_CASE("try_pop empty", "[unit][queue]") {
  ThreadSafeQueue<int> q;
  auto result = q.try_pop();
  CHECK_FALSE(result.has_value());
}

TEST_CASE("try_pop non-empty", "[unit][queue]") {
  ThreadSafeQueue<int> q;
  q.push(7);
  auto result = q.try_pop();
  REQUIRE(result.has_value());
  CHECK(*result == 7);
  CHECK(q.empty());
}

TEST_CASE("empty and size", "[unit][queue]") {
  ThreadSafeQueue<int> q;
  CHECK(q.empty());
  CHECK(q.size() == 0);

  q.push(1);
  CHECK_FALSE(q.empty());
  CHECK(q.size() == 1);

  q.push(2);
  CHECK(q.size() == 2);

  q.pop();
  CHECK(q.size() == 1);

  q.pop();
  CHECK(q.empty());
}

TEST_CASE("multi-producer", "[unit][queue]") {
  ThreadSafeQueue<int> q;
  constexpr int num_threads = 4;
  constexpr int items_per_thread = 100;

  std::vector<std::thread> producers;
  for (int t = 0; t < num_threads; ++t) {
    producers.emplace_back([&q, t] {
      for (int i = 0; i < items_per_thread; ++i) {
        q.push(t * items_per_thread + i);
      }
    });
  }

  for (auto &t : producers)
    t.join();

  // All items should be present
  CHECK(q.size() == num_threads * items_per_thread);

  std::set<int> received;
  for (int i = 0; i < num_threads * items_per_thread; ++i) {
    received.insert(q.pop());
  }
  CHECK(received.size() == num_threads * items_per_thread);
}

TEST_CASE("multi-consumer", "[unit][queue]") {
  ThreadSafeQueue<int> q;
  constexpr int total_items = 400;
  constexpr int num_consumers = 4;

  // Pre-fill queue
  for (int i = 0; i < total_items; ++i) {
    q.push(i);
  }

  std::mutex results_mutex;
  std::vector<int> all_results;

  std::vector<std::thread> consumers;
  for (int c = 0; c < num_consumers; ++c) {
    consumers.emplace_back([&] {
      std::vector<int> local;
      while (true) {
        auto val = q.try_pop();
        if (!val.has_value())
          break;
        local.push_back(*val);
      }
      std::lock_guard lock(results_mutex);
      all_results.insert(all_results.end(), local.begin(), local.end());
    });
  }

  for (auto &t : consumers)
    t.join();

  // Every item should be received exactly once
  std::sort(all_results.begin(), all_results.end());
  CHECK(all_results.size() == total_items);

  // Check no duplicates
  auto it = std::unique(all_results.begin(), all_results.end());
  CHECK(it == all_results.end());
}

TEST_CASE("stress test", "[unit][queue]") {
  ThreadSafeQueue<int> q;
  constexpr int cycles = 10000;
  constexpr int num_threads = 4;

  std::atomic<int> total_pushed{0};
  std::atomic<int> total_popped{0};

  // Producers
  std::vector<std::thread> threads;
  for (int t = 0; t < num_threads; ++t) {
    threads.emplace_back([&q, &total_pushed] {
      for (int i = 0; i < cycles; ++i) {
        q.push(i);
        total_pushed.fetch_add(1);
      }
    });
  }

  // Consumers
  std::atomic<bool> done{false};
  for (int t = 0; t < num_threads; ++t) {
    threads.emplace_back([&q, &total_popped, &done] {
      while (!done || !q.empty()) {
        auto val = q.try_pop();
        if (val.has_value()) {
          total_popped.fetch_add(1);
        } else {
          std::this_thread::yield();
        }
      }
    });
  }

  // Wait for producers to finish, then signal consumers
  for (int i = 0; i < num_threads; ++i)
    threads[i].join();
  done = true;
  for (int i = num_threads; i < 2 * num_threads; ++i)
    threads[i].join();

  CHECK(total_pushed == num_threads * cycles);
  CHECK(total_popped == num_threads * cycles);
}

TEST_CASE("move-only types", "[unit][queue]") {
  ThreadSafeQueue<std::unique_ptr<int>> q;

  q.push(std::make_unique<int>(42));
  auto val = q.pop();
  REQUIRE(val != nullptr);
  CHECK(*val == 42);
}
