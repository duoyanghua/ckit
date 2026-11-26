// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <ckit/mpmc/mpmc_config.hpp>

namespace mpmc::rigtorp {

template <typename T>
struct Slot {

  alignas(kCacheLineSize) std::atomic<size_t> turn = {0};
  alignas(T) std::byte storage[sizeof(T)];

  ~Slot() noexcept {
    if (turn & 1) {
      destroy();
    }
  }
  template <typename... Args>
  void construct(Args &&...args) noexcept {
    static_assert(std::is_nothrow_constructible_v<T, Args &&...>,"T must be nothrow constructible with Args&&...");
    new (&storage) T(std::forward<Args>(args)...);
  }

  void destroy() noexcept {
    static_assert(std::is_nothrow_destructible_v<T>,"T must be nothrow destructible");
    reinterpret_cast<T *>(&storage)->~T();
  }

  T &&move() noexcept {
    return std::move(*reinterpret_cast<T *>(&storage));
  }
};

template <typename T, typename Allocator = std::allocator<Slot<T>>>
class Queue {

  const size_t capacity_;
  Slot<T> *slots_;
  Allocator allocator_ [[no_unique_address]];

  alignas(kCacheLineSize) std::atomic<size_t> head_;
  alignas(kCacheLineSize) std::atomic<size_t> tail_;

  static_assert(std::is_nothrow_copy_assignable_v<T> || std::is_nothrow_move_assignable_v<T>,
                "T must be nothrow copy or move assignable");
  static_assert(std::is_nothrow_destructible_v<T>, "T must be nothrow destructible");

private:

  constexpr size_t idx(size_t i) const noexcept { return i % capacity_; }

  constexpr size_t turn(size_t i) const noexcept { return i / capacity_; }

public:
  explicit Queue(const size_t capacity, const Allocator &allocator = Allocator())
      : capacity_(capacity), allocator_(allocator), head_(0), tail_(0) {
    if (capacity_ < 1) {
      throw std::invalid_argument("capacity < 1");
    }

    slots_ = allocator_.allocate(capacity_ + 1);

    if (reinterpret_cast<size_t>(slots_) % alignof(Slot<T>) != 0) {
      allocator_.deallocate(slots_, capacity_ + 1);
      throw std::bad_alloc();
    }
    for (size_t i = 0; i < capacity_; ++i) {
      new (&slots_[i]) Slot<T>();
    }
  }
  ~Queue() noexcept {
    for (size_t i = 0; i < capacity_; ++i) {
      slots_[i].~Slot();
    }
    allocator_.deallocate(slots_, capacity_ + 1);
  }

  Queue(const Queue &) = delete;
  Queue &operator=(const Queue &) = delete;

  template <typename... Args>
  void emplace(Args &&...args) noexcept {
    static_assert(std::is_nothrow_constructible_v<T, Args &&...>, "T must be nothrow constructible with Args&&...");
    auto const head = head_.fetch_add(1);
    auto &slot = slots_[idx(head)];
    while (turn(head) * 2 != slot.turn.load(std::memory_order_acquire))
      ;
    slot.construct(std::forward<Args>(args)...);
    slot.turn.store(turn(head) * 2 + 1, std::memory_order_release);
  }

  template <typename... Args> bool try_emplace(Args &&...args) noexcept {
    static_assert(std::is_nothrow_constructible_v<T, Args &&...>, "T must be nothrow constructible with Args&&...");
    auto head = head_.load(std::memory_order_acquire);
    for (;;) {
      auto &slot = slots_[idx(head)];
      if (turn(head) * 2 == slot.turn.load(std::memory_order_acquire)) {
        if (head_.compare_exchange_strong(head, head + 1)) {
          slot.construct(std::forward<Args>(args)...);
          slot.turn.store(turn(head) * 2 + 1, std::memory_order_release);
          return true;
        }
      } else {
        auto const prevHead = head;
        head = head_.load(std::memory_order_acquire);
        if (head == prevHead) {
          return false;
        }
      }
    }
  }

  void push(const T &v) noexcept {
    static_assert(std::is_nothrow_copy_constructible_v<T>,"T must be nothrow copy constructible");
    emplace(v);
  }
  template <typename P, typename = std::enable_if_t<std::is_nothrow_constructible_v<T, P &&>>>
  void push(P &&v) noexcept {
    emplace(std::forward<P>(v));
  }
  bool try_push(const T &v) noexcept {
    static_assert(std::is_nothrow_copy_constructible_v<T>,"T must be nothrow copy constructible");
    return try_emplace(v);
  }
  template <typename P, typename = std::enable_if_t<std::is_nothrow_constructible_v<T, P &&>>>
  bool try_push(P &&v) noexcept {
    return try_emplace(std::forward<P>(v));
  }

  void pop(T &v) noexcept {
    auto const tail = tail_.fetch_add(1);
    auto &slot = slots_[idx(tail)];
    while (turn(tail)*2 + 1 != slot.turn.load(std::memory_order_acquire))
      ;
    v = slot.move();
    slot.destroy();
    slot.turn.store(turn(tail) * 2 + 2, std::memory_order_release);
  }

  bool try_pop(T &v) noexcept {
    auto tail = tail_.load(std::memory_order_acquire);
    for (;;) {
      auto &slot = slots_[idx(tail)];
      if (turn(tail) * 2 + 1 == slot.turn.load(std::memory_order_acquire)) {
        if (tail_.compare_exchange_strong(tail, tail + 1)) {
          v = slot.move();
          slot.destroy();
          slot.turn.store(turn(tail) * 2 + 2, std::memory_order_release);
          return true;
        }
      } else {
        auto const prevTail = tail;
        tail = tail_.load(std::memory_order_acquire);
        if (tail == prevTail) {
          return false;
        }
      }
    }
  }

  ptrdiff_t size() const noexcept {

    return static_cast<ptrdiff_t>(head_.load(std::memory_order_relaxed) -
                                  tail_.load(std::memory_order_relaxed));
  }

  bool empty() const noexcept { return size() <= 0; }
};

}

namespace mpmc {

template <typename T, typename Allocator = std::allocator<rigtorp::Slot<T>>>
using MPMCQueue = rigtorp::Queue<T, Allocator>;

}
