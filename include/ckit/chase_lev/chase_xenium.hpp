// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>

#include <ckit/chase_lev/chase_config.hpp>

namespace chase::xenium {

namespace detail {

inline constexpr std::size_t find_last_bit_set(std::size_t v) noexcept {
  std::size_t r = 0;
  while (v != 0) { ++r; v >>= 1; }
  return r;
}

template <class T, std::size_t MinCapacity = 128, std::size_t MaxCapacity = (std::size_t{1} << 31)>
struct growing_circular_array {
  static constexpr std::size_t min_capacity = MinCapacity;
  static constexpr std::size_t max_capacity = MaxCapacity;
  static constexpr std::size_t initial_buckets = find_last_bit_set(MinCapacity);
  static constexpr std::size_t num_buckets = find_last_bit_set(max_capacity);

  std::size_t _buckets;
  std::atomic<std::size_t> _capacity;
  using entry = std::atomic<T*>;
  entry* _data[num_buckets];

  growing_circular_array() : _buckets(initial_buckets), _capacity(MinCapacity), _data() {
    auto* ptr = new entry[MinCapacity]{};
    _data[0] = ptr++;
    for (std::size_t i = 1; i < _buckets; ++i) {
      _data[i] = ptr;
      ptr += static_cast<std::size_t>(1) << (i - 1);
    }
  }
  ~growing_circular_array(){
    delete[] (_data[0]);

    for (std::size_t i = initial_buckets; i < num_buckets; ++i) {
      delete[] (_data[i]);
    }
  }
private:

  entry& get_entry(std::size_t idx, std::size_t capacity) {
    idx &= capacity - 1;
    std::size_t bucket = find_last_bit_set(idx);
    assert(bucket < num_buckets);
    idx ^= (std::size_t{1} << bucket) >> 1;
    return _data[bucket][idx];

  }
public:

  T* get(std::size_t idx, std::memory_order order) {

    auto cap = _capacity.load(std::memory_order_acquire);
    return get_entry(idx, cap).load(order);
  }

  void put(std::size_t idx, T* value, std::memory_order order) {
    auto cap = _capacity.load(std::memory_order_relaxed);
    get_entry(idx, cap).store(value, order);
  }

  [[nodiscard]] std::size_t capacity() const {
    return _capacity.load(std::memory_order_relaxed);
  }
  bool can_grow() const { return capacity() < max_capacity; }

  void grow(std::size_t bottom, std::size_t top){
    assert(can_grow());
    auto capacity = this->capacity();
    auto mod_mask = capacity - 1;
    assert((capacity & mod_mask) == 0);
    _data[_buckets] = new entry[capacity]{};
    ++_buckets;
    auto new_capacity = capacity * 2;
    auto new_mod_mask = new_capacity - 1;
    auto start = top;
    auto start_mod = top & mod_mask;

    if (start_mod == (top & new_mod_mask)) {
      start += capacity - start_mod;
    }
    for (std::size_t i = start; i < bottom; ++i) {
      auto oldI = i & mod_mask;
      auto newI = i & new_mod_mask;
      if (oldI != newI) {
        auto oldBit = find_last_bit_set(oldI);
        auto newBit = find_last_bit_set(newI);
        auto* v = _data[oldBit][oldI ^ ((std::size_t{1} << oldBit) >> 1)].load(std::memory_order_relaxed);
        _data[newBit][newI ^ ((std::size_t{1} << newBit) >> 1)].store(v, std::memory_order_relaxed);

      } else {
        break;
      }
    }

    _capacity.store(new_capacity, std::memory_order_release);
  }

  static_assert(chase::is_power_of_two(min_capacity), "MinCapacity must be a power of two");
  static_assert(chase::is_power_of_two(max_capacity), "MaxCapacity must be a power of two");
  static_assert(min_capacity < max_capacity, "MaxCapacity must be greater than MinCapacity");
};

}

template <class T, std::size_t Capacity = 128>
struct chase_work_stealing_deque {
  using value_type = T*;
  using container = detail::growing_circular_array<T, Capacity>;

private:
  container _items;
  std::atomic<std::size_t> _bottom{0};
  std::atomic<std::size_t> _top{0};

public:
  chase_work_stealing_deque() = default;

  chase_work_stealing_deque(const chase_work_stealing_deque&) = delete;
  chase_work_stealing_deque(chase_work_stealing_deque&&) = delete;
  chase_work_stealing_deque& operator=(const chase_work_stealing_deque&) = delete;
  chase_work_stealing_deque& operator=(chase_work_stealing_deque&&) = delete;

  bool try_push(value_type item) {
    auto b = _bottom.load(std::memory_order_relaxed);
    auto t = _top.load(std::memory_order_acquire);
    auto size = b - t;
    if (size >= _items.capacity()) {
      if (_items.can_grow()) {
        _items.grow(b, t);
        assert(size < _items.capacity());

      } else {
        return false;
      }
    }
    _items.put(b, item, std::memory_order_relaxed);

    _bottom.store(b + 1, std::memory_order_release);
    return true;
  }
  bool try_pop(value_type& result) {
    auto b = _bottom.load(std::memory_order_relaxed);
    auto t = _top.load(std::memory_order_relaxed);
    if (b == t) {
      return false;
    }
    --b;
    _bottom.store(b, std::memory_order_seq_cst);
    t = _top.load(std::memory_order_seq_cst);

    auto* item = _items.get(b, std::memory_order_relaxed);
    if (b > t) {
      result = item;
      return true;
    }
    if (b == t) {
      if (_top.compare_exchange_strong(t, t + 1,std::memory_order_seq_cst, std::memory_order_relaxed)) {
        _bottom.store(t + 1, std::memory_order_relaxed);
        result = item;
        return true;
      }
      _bottom.store(t, std::memory_order_relaxed);
      return false;
    }
    assert(b == t - 1);
    _bottom.store(t, std::memory_order_relaxed);
    return false;
  }

  bool try_steal(value_type& result) {
    auto t = _top.load(std::memory_order_seq_cst);
    auto b = _bottom.load(std::memory_order_seq_cst);

    auto size = static_cast<std::intptr_t>(b) - static_cast<std::intptr_t>(t);
    if (size <= 0) {
      return false;
    }
    auto* item = _items.get(t, std::memory_order_relaxed);
    if (_top.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed)) {
      result = item;
      return true;
    }
    return false;
  }
  std::size_t size() const {
    auto t = _top.load(std::memory_order_relaxed);
    return _bottom.load(std::memory_order_relaxed) - t;
  }
};

}
