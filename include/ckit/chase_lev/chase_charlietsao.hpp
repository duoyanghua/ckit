// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace chase::charlietsao {

template <typename T>
class WorkStealingQueue {
  static_assert(std::is_trivially_copyable_v<T>, "buffer uses std::atomic<T>; T must be trivially copyable");

  struct Array {
    int64_t C;
    int64_t M;
    std::atomic<T>* S;
    explicit Array(int64_t c) : C {c}, M {c-1}, S {new std::atomic<T>[static_cast<size_t>(C)]} { }
    ~Array() { delete [] S; }
    int64_t capacity() const noexcept { return C; }

    template <typename O>
    void push(int64_t i, O&& o) noexcept {
      S[i & M].store(std::forward<O>(o), std::memory_order_relaxed);
    }
    T pop(int64_t i) noexcept {
      return S[i & M].load(std::memory_order_relaxed);
    }
    Array* resize(int64_t b, int64_t t) {
      Array* ptr = new Array {2*C};
      for(int64_t i=t; i!=b; ++i) {
        ptr->push(i, pop(i));
      }
      return ptr;
    }
  };
  std::atomic<int64_t> _top;
  std::atomic<int64_t> _bottom;
  std::atomic<Array*> _array;
  std::vector<Array*> _garbage;
public:

    explicit WorkStealingQueue(int64_t c = 1024){
      assert(c && (!(c & (c-1))));
      _top.store(0, std::memory_order_relaxed);
      _bottom.store(0, std::memory_order_relaxed);
      _array.store(new Array{c}, std::memory_order_relaxed);
      _garbage.reserve(32);
    }

    ~WorkStealingQueue(){
      for(auto a : _garbage) {
        delete a;
      }
      delete _array.load();
    }

    bool empty() const noexcept{
      int64_t b = _bottom.load(std::memory_order_relaxed);
      int64_t t = _top.load(std::memory_order_relaxed);
      return b <= t;
    }
    size_t size() const noexcept{
      int64_t b = _bottom.load(std::memory_order_relaxed);
      int64_t t = _top.load(std::memory_order_relaxed);
      return static_cast<size_t>(b >= t ? b - t : 0);
    }

    int64_t capacity() const noexcept{
      return _array.load(std::memory_order_relaxed)->capacity();
    }

    template <typename O>
    void push(O&& o){
      int64_t b = _bottom.load(std::memory_order_relaxed);
      int64_t t = _top.load(std::memory_order_acquire);
      Array* a = _array.load(std::memory_order_relaxed);

      if(a->capacity() - 1 < (b - t)) {
        Array* tmp = a->resize(b, t);
        _garbage.push_back(a);
        std::swap(a, tmp);
        _array.store(a, std::memory_order_release);
      }
      a->push(b, std::forward<O>(o));
      std::atomic_thread_fence(std::memory_order_release);
      _bottom.store(b + 1, std::memory_order_relaxed);
    }
    std::optional<T> pop(){
      int64_t b = _bottom.load(std::memory_order_relaxed) - 1;
      Array* a = _array.load(std::memory_order_relaxed);
      _bottom.store(b, std::memory_order_release);
      std::atomic_thread_fence(std::memory_order_seq_cst);
      int64_t t = _top.load(std::memory_order_relaxed);
      std::optional<T> item;
      if(t <= b) {
        item = a->pop(b);
        if(t == b) {
          if(!_top.compare_exchange_strong(t, t+1,  std::memory_order_seq_cst,  std::memory_order_relaxed)) {
            item = std::nullopt;
          }
          _bottom.store(b + 1, std::memory_order_relaxed);
        }
      }
      else {
        _bottom.store(b + 1, std::memory_order_relaxed);
      }
      return item;
    }
    std::optional<T> steal(){
      int64_t t = _top.load(std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_seq_cst);
      int64_t b = _bottom.load(std::memory_order_acquire);
      std::optional<T> item;
      if(t < b) {
        Array* a = _array.load(std::memory_order_acquire);
        item = a->pop(t);
        if(!_top.compare_exchange_strong(t, t+1, std::memory_order_seq_cst, std::memory_order_relaxed)) {
          return std::nullopt;
        }
      }
      return item;
    }
};

}
