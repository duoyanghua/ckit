// SPDX-License-Identifier: Apache-2.0 -- adapted from folly::ProducerConsumerQueue.
#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <ckit/spsc/spsc_config.hpp>

namespace spsc::follyv {

template <class T>
class alignas(kCacheLineSize) ProducerConsumerQueue {
private:
  struct State {
    T* records{};
    std::uint32_t mask{};
    std::uint32_t size{};
  };

  struct Side {
    T* const records{};
    std::uint32_t const mask{};
    std::uint32_t const size{};
    std::atomic<std::uint64_t> localIndex{};
    std::uint64_t remoteIndexCache{};
  };
  alignas(kCacheLineSize) Side producer_;
  alignas(kCacheLineSize) Side consumer_;

  static constexpr std::uint32_t kMaxSize = std::uint32_t{1} << 31;
  static std::uint32_t validateSize(std::uint32_t size) {
    if (size < 1 || size > kMaxSize) {
      throw std::invalid_argument("ProducerConsumerQueue: size must be in [1, 1 << 31]");
    }
    return size;
  }
  static std::size_t allocationSize(std::uint32_t mask) {
    std::uint64_t const bytes = sizeof(T) * (std::uint64_t{mask} + 1);
    if (bytes > std::numeric_limits<std::size_t>::max()) {
      throw std::length_error("ProducerConsumerQueue: requested size overflows size_t");
    }
    return static_cast<std::size_t>(bytes);
  }
  static State makeState(std::uint32_t size) {
    std::uint32_t const validSize = validateSize(size);
    std::uint32_t const mask = bit_ceil(validSize) - 1;
    auto* const records = static_cast<T*>(std::malloc(allocationSize(mask)));

    if (!records) {
      throw std::bad_alloc();
    }
    return State{records, mask, validSize};
  }
  explicit ProducerConsumerQueue(State st) noexcept
      : producer_{st.records, st.mask, st.size}, consumer_{st.records, st.mask, st.size} {}

public:
  using value_type = T;
  ProducerConsumerQueue(const ProducerConsumerQueue&) = delete;
  ProducerConsumerQueue& operator=(const ProducerConsumerQueue&) = delete;

  explicit ProducerConsumerQueue(std::uint32_t size)
      : ProducerConsumerQueue(makeState(size)) {}
  ~ProducerConsumerQueue() {

    if (!std::is_trivially_destructible_v<T>) {
      auto readIndex = consumer_.localIndex.load(std::memory_order_relaxed);
      auto const endIndex = producer_.localIndex.load(std::memory_order_relaxed);
      while (readIndex != endIndex) {
        producer_.records[readIndex & producer_.mask].~T();
        ++readIndex;
      }
    }
    std::free(producer_.records);
  }

  template <class... Args>
  bool write(Args&&... recordArgs) {
    auto const currentWrite = producer_.localIndex.load(std::memory_order_relaxed);

    if (currentWrite - producer_.remoteIndexCache >= producer_.size) {

      producer_.remoteIndexCache = consumer_.localIndex.load(std::memory_order_acquire);
      if (currentWrite - producer_.remoteIndexCache >= producer_.size) {
        return false;
      }
    }
    new (&producer_.records[currentWrite & producer_.mask]) T(std::forward<Args>(recordArgs)...);

    producer_.localIndex.store(currentWrite + 1, std::memory_order_release);
    return true;
  }

  bool read(T& record) {
    auto const currentRead = consumer_.localIndex.load(std::memory_order_relaxed);
    if (currentRead >= consumer_.remoteIndexCache) {

      consumer_.remoteIndexCache = producer_.localIndex.load(std::memory_order_acquire);
      if (currentRead >= consumer_.remoteIndexCache) {
        return false;
      }
    }
    auto const idx = currentRead & consumer_.mask;
    record = std::move(consumer_.records[idx]);
    consumer_.records[idx].~T();
    consumer_.localIndex.store(currentRead + 1, std::memory_order_release);
    return true;
  }

  T* frontPtr() {
    auto const currentRead = consumer_.localIndex.load(std::memory_order_relaxed);
    if (currentRead >= consumer_.remoteIndexCache) {
      consumer_.remoteIndexCache = producer_.localIndex.load(std::memory_order_acquire);
      if (currentRead >= consumer_.remoteIndexCache) {
        return nullptr;
      }
    }
    return &consumer_.records[currentRead & consumer_.mask];
  }

  void popFront() {
    auto const currentRead = consumer_.localIndex.load(std::memory_order_relaxed);
    assert(currentRead != producer_.localIndex.load(std::memory_order_acquire));
    auto const idx = currentRead & consumer_.mask;
    consumer_.records[idx].~T();
    consumer_.localIndex.store(currentRead + 1, std::memory_order_release);
  }

  std::size_t capacity() const noexcept { return producer_.size; }
  std::size_t sizeGuess() const noexcept {
    auto const c = consumer_.localIndex.load(std::memory_order_relaxed);
    auto const p = producer_.localIndex.load(std::memory_order_relaxed);
    auto const m = capacity();
    return p < c ? 0 : m < p - c ? m : p - c;
  }
  bool isEmpty() const noexcept { return sizeGuess() == 0; }
  bool isFull() const noexcept { return sizeGuess() == producer_.size; }

};

}
