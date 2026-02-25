#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>

namespace frametap {

template <typename T> class ThreadSafeQueue {
public:
  void push(T value) {
    {
      std::lock_guard lock(mutex_);
      if (closed_)
        return;
      deque_.push_back(std::move(value));
    }
    cv_.notify_one();
  }

  T pop() {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [this] { return !deque_.empty() || closed_; });
    if (deque_.empty())
      return T{};
    T value = std::move(deque_.front());
    deque_.pop_front();
    return value;
  }

  template <typename Rep, typename Period>
  std::optional<T> pop(std::chrono::duration<Rep, Period> timeout) {
    std::unique_lock lock(mutex_);
    if (!cv_.wait_for(lock, timeout,
                      [this] { return !deque_.empty() || closed_; }))
      return std::nullopt;
    if (deque_.empty())
      return std::nullopt;
    T value = std::move(deque_.front());
    deque_.pop_front();
    return value;
  }

  std::optional<T> try_pop() {
    std::lock_guard lock(mutex_);
    if (deque_.empty())
      return std::nullopt;
    T value = std::move(deque_.front());
    deque_.pop_front();
    return value;
  }

  // Unblocks all threads waiting in pop(). After close(), push() is a no-op
  // and pop() returns T{} immediately once the queue is drained.
  void close() {
    {
      std::lock_guard lock(mutex_);
      closed_ = true;
    }
    cv_.notify_all();
  }

  [[nodiscard]] bool is_closed() const {
    std::lock_guard lock(mutex_);
    return closed_;
  }

  [[nodiscard]] bool empty() const {
    std::lock_guard lock(mutex_);
    return deque_.empty();
  }

  [[nodiscard]] size_t size() const {
    std::lock_guard lock(mutex_);
    return deque_.size();
  }

private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<T> deque_;
  bool closed_ = false;
};

} // namespace frametap
