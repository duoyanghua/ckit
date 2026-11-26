// SPDX-License-Identifier: BSD-2-Clause -- after D. Vyukov's bounded MPMC queue.
#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <ckit/mpmc/mpmc_config.hpp>

namespace mpmc::vyukov {

template <typename T>
class mpmc_bounded_queue {

  struct alignas(kCacheLineSize) cell_t {
    std::atomic<size_t> sequence_;
    T data_;
  };
  alignas(kCacheLineSize) cell_t *const buffer_;
  const size_t buffer_mask_;
  alignas(kCacheLineSize) std::atomic<size_t> enqueue_pos_;
  alignas(kCacheLineSize) std::atomic<size_t> dequeue_pos_;

public:

  explicit
  mpmc_bounded_queue(size_t buffer_size) : buffer_(new cell_t[buffer_size]), buffer_mask_(buffer_size - 1) {

    assert((buffer_size >= 2) && ((buffer_size & (buffer_size - 1)) == 0));
    for (size_t i = 0; i != buffer_size; i += 1) {
      buffer_[i].sequence_.store(i, std::memory_order_relaxed);
    }
    enqueue_pos_.store(0, std::memory_order_relaxed);
    dequeue_pos_.store(0, std::memory_order_relaxed);
  }

  ~mpmc_bounded_queue() { delete[] buffer_; }

  mpmc_bounded_queue(const mpmc_bounded_queue &) = delete;
  mpmc_bounded_queue &operator=(const mpmc_bounded_queue &) = delete;

  bool enqueue(const T &data) {
    cell_t *cell;
    size_t pos = enqueue_pos_.load(std::memory_order_relaxed);

    for (;;) {
      cell = &buffer_[pos & buffer_mask_];
      size_t seq = cell->sequence_.load(std::memory_order_acquire);

      std::intptr_t dif = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);
      if (dif == 0) {

        if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
          break;
        }
      } else if (dif < 0) {

        return false;
      } else {

        pos = enqueue_pos_.load(std::memory_order_relaxed);
      }
    }
    cell->data_ = data;
    cell->sequence_.store(pos + 1, std::memory_order_release);

    return true;
  }

  bool dequeue(T &data) {
    cell_t *cell;
    size_t pos = dequeue_pos_.load(std::memory_order_relaxed);

    for (;;) {
      cell = &buffer_[pos & buffer_mask_];
      size_t seq = cell->sequence_.load(std::memory_order_acquire);

      std::intptr_t dif = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos + 1);
      if (dif == 0) {

        if (dequeue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
          break;
        }
      } else if (dif < 0) {

        return false;
      } else {

        pos = dequeue_pos_.load(std::memory_order_relaxed);
      }
    }

    data = cell->data_;
    cell->sequence_.store(pos + buffer_mask_ + 1, std::memory_order_release);

    return true;
  }

};

}
