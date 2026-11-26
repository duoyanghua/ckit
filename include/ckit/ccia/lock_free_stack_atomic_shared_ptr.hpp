// Study adaptation of A. Williams, C++ Concurrency in Action (2nd ed.).
#pragma once

#include <atomic>
#include <memory>
#include <utility>

namespace ckit::ccia {

namespace detail {

#if defined(__cpp_lib_atomic_shared_ptr)
template <class T>
using atomic_shared_ptr = std::atomic<std::shared_ptr<T>>;
#else
template <class T>
class atomic_shared_ptr {
  std::shared_ptr<T> value_;

 public:
  atomic_shared_ptr() noexcept = default;
  atomic_shared_ptr(std::nullptr_t) noexcept {}
  explicit atomic_shared_ptr(std::shared_ptr<T> value) noexcept : value_(std::move(value)) {}

  std::shared_ptr<T> load(std::memory_order order = std::memory_order_seq_cst) const {
    return std::atomic_load_explicit(&value_, order);
  }
  void store(std::shared_ptr<T> value, std::memory_order order = std::memory_order_seq_cst) {
    std::atomic_store_explicit(&value_, std::move(value), order);
  }
  bool compare_exchange_weak(std::shared_ptr<T>& expected, std::shared_ptr<T> desired,
                             std::memory_order success = std::memory_order_seq_cst,
                             std::memory_order failure = std::memory_order_seq_cst) {
    return std::atomic_compare_exchange_weak_explicit(&value_, &expected, std::move(desired),
                                                      success, failure);
  }
  bool compare_exchange_strong(std::shared_ptr<T>& expected, std::shared_ptr<T> desired,
                               std::memory_order success = std::memory_order_seq_cst,
                               std::memory_order failure = std::memory_order_seq_cst) {
    return std::atomic_compare_exchange_strong_explicit(&value_, &expected, std::move(desired),
                                                        success, failure);
  }
  atomic_shared_ptr& operator=(std::shared_ptr<T> value) {
    store(std::move(value));
    return *this;
  }
  operator std::shared_ptr<T>() const { return load(); }
};
#endif

}  // namespace detail


template <typename T>
class lock_free_stack_atomic_shared_ptr
{
private:
    struct node
    {
        std::shared_ptr<T> data;
        detail::atomic_shared_ptr<node> next;

        node(T const &data_) : data(std::make_shared<T>(data_)) {}
        node(T &&data_) : data(std::make_shared<T>(std::move(data_))) {}
    };
    detail::atomic_shared_ptr<node> head{nullptr};

public:
    void push(const T& data) requires std::is_copy_constructible_v<T> {
        std::shared_ptr<node> const new_node = std::make_shared<node>(data);
        push_impl(new_node);
    }
    void push(T&& data) requires std::move_constructible<T> {
        std::shared_ptr<node> const new_node = std::make_shared<node>(std::move(data));
        push_impl(new_node);
    }
private:
    void push_impl(std::shared_ptr<node> new_node)
    {
        std::shared_ptr<node> old_head = head.load();
        new_node->next.store(old_head);
        while (!head.compare_exchange_weak(old_head, new_node))
        {
            new_node->next.store(old_head);

        }
    }
public:
    std::shared_ptr<T> pop()
    {
        std::shared_ptr<node> old_head = head.load();
        while (old_head && !head.compare_exchange_weak(old_head, old_head->next.load()))
            ;
        if (old_head)
        {
            old_head->next = std::shared_ptr<node>();

            return old_head->data;
        }
        return std::shared_ptr<T>();
    }
    ~lock_free_stack_atomic_shared_ptr()
    {
        while (pop())
            ;
    }
};

}  // namespace ckit::ccia
