// After Le, Pop, Cohen, Zappa Nardelli, PPoPP 2013 (Figure 1).
#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <type_traits>
#include <vector>

#include <ckit/chase_lev/chase_config.hpp>

namespace chase::paper {

template <class T>
class WorkStealingDeque {
  static_assert(std::is_trivially_copyable_v<T>,"paper's buffer uses std::atomic<T>; T must be trivially copyable");

  struct Array {
    std::int64_t size;
    std::atomic<T>* buffer;

    explicit
    Array(std::int64_t cap) : size(cap), buffer(new std::atomic<T>[static_cast<std::size_t>(cap)]{}) {
      assert(chase::is_power_of_two(static_cast<std::size_t>(cap)));
    }
    ~Array() { delete[] buffer; }
    T get(std::int64_t i) const noexcept {
      return buffer[i & (size - 1)].load(std::memory_order_relaxed);
    }
    void put(std::int64_t i, T x) noexcept {
      buffer[i & (size - 1)].store(x, std::memory_order_relaxed);
    }
  };

  alignas(chase::kCacheLineSize) std::atomic<std::int64_t> top_{0};
  alignas(chase::kCacheLineSize) std::atomic<std::int64_t> bottom_{0};
  std::atomic<Array*> array_;
  std::vector<Array*> garbage_;

public:
  explicit WorkStealingDeque(std::int64_t capacity = 64)
      : array_(new Array(static_cast<std::int64_t>(chase::next_pow_two(static_cast<std::size_t>(capacity))))) {
    assert(capacity >= 2);
    garbage_.reserve(32);
  }
  ~WorkStealingDeque() {
    delete array_.load(std::memory_order_relaxed);
    for (Array* a : garbage_) delete a;
  }
  WorkStealingDeque(const WorkStealingDeque&) = delete;
  WorkStealingDeque& operator=(const WorkStealingDeque&) = delete;

  bool empty() const noexcept {
    return bottom_.load(std::memory_order_relaxed) <= top_.load(std::memory_order_relaxed);
  }
  std::size_t size() const noexcept {
    const std::int64_t b = bottom_.load(std::memory_order_relaxed);
    const std::int64_t t = top_.load(std::memory_order_relaxed);
    return static_cast<std::size_t>(b >= t ? b - t : 0);
  }
  std::int64_t capacity() const noexcept {
    return array_.load(std::memory_order_relaxed)->size;
  }

private:

  void resize() {
    const std::int64_t b = bottom_.load(std::memory_order_relaxed);
    const std::int64_t t = top_.load(std::memory_order_acquire);

    Array* old = array_.load(std::memory_order_relaxed);
    Array* next = new Array(old->size * 2);
    for (std::int64_t i = t; i != b; ++i) {
      next->put(i, old->get(i));
    }
    garbage_.push_back(old);
    array_.store(next, std::memory_order_release);
  }

public:

  void push(T x) {
    std::int64_t b = bottom_.load(std::memory_order_relaxed);
    std::int64_t t = top_.load(std::memory_order_acquire);

    Array* a = array_.load(std::memory_order_relaxed);

    if (b - t > a->size - 1) {
      resize();
      a = array_.load(std::memory_order_relaxed);
    }
    a->put(b, x);
    bottom_.store(b + 1, std::memory_order_release);
  }

  std::optional<T> take() {

    std::int64_t b = bottom_.load(std::memory_order_relaxed) - 1;
    Array* a = array_.load(std::memory_order_relaxed);

    bottom_.store(b, std::memory_order_seq_cst);
    std::int64_t t = top_.load(std::memory_order_seq_cst);

    if (t <= b) {
      T x = a->get(b);
      if (t == b) {

        if (!top_.compare_exchange_strong(t, t + 1,std::memory_order_seq_cst,std::memory_order_relaxed)) {
          bottom_.store(b + 1, std::memory_order_relaxed);
          return std::nullopt;

        }
        bottom_.store(b + 1, std::memory_order_relaxed);

      }
      return x;

    }
    bottom_.store(b + 1, std::memory_order_relaxed);

    return std::nullopt;

  }

  std::optional<T> steal() {
    std::int64_t t = top_.load(std::memory_order_seq_cst);
    std::int64_t b = bottom_.load(std::memory_order_seq_cst);

    if (t < b) {
      Array* a = array_.load(std::memory_order_acquire);

      T x = a->get(t);
      if (!top_.compare_exchange_strong(t, t + 1,std::memory_order_seq_cst,std::memory_order_relaxed)) {
        return std::nullopt;
      }
      return x;
    }
    return std::nullopt;
  }
};

}
