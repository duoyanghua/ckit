// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include <ckit/chase_lev/chase_config.hpp>

namespace chase::taskflow {

inline constexpr int kDefaultBoundedLogSize = 8;
inline constexpr int kDefaultUnboundedLogSize = 10;

template <typename T>
class UnboundedWSQ {

  struct Array {
    size_t C;
    size_t M;
    std::atomic<T>* S;
    explicit Array(size_t c) :
      C {c}, M {c-1}, S {new std::atomic<T>[C]} {}
    ~Array() {
      delete [] S;
    }
    size_t capacity() const noexcept {
      return C;
    }
    void push(int64_t i, T o) noexcept {
      S[i & M].store(o, std::memory_order_relaxed);
    }
    T pop(int64_t i) noexcept {
      return S[i & M].load(std::memory_order_relaxed);
    }

    Array* resize(int64_t b, int64_t t) {
      Array* ptr = new Array{2*C};
      for(int64_t i=t; i!=b; ++i) {
        ptr->push(i, pop(i));
      }
      return ptr;
    }

    Array* resize(int64_t b, int64_t t, size_t N) {
      Array* ptr = new Array{chase::next_pow_two(C + N)};
      for(int64_t i=t; i!=b; ++i) {
        ptr->push(i, pop(i));
      }
      return ptr;
    }
  };

  alignas(chase::kCacheLineSize) std::atomic<int64_t> _top;
  alignas(chase::kCacheLineSize) std::atomic<int64_t> _bottom;
  int64_t _cached_top {0};
  alignas(chase::kCacheLineSize) std::atomic<Array*> _array;
  std::vector<Array*> _garbage;

  Array* _resize_array(Array* a, int64_t b, int64_t t){
    Array* tmp = a->resize(b, t);
    _garbage.push_back(a);
    _array.store(tmp, std::memory_order_release);
    return tmp;
  }
  Array* _resize_array(Array* a, int64_t b, int64_t t, size_t N){
    Array* tmp = a->resize(b, t, N);
    _garbage.push_back(a);
    _array.store(tmp, std::memory_order_release);
    return tmp;
  }
public:

  using value_type = std::conditional_t<std::is_pointer_v<T>, T, std::optional<T>>;

  explicit UnboundedWSQ(int64_t LogSize = kDefaultUnboundedLogSize){
    _top.store(0, std::memory_order_relaxed);
    _bottom.store(0, std::memory_order_relaxed);
    _array.store(new Array{(size_t{1} << LogSize)}, std::memory_order_relaxed);
    _garbage.reserve(32);
  }
  ~UnboundedWSQ(){
    for(auto a : _garbage) delete a;
    delete _array.load();
  }
  UnboundedWSQ(const UnboundedWSQ&)            = delete;
  UnboundedWSQ& operator=(const UnboundedWSQ&) = delete;

  bool   empty()    const noexcept{
    int64_t t = _top.load(std::memory_order_relaxed);
    int64_t b = _bottom.load(std::memory_order_relaxed);
    return (b <= t);
  }
  size_t size()     const noexcept{
    int64_t t = _top.load(std::memory_order_relaxed);
    int64_t b = _bottom.load(std::memory_order_relaxed);
    return static_cast<size_t>(b >= t ? b - t : 0);
  }
  size_t capacity() const noexcept{
    return _array.load(std::memory_order_relaxed)->capacity();
  }

  void push(T o){
    int64_t b = _bottom.load(std::memory_order_relaxed);
    Array*  a = _array.load(std::memory_order_relaxed);

    if(a->capacity() < static_cast<size_t>(b - _cached_top + 1))  {
      _cached_top = _top.load(std::memory_order_acquire);
      if(a->capacity() < static_cast<size_t>(b - _cached_top + 1))  {
        a = _resize_array(a, b, _cached_top);
      }
    }
    a->push(b, o);
    _bottom.store(b + 1, std::memory_order_release);
  }
  template <typename I>
  void bulk_push(I first, size_t N){
    if(N == 0) return;
    int64_t b = _bottom.load(std::memory_order_relaxed);
    Array*  a = _array.load(std::memory_order_relaxed);

    if((b - _cached_top + N) > a->capacity())  {
      _cached_top = _top.load(std::memory_order_acquire);
      if((b - _cached_top + N) > a->capacity())  {
        a = _resize_array(a, b, _cached_top, N);
      }
    }
    for(size_t i=0; i<N; ++i) {
      a->push(b++, first[i]);
    }
    _bottom.store(b, std::memory_order_release);
  }

  value_type pop(){
    int64_t b = _bottom.load(std::memory_order_relaxed) - 1;
    Array* a = _array.load(std::memory_order_relaxed);
    _bottom.store(b, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    int64_t t = _top.load(std::memory_order_relaxed);

    auto item = empty_value();
    if(t <= b) {
      item = a->pop(b);
      if(t == b) {

        if(!_top.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed)) {
          item = empty_value();
        }
        _bottom.store(b + 1, std::memory_order_relaxed);
      }
    }
    else {
      _bottom.store(b + 1, std::memory_order_relaxed);
    }
    return item;
  }
  value_type steal(){
    int64_t t = _top.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    int64_t b = _bottom.load(std::memory_order_acquire);

    auto item = empty_value();
    if(t < b) {
      Array* a = _array.load(std::memory_order_acquire);
      item = a->pop(t);
      if(!_top.compare_exchange_strong(t, t+1, std::memory_order_seq_cst, std::memory_order_relaxed)) {
        return empty_value();
      }
    }
    return item;
  }

