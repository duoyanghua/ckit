// SPDX-License-Identifier: BSL-1.0 -- adapted from boost::lockfree::spsc_queue.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <iterator>
#include <memory>
#include <type_traits>
#include <utility>

#include <ckit/spsc/spsc_config.hpp>

namespace spsc::boostv {

template <typename T, typename Alloc = std::allocator<T>>
class spsc_queue : private Alloc {

  using alloc_traits = std::allocator_traits<Alloc>;
  inline static constexpr int kPaddingSize = static_cast<int>(kCacheLineSize) - static_cast<int>(sizeof(size_t));
  static_assert(kPaddingSize >= 0, "cache line must be at least sizeof(size_t)");

  alignas(kCacheLineSize) std::atomic<size_t> write_index_;
  char padding_[kPaddingSize];
  alignas(kCacheLineSize) std::atomic<size_t> read_index_;
  const size_t max_elements_;
  T* array_;

  static size_t next_index(size_t arg, size_t max_size) {
    size_t ret = arg + 1;
    while (ret >= max_size) {
      ret -= max_size;
    }
    return ret;
  }
  static bool empty_impl(size_t write_index, size_t read_index) {
    return write_index == read_index;
  }
  static size_t read_available_impl(size_t write_index, size_t read_index, size_t max_size) {
    if (write_index >= read_index) {
      return write_index - read_index;
    }
    const size_t ret = write_index + max_size - read_index;
    return ret;
  }
  static size_t write_available_impl(size_t write_index, size_t read_index, size_t max_size) {
    size_t ret = read_index - write_index - 1;
    if (write_index >= read_index) {
      ret += max_size;
    }
    return ret;
  }

  template <class OutputIterator>
  static OutputIterator copy_and_delete(T* first, T* last, OutputIterator out) {
    if (std::is_trivially_destructible_v<T>) {
      return std::copy(first, last, out);
    }
    for (; first != last; ++first, ++out) {
      *out = *first;
      first->~T();
    }
    return out;
  }
  template <class Functor>
  static void run_functor_and_delete(T* first, T* last, Functor& functor) {
    for (; first != last; ++first) {
      functor(*first);
      first->~T();
    }
  }
  static void noop(const T&) {}

public:
  explicit spsc_queue(size_t element_count, const Alloc& alloc = Alloc())
      : Alloc(alloc),
        write_index_(0),
        read_index_(0),
        max_elements_(element_count + 1),
        array_(alloc_traits::allocate(static_cast<Alloc&>(*this), max_elements_)) {}

  ~spsc_queue() {
    consume_all([](const T&) {});
    alloc_traits::deallocate(static_cast<Alloc&>(*this), array_, max_elements_);
  }

  spsc_queue(const spsc_queue&) = delete;
  spsc_queue& operator=(const spsc_queue&) = delete;

  bool push(const T& t) {
    const size_t write_index = write_index_.load(std::memory_order_relaxed);
    const size_t next = next_index(write_index, max_elements_);

    if (next == read_index_.load(std::memory_order_acquire)) {
      return false;
    }
    new (array_ + write_index) T(t);
    write_index_.store(next, std::memory_order_release);
    return true;
  }

  size_t push(const T* input_buffer, size_t input_count) {
    return static_cast<size_t>(push(input_buffer, input_buffer + input_count) - input_buffer);
  }

  template <typename ConstIterator>
  ConstIterator push(ConstIterator begin, ConstIterator end) {
    const size_t write_index = write_index_.load(std::memory_order_relaxed);
    const size_t read_index = read_index_.load(std::memory_order_acquire);
    const size_t avail = write_available_impl(write_index, read_index, max_elements_);
    if (avail == 0) {
      return begin;
    }
    size_t input_count = static_cast<size_t>(std::distance(begin, end));
    input_count = (std::min)(input_count, avail);
    size_t new_write_index = write_index + input_count;
    const ConstIterator last = std::next(begin, static_cast<std::ptrdiff_t>(input_count));

    if (write_index + input_count > max_elements_) {

      const size_t count0 = max_elements_ - write_index;
      const ConstIterator midpoint = std::next(begin, static_cast<std::ptrdiff_t>(count0));
      std::uninitialized_copy(begin, midpoint, array_ + write_index);
      std::uninitialized_copy(midpoint, last, array_);
      new_write_index -= max_elements_;
    } else {
      std::uninitialized_copy(begin, last, array_ + write_index);
      if (new_write_index == max_elements_) {
        new_write_index = 0;
      }
    }

    write_index_.store(new_write_index, std::memory_order_release);
    return last;
  }

