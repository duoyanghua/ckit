// SPDX-License-Identifier: GPL-3.0-or-later WITH GCC-exception-3.1 -- adapted from libstdc++ <memory_resource>.
#pragma once

#ifndef PMRT_TEACHING_ALLOCATOR_HPP
#define PMRT_TEACHING_ALLOCATOR_HPP

#include <ckit/pmr/pmr_teaching.hpp>

#include <cassert>
#include <cstddef>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace pmrt {

template <class T>
class polymorphic_allocator {
  memory_resource* resource_;
public:
  using value_type = T;
  using size_type = std::size_t;
  using difference_type = std::ptrdiff_t;
  using propagate_on_container_copy_assignment = std::false_type;
  using propagate_on_container_move_assignment = std::false_type;
  using propagate_on_container_swap = std::false_type;
  using is_always_equal = std::false_type;

  template <class U>
  struct rebind {
    using other = polymorphic_allocator<U>;
  };

  polymorphic_allocator() noexcept : resource_(get_default_resource()) {}

  polymorphic_allocator(memory_resource* resource) noexcept : resource_(resource) {
    assert(resource_ != nullptr);
  }
  polymorphic_allocator(const polymorphic_allocator&) noexcept = default;
  template <class U>
  polymorphic_allocator(const polymorphic_allocator<U>& other) noexcept
      : resource_(other.resource()) {}
  polymorphic_allocator&
  operator=(const polymorphic_allocator&) = delete;

  [[nodiscard]] memory_resource*
  resource() const noexcept {
    return resource_;
  }
  [[nodiscard]] size_type
  max_size() const noexcept {
    return std::numeric_limits<size_type>::max() / sizeof(T);
  }

  polymorphic_allocator
  select_on_container_copy_construction() const noexcept {
    return polymorphic_allocator();
  }
  [[nodiscard]] T*
  allocate(size_type n) {
    if (n > max_size())
      throw std::bad_array_new_length();
    return static_cast<T*>(resource_->allocate(n * sizeof(T), alignof(T)));
  }
  void
  deallocate(T* p, size_type n) noexcept {
    resource_->deallocate(p, n * sizeof(T), alignof(T));
  }

  [[nodiscard]] void*
  allocate_bytes(size_type nbytes, size_type alignment = alignof(std::max_align_t)) {
    return resource_->allocate(nbytes, alignment);
  }
  void
  deallocate_bytes(void* p, size_type nbytes, size_type alignment = alignof(std::max_align_t)) noexcept {
    resource_->deallocate(p, nbytes, alignment);
  }

  template <class U>
  [[nodiscard]] U*
  allocate_object(size_type n = 1) {
    if (n > std::numeric_limits<size_type>::max() / sizeof(U))
      throw std::bad_array_new_length();
    return static_cast<U*>(resource_->allocate(n * sizeof(U), alignof(U)));
  }
  template <class U>
  void
  deallocate_object(U* p, size_type n = 1) noexcept {
    resource_->deallocate(p, n * sizeof(U), alignof(U));
  }

  template <class U, class... Args>
  void
  construct(U* p, Args&&... args) {
    ::new (static_cast<void*>(p)) U(std::forward<Args>(args)...);
  }
  template <class U>
  void
  destroy(U* p) noexcept {
    p->~U();
  }
  template <class U, class... Args>
  [[nodiscard]] U*
  new_object(Args&&... args) {
    U* p = allocate_object<U>();
    try {
      construct(p, std::forward<Args>(args)...);
    } catch (...) {
      deallocate_object(p);
      throw;
    }
    return p;
  }
  template <class U>
  void
  delete_object(U* p) noexcept {
    destroy(p);
    deallocate_object(p);
  }
};

template <class T, class U>
[[nodiscard]] bool
operator==(const polymorphic_allocator<T>& a, const polymorphic_allocator<U>& b) noexcept {
  return *a.resource() == *b.resource();
}
template <class T, class U>
[[nodiscard]] bool
operator!=(const polymorphic_allocator<T>& a, const polymorphic_allocator<U>& b) noexcept {
  return !(a == b);
}

template <class T>
using vector = std::vector<T, polymorphic_allocator<T>>;

}

#endif