  value_type steal_with_feedback(size_t& num_empty_steals){
    int64_t t = _top.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    int64_t b = _bottom.load(std::memory_order_acquire);
    auto item = empty_value();
    if(t < b) {
      num_empty_steals = 0;
      Array* a = _array.load(std::memory_order_acquire);
      item = a->pop(t);
      if(!_top.compare_exchange_strong(t, t+1, std::memory_order_seq_cst, std::memory_order_relaxed)) {
        return empty_value();
      }
    }
    else {
      ++num_empty_steals;
    }
    return item;
  }

  static constexpr value_type empty_value() noexcept {
    if constexpr (std::is_pointer_v<T>) return T{nullptr};
    else                                return std::optional<T>{std::nullopt};
  }
};

template <typename T, size_t LogSize = kDefaultBoundedLogSize>
class BoundedWSQ {

  static constexpr size_t BufferSize = size_t{1} << LogSize;
  static constexpr size_t BufferMask = BufferSize - 1;
  static_assert(BufferSize >= 2 && (BufferSize & BufferMask) == 0);

  alignas(chase::kCacheLineSize) std::atomic<int64_t> _top    {0};
  alignas(chase::kCacheLineSize) std::atomic<int64_t> _bottom {0};
  int64_t _cached_top {0};
  alignas(chase::kCacheLineSize) std::atomic<T>       _buffer[BufferSize];

public:
  using value_type = std::conditional_t<std::is_pointer_v<T>, T, std::optional<T>>;

  BoundedWSQ()  = default;
  ~BoundedWSQ() = default;
  BoundedWSQ(const BoundedWSQ&)            = delete;
  BoundedWSQ& operator=(const BoundedWSQ&) = delete;

  bool   empty()    const noexcept{
    int64_t t = _top.load(std::memory_order_relaxed);
    int64_t b = _bottom.load(std::memory_order_relaxed);
    return b <= t;
  }
  size_t size()     const noexcept{
    int64_t t = _top.load(std::memory_order_relaxed);
    int64_t b = _bottom.load(std::memory_order_relaxed);
    return static_cast<size_t>(b >= t ? b - t : 0);
  }
  static constexpr size_t capacity() noexcept { return BufferSize; }

  template <typename O>
  bool try_push(O&& o){
    int64_t b = _bottom.load(std::memory_order_relaxed);
    if(static_cast<size_t>(b - _cached_top + 1) > BufferSize) {
      _cached_top = _top.load(std::memory_order_acquire);
      if(static_cast<size_t>(b - _cached_top + 1) > BufferSize) {
        return false;
      }
    }
    _buffer[b & BufferMask].store(std::forward<O>(o), std::memory_order_relaxed);
    _bottom.store(b + 1, std::memory_order_release);
    return true;
  }
  template <typename I>
  size_t try_bulk_push(I first, size_t N){
    if(N == 0) return 0;
    int64_t b = _bottom.load(std::memory_order_relaxed);
    size_t r = BufferSize - static_cast<size_t>(b - _cached_top);
    if(r == 0)  {
      _cached_top = _top.load(std::memory_order_acquire);
      r = BufferSize - static_cast<size_t>(b - _cached_top);
    }
    size_t n = std::min(N, r);
    if(n > 0) {
      for(size_t i=0; i<n; ++i) {
        _buffer[b++ & BufferMask].store(first[i], std::memory_order_relaxed);
      }
      _bottom.store(b, std::memory_order_release);
    }
    return n;
  }

  value_type pop(){
    int64_t b = _bottom.load(std::memory_order_relaxed) - 1;
    _bottom.store(b, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    int64_t t = _top.load(std::memory_order_relaxed);
    auto item = empty_value();
    if(t <= b) {
      item = _buffer[b & BufferMask].load(std::memory_order_relaxed);
      if(t == b) {
        if(!_top.compare_exchange_strong(t, t+1, std::memory_order_seq_cst, std::memory_order_relaxed)) {
          item = empty_value();
        }
        _bottom.store(b + 1, std::memory_order_relaxed);
      }
    }
    else {
      _bottom.store(b + 1, std::memory_order_relaxed);
    }
    return item;
  }
  value_type steal(){
    int64_t t = _top.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    int64_t b = _bottom.load(std::memory_order_acquire);
    auto item = empty_value();
    if(t < b) {
      item = _buffer[t & BufferMask].load(std::memory_order_relaxed);
      if(!_top.compare_exchange_strong(t, t+1, std::memory_order_seq_cst, std::memory_order_relaxed)) {
        return empty_value();
      }
    }
    return item;
  }
  value_type steal_with_feedback(size_t& num_empty_steals){
    int64_t t = _top.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    int64_t b = _bottom.load(std::memory_order_acquire);
    auto item = empty_value();
    if(t < b) {
      num_empty_steals = 0;
      item = _buffer[t & BufferMask].load(std::memory_order_relaxed);
      if(!_top.compare_exchange_strong(t, t+1, std::memory_order_seq_cst, std::memory_order_relaxed)) {
        return empty_value();
      }
    }
    else {
      ++num_empty_steals;
    }
    return item;
  }

  static constexpr value_type empty_value() noexcept {
    if constexpr (std::is_pointer_v<T>) return T{nullptr};
    else                                return std::optional<T>{std::nullopt};
  }
};

}
