// Lock-free, allocation-free hand-off between threads. The only ways data crosses into or out of
// the audio thread (architecture.md, "Realtime rules").
// Owner: t3.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace op1 {

// Bounded single-producer single-consumer queue of trivially copyable items. push() on exactly one
// thread, pop() on exactly one other. Never blocks, never allocates; push() returns false when full.
template <typename T, size_t N>
class SpscQueue {
  static_assert((N & (N - 1)) == 0, "N must be a power of two");
  static_assert(std::is_trivially_copyable_v<T>, "items are copied by value across threads");
  T buf_[N];
  alignas(64) std::atomic<size_t> head_{0};
  alignas(64) std::atomic<size_t> tail_{0};

 public:
  bool push(const T& v) noexcept {
    const size_t h = head_.load(std::memory_order_relaxed);
    if (h - tail_.load(std::memory_order_acquire) == N) return false;
    buf_[h & (N - 1)] = v;
    head_.store(h + 1, std::memory_order_release);
    return true;
  }
  bool pop(T& v) noexcept {
    const size_t t = tail_.load(std::memory_order_relaxed);
    if (t == head_.load(std::memory_order_acquire)) return false;
    v = buf_[t & (N - 1)];
    tail_.store(t + 1, std::memory_order_release);
    return true;
  }
  size_t size_approx() const noexcept {
    return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
  }
};

// Wait-free triple buffer: one writer publishes whole snapshots, one reader always gets the newest
// complete one. Neither side ever waits for the other.
template <typename T>
class TripleBuffer {
  static_assert(std::is_trivially_copyable_v<T>);
  T slot_[3]{};
  std::atomic<uint32_t> middle_{1};
  uint32_t back_ = 0;   // writer only
  uint32_t front_ = 2;  // reader only
  static constexpr uint32_t kFresh = 4;

 public:
  T& back() noexcept { return slot_[back_]; }  // writer: fill, then publish()
  void publish() noexcept { back_ = middle_.exchange(back_ | kFresh, std::memory_order_acq_rel) & 3; }
  const T& read() noexcept {  // reader
    if (middle_.load(std::memory_order_acquire) & kFresh)
      front_ = middle_.exchange(front_, std::memory_order_acq_rel) & 3;
    return slot_[front_];
  }
};

}  // namespace op1
