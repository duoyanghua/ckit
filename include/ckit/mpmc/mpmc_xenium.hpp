// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

#include <ckit/mpmc/mpmc_config.hpp>

namespace mpmc::xenium {

template <class T>
class vyukov_bounded_queue {
  using storage_t = std::byte[sizeof(T)];

  struct alignas(kCacheLineSize) cell {
    std::atomic<std::size_t> sequence;
    alignas(T) storage_t data;
  };
  std::unique_ptr<cell[]> cells;
  const std::size_t index_mask;
  alignas(kCacheLineSize) std::atomic<std::size_t> enqueue_pos;
  alignas(kCacheLineSize) std::atomic<std::size_t> dequeue_pos;

  void assign_value(storage_t &v, const T &source) { new (&v) T(source); }
  void assign_value(storage_t &v, T &&source) { new (&v) T(std::move(source)); }
  template <class... Args>
  void assign_value(storage_t &v, Args &&...args) { new (&v) T{std::forward<Args>(args)...}; }

public:
  static_assert(std::is_nothrow_move_constructible_v<T>,"T must be nothrow move constructible.");

  explicit vyukov_bounded_queue(std::size_t size) : cells(new cell[size]), index_mask(size - 1) {
    assert(size >= 2 && is_power_of_two(size));
    for (std::size_t i = 0; i < size; ++i) {
      cells[i].sequence.store(i, std::memory_order_relaxed);
    }
    enqueue_pos.store(0, std::memory_order_relaxed);
    dequeue_pos.store(0, std::memory_order_relaxed);
  }
  ~vyukov_bounded_queue(){
    std::size_t deq_pos = dequeue_pos.load(std::memory_order_relaxed);
    std::size_t enq_pos = enqueue_pos.load(std::memory_order_relaxed);

    for (; deq_pos != enq_pos; ++deq_pos) {
      auto c = &cells[deq_pos & index_mask];
      [[maybe_unused]] std::size_t seq = c->sequence.load(std::memory_order_acquire);
      assert(seq == deq_pos + 1);
      reinterpret_cast<T &>(c->data).~T();
    }
  }

  vyukov_bounded_queue(const vyukov_bounded_queue &) = delete;
  vyukov_bounded_queue(vyukov_bounded_queue &&) = delete;
  vyukov_bounded_queue &operator=(const vyukov_bounded_queue &) = delete;
  vyukov_bounded_queue &operator=(vyukov_bounded_queue &&) = delete;

  template <class... Args>
  bool try_push(Args &&...args) {
    return do_try_push<false>(std::forward<Args>(args)...);
  }

  template <class... Args>
  bool try_push_strong(Args &&...args) {
    return do_try_push<false>(std::forward<Args>(args)...);
  }

  template <class... Args>
  bool try_push_weak(Args &&...args) {
    return do_try_push<true>(std::forward<Args>(args)...);
  }

  [[nodiscard]] bool try_pop(T &result) {
    return do_try_pop<false>(
        [&result](T &v) {
          result = std::move(v);
          return true;
        },
        []() { return false; });
  }

  [[nodiscard]] bool try_pop_strong(T &result) {
    return do_try_pop<false>(
        [&result](T &v) {
          result = std::move(v);
          return true;
        },
        []() { return false; });
  }

  [[nodiscard]] bool try_pop_weak(T &result) {
    return do_try_pop<true>(
        [&result](T &v) {
          result = std::move(v);
          return true;
        },
        []() { return false; });
  }

private:

  template <bool Weak, class... Args>
  bool do_try_push(Args &&...args) {
    cell *c;
    std::size_t pos = enqueue_pos.load(std::memory_order_relaxed);
    for (;;) {
      c = &cells[pos & index_mask];
      std::size_t seq = c->sequence.load(std::memory_order_acquire);
      if (seq == pos) {

        if (enqueue_pos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
          break;
        }
      } else {
        if constexpr (Weak) {
          if (seq < pos) {
            return false;
          }

          pos = enqueue_pos.load(std::memory_order_relaxed);
        } else {

          auto pos2 = enqueue_pos.load(std::memory_order_relaxed);

          if (pos2 == pos && dequeue_pos.load(std::memory_order_relaxed) + index_mask + 1 == pos) {
            return false;
          }

          pos = pos2;
        }
      }
    }
    assign_value(c->data, std::forward<Args>(args)...);
    c->sequence.store(pos + 1, std::memory_order_release);
    return true;
  }

  template <bool Weak, class SuccessFunc, class EmptyFunc>
  auto do_try_pop(SuccessFunc successFunc, EmptyFunc emptyFunc) {
    cell *c;
    std::size_t pos = dequeue_pos.load(std::memory_order_relaxed);
    for (;;) {
      c = &cells[pos & index_mask];
      std::size_t seq = c->sequence.load(std::memory_order_acquire);
      auto new_pos = pos + 1;
      if (seq == new_pos) {
        if (dequeue_pos.compare_exchange_weak(pos, new_pos, std::memory_order_relaxed)) {
          break;
        }
      } else {
        if constexpr (Weak) {
          if (seq < new_pos) {
            return emptyFunc();
          }
          pos = dequeue_pos.load(std::memory_order_relaxed);
        } else {

          auto pos2 = dequeue_pos.load(std::memory_order_relaxed);

          if (pos2 == pos && enqueue_pos.load(std::memory_order_relaxed) == pos) {
            return emptyFunc();
          }
          pos = pos2;
        }
      }
    }
    auto &v = reinterpret_cast<T &>(c->data);
    auto result = successFunc(v);
    v.~T();

    c->sequence.store(pos + index_mask + 1, std::memory_order_release);
    return result;
  }

};

}
