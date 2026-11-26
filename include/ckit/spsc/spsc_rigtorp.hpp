// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <ckit/spsc/spsc_config.hpp>

namespace spsc::rigtorp {

template <typename T, typename Allocator = std::allocator<T>>
class SPSCQueue {

private:

  static constexpr size_t kPadding = (kCacheLineSize - 1) / sizeof(T) + 1;
  size_t capacity_;
  T *slots_;
  Allocator allocator_;

  alignas(kCacheLineSize) std::atomic<size_t> writeIdx_ = {0};
  alignas(kCacheLineSize) size_t readIdxCache_ = 0;
  alignas(kCacheLineSize) std::atomic<size_t> readIdx_ = {0};
  alignas(kCacheLineSize) size_t writeIdxCache_ = 0;

public:
  explicit SPSCQueue(const size_t capacity, const Allocator &allocator = Allocator())
      : capacity_(capacity), allocator_(allocator) {
    if (capacity_ < 1) {
      capacity_ = 1;
    }
    if (capacity_ > SIZE_MAX - 2 * kPadding - 1) {
      capacity_ = SIZE_MAX - 2 * kPadding - 1;
    }
    capacity_++;
    slots_ = std::allocator_traits<Allocator>::allocate(allocator_, capacity_ + 2 * kPadding);

    static_assert(alignof(SPSCQueue<T>) == kCacheLineSize, "");
    assert(reinterpret_cast<char *>(&readIdx_) - reinterpret_cast<char *>(&writeIdx_) >=
           static_cast<std::ptrdiff_t>(kCacheLineSize));
  }
  ~SPSCQueue() {
    while (front()) {
      pop();
    }
    std::allocator_traits<Allocator>::deallocate(allocator_, slots_, capacity_ + 2 * kPadding);
  }

  SPSCQueue(const SPSCQueue &) = delete;
  SPSCQueue &operator=(const SPSCQueue &) = delete;

  template <typename... Args>
  void emplace(Args &&...args) noexcept(std::is_nothrow_constructible_v<T, Args &&...>) {
    static_assert(std::is_constructible_v<T, Args &&...>, "T must be constructible with Args&&...");
    auto const writeIdx = writeIdx_.load(std::memory_order_relaxed);
    auto nextWriteIdx = writeIdx + 1;
    if (nextWriteIdx == capacity_) {
      nextWriteIdx = 0;
    }

    while (nextWriteIdx == readIdxCache_) {
      readIdxCache_ = readIdx_.load(std::memory_order_acquire);
    }
    new (&slots_[writeIdx + kPadding]) T(std::forward<Args>(args)...);

    writeIdx_.store(nextWriteIdx, std::memory_order_release);
  }

  template <typename... Args>
  bool try_emplace(Args &&...args) noexcept(std::is_nothrow_constructible_v<T, Args &&...>) {
    static_assert(std::is_constructible_v<T, Args &&...>, "T must be constructible with Args&&...");
    auto const writeIdx = writeIdx_.load(std::memory_order_relaxed);
    auto nextWriteIdx = writeIdx + 1;
    if (nextWriteIdx == capacity_) {
      nextWriteIdx = 0;
    }
    if (nextWriteIdx == readIdxCache_) {
      readIdxCache_ = readIdx_.load(std::memory_order_acquire);

      if (nextWriteIdx == readIdxCache_) {
        return false;
      }
    }
    new (&slots_[writeIdx + kPadding]) T(std::forward<Args>(args)...);
    writeIdx_.store(nextWriteIdx, std::memory_order_release);
    return true;
  }

  void
  push(const T &v) noexcept(std::is_nothrow_copy_constructible_v<T>) {
    static_assert(std::is_copy_constructible_v<T>, "T must be copy constructible");
    emplace(v);
  }

  template <typename P, typename = std::enable_if_t<std::is_constructible_v<T, P &&>>>
  void
  push(P &&v) noexcept(std::is_nothrow_constructible_v<T, P &&>) {
    emplace(std::forward<P>(v));
  }
  bool
  try_push(const T &v) noexcept(std::is_nothrow_copy_constructible_v<T>) {
    static_assert(std::is_copy_constructible_v<T>, "T must be copy constructible");
    return try_emplace(v);
  }
  template <typename P, typename = typename std::enable_if<std::is_constructible_v<T, P &&>>>
  bool
  try_push(P &&v) noexcept(std::is_nothrow_constructible_v<T, P &&>) {
    return try_emplace(std::forward<P>(v));
  }

  T *front() noexcept {
    auto const readIdx = readIdx_.load(std::memory_order_relaxed);
    if (readIdx == writeIdxCache_) {
      writeIdxCache_ = writeIdx_.load(std::memory_order_acquire);
      if (writeIdxCache_ == readIdx) {
        return nullptr;
      }
    }
    return &slots_[readIdx + kPadding];
  }
  void pop() noexcept {
    static_assert(std::is_nothrow_destructible_v<T>, "T must be nothrow destructible");
    auto const readIdx = readIdx_.load(std::memory_order_relaxed);
    assert(writeIdx_.load(std::memory_order_acquire) != readIdx && "Can only call pop() after front() has returned a non-nullptr");
    slots_[readIdx + kPadding].~T();
    auto nextReadIdx = readIdx + 1;
    if (nextReadIdx == capacity_) {
      nextReadIdx = 0;
    }

    readIdx_.store(nextReadIdx, std::memory_order_release);
  }

  size_t size() const noexcept {
    std::ptrdiff_t diff = writeIdx_.load(std::memory_order_acquire) - readIdx_.load(std::memory_order_acquire);
    if (diff < 0) {
      diff += capacity_;
    }
    return static_cast<size_t>(diff);
  }

  bool empty() const noexcept {
    return writeIdx_.load(std::memory_order_acquire) == readIdx_.load(std::memory_order_acquire);
  }

  size_t capacity() const noexcept { return capacity_ - 1; }
};

}