  template <typename Functor>
  bool consume_one(Functor&& functor) {
    const size_t write_index = write_index_.load(std::memory_order_acquire);
    const size_t read_index = read_index_.load(std::memory_order_relaxed);
    if (empty_impl(write_index, read_index)) {
      return false;
    }
    T& object_to_consume = array_[read_index];
    functor(object_to_consume);
    object_to_consume.~T();
    const size_t next = next_index(read_index, max_elements_);
    read_index_.store(next, std::memory_order_release);
    return true;
  }
  bool pop() {
    return consume_one([](const T&) {});
  }
  template <typename U>
  bool pop(U& ret) {
    return consume_one([&ret](T& t) { ret = t; });
  }

  template <typename Functor>
  size_t consume_all(Functor&& functor) {
    const size_t write_index = write_index_.load(std::memory_order_acquire);
    const size_t read_index = read_index_.load(std::memory_order_relaxed);
    const size_t avail = read_available_impl(write_index, read_index, max_elements_);
    if (avail == 0) {
      return 0;
    }
    const size_t output_count = avail;
    size_t new_read_index = read_index + output_count;
    if (read_index + output_count > max_elements_) {
      const size_t count0 = max_elements_ - read_index;
      const size_t count1 = output_count - count0;
      run_functor_and_delete(array_ + read_index, array_ + max_elements_, functor);
      run_functor_and_delete(array_, array_ + count1, functor);
      new_read_index -= max_elements_;
    } else {
      run_functor_and_delete(array_ + read_index, array_ + read_index + output_count, functor);
      if (new_read_index == max_elements_) {
        new_read_index = 0;
      }
    }
    read_index_.store(new_read_index, std::memory_order_release);
    return output_count;
  }

  size_t pop(T* output_buffer, size_t output_count) {
    const size_t write_index = write_index_.load(std::memory_order_acquire);
    const size_t read_index = read_index_.load(std::memory_order_relaxed);
    const size_t avail = read_available_impl(write_index, read_index, max_elements_);
    if (avail == 0) {
      return 0;
    }
    output_count = (std::min)(output_count, avail);
    size_t new_read_index = read_index + output_count;
    if (read_index + output_count > max_elements_) {
      const size_t count0 = max_elements_ - read_index;
      const size_t count1 = output_count - count0;
      copy_and_delete(array_ + read_index, array_ + max_elements_, output_buffer);
      copy_and_delete(array_, array_ + count1, output_buffer + count0);
      new_read_index -= max_elements_;
    } else {
      copy_and_delete(array_ + read_index, array_ + read_index + output_count, output_buffer);
      if (new_read_index == max_elements_) {
        new_read_index = 0;
      }
    }
    read_index_.store(new_read_index, std::memory_order_release);
    return output_count;
  }

  template <typename OutputIterator>
  size_t pop_to_output_iterator(OutputIterator it) {
    const size_t write_index = write_index_.load(std::memory_order_acquire);
    const size_t read_index = read_index_.load(std::memory_order_relaxed);
    const size_t avail = read_available_impl(write_index, read_index, max_elements_);
    if (avail == 0) {
      return 0;
    }
    size_t new_read_index = read_index + avail;
    if (read_index + avail > max_elements_) {
      const size_t count0 = max_elements_ - read_index;
      const size_t count1 = avail - count0;
      it = copy_and_delete(array_ + read_index, array_ + max_elements_, it);
      copy_and_delete(array_, array_ + count1, it);
      new_read_index -= max_elements_;
    } else {
      copy_and_delete(array_ + read_index, array_ + read_index + avail, it);
      if (new_read_index == max_elements_) {
        new_read_index = 0;
      }
    }
    read_index_.store(new_read_index, std::memory_order_release);
    return avail;
  }

  const T& front() const {
    const size_t read_index = read_index_.load(std::memory_order_relaxed);
    return *(array_ + read_index);
  }
  T& front() {
    const size_t read_index = read_index_.load(std::memory_order_relaxed);
    return *(array_ + read_index);
  }
  size_t read_available() const {
    const size_t write_index = write_index_.load(std::memory_order_acquire);
    const size_t read_index = read_index_.load(std::memory_order_relaxed);
    return read_available_impl(write_index, read_index, max_elements_);
  }
  size_t write_available() const {
    const size_t write_index = write_index_.load(std::memory_order_relaxed);
    const size_t read_index = read_index_.load(std::memory_order_acquire);
    return write_available_impl(write_index, read_index, max_elements_);
  }
  void reset() {
    if (!std::is_trivially_destructible<T>::value) {
      consume_all([](const T&) {});
    } else {
      write_index_.store(0, std::memory_order_relaxed);
      read_index_.store(0, std::memory_order_release);
    }
  }
  bool empty() {
    return empty_impl(write_index_.load(std::memory_order_relaxed), read_index_.load(std::memory_order_relaxed));
  }
  bool is_lock_free() const {
    return write_index_.is_lock_free() && read_index_.is_lock_free();
  }
};

}
