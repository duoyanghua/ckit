// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace xenium_qsbr {

#if defined(__SANITIZE_THREAD__)
  #define QSBR_TSAN 1
#elif defined(__has_feature)
  #if __has_feature(thread_sanitizer)
    #define QSBR_TSAN 1
  #endif
#endif

#if defined(QSBR_TSAN)
  #define QSBR_LOAD_ORDER std::memory_order_acquire
  #if defined(__GNUC__) && !defined(__clang__)
    #define QSBR_FENCE(order)                        \
      _Pragma("GCC diagnostic push")                 \
      _Pragma("GCC diagnostic ignored \"-Wtsan\"")   \
      std::atomic_thread_fence(order);               \
      _Pragma("GCC diagnostic pop")
  #else
    #define QSBR_FENCE(order) std::atomic_thread_fence(order)
  #endif
#else
  #define QSBR_LOAD_ORDER std::memory_order_relaxed
  #define QSBR_FENCE(order) std::atomic_thread_fence(order)
#endif

namespace detail {

struct deletable_object {
  virtual void delete_self() = 0;
  deletable_object* next = nullptr;
protected:
  virtual ~deletable_object() = default;
};

inline void delete_objects(deletable_object*& list) {
  auto* cur = list;
  for (deletable_object* next = nullptr; cur != nullptr; cur = next) {
    next = cur->next;
    cur->delete_self();
  }
  list = nullptr;
}

template <unsigned Epochs>
struct orphan : deletable_object {
private:
  std::array<deletable_object*, Epochs> retire_lists{};
public:
  const unsigned target_epoch;

public:

  orphan(unsigned target_epoch, std::array<deletable_object*, Epochs>& lists) : target_epoch(target_epoch) {
    for (unsigned i = 0; i < Epochs; ++i) {
      retire_lists[i] = lists[i];
      lists[i] = nullptr;
    }
  }

  ~orphan() override {
    for (auto* list : retire_lists) {
      delete_objects(list);
    }
  }

  void delete_self() override { delete this; }
};

enum class entry_state { free, inactive, active };

template <class T>
struct thread_block_list {

  class entry {
    friend class thread_block_list;
    T* next_entry;
    std::atomic<entry_state> state;
  private:
    bool try_adopt(entry_state initial_state) {
      if (state.load(std::memory_order_relaxed) == entry_state::free) {
        auto expected = entry_state::free;
        return state.compare_exchange_strong(expected, initial_state, std::memory_order_acquire);
      }
      return false;
    }
  public:
    entry() : next_entry(nullptr), state(entry_state::active) {}

    [[nodiscard]] bool is_active(std::memory_order order = std::memory_order_relaxed) const {
      return state.load(order) == entry_state::active;
    }
    void abandon() {
      assert(is_active());
      state.store(entry_state::free, std::memory_order_release);
    }

    void activate() {
      assert(state.load(std::memory_order_relaxed) == entry_state::inactive);
      state.store(entry_state::active, std::memory_order_release);
    }
  };

private:

  std::atomic<T*> head_{nullptr};
  alignas(64) std::atomic<deletable_object*> abandoned_retired_nodes_{nullptr};

private:

  void add_entry(T* node) {
    static_assert(std::is_base_of<entry, T>::value, "T 必须继承 thread_block_list::entry");
    T* h = head_.load(std::memory_order_relaxed);
    do {
      node->next_entry = h;
    } while (!head_.compare_exchange_weak(h, node, std::memory_order_release, std::memory_order_relaxed));
  }

  T* adopt_or_create_entry(entry_state initial_state) {
    T* result = head_.load(std::memory_order_acquire);
    while (result != nullptr) {
      if (result->try_adopt(initial_state))
        return result;
      result = result->next_entry;
    }
    result = new T();
    result->state.store(initial_state, std::memory_order_relaxed);
    add_entry(result);
    return result;
  }

public:
  T* acquire_entry() {
    return adopt_or_create_entry(entry_state::active);
  }
  void release_entry(T* entry) {
    entry->abandon();
  }

  template <class Fn>
  void for_each_entry(Fn&& fn) {
    for (T* e = head_.load(std::memory_order_acquire); e != nullptr; e = e->next_entry)
      std::forward<Fn>(fn)(*e);
  }

  void abandon_retired_nodes(deletable_object* bag) {
    auto* last = bag;
    while (last->next != nullptr) {
      last = last->next;
    }
    auto* h = abandoned_retired_nodes_.load(std::memory_order_relaxed);
    do {
      last->next = h;
    } while (!abandoned_retired_nodes_.compare_exchange_weak(h, bag, std::memory_order_release, std::memory_order_relaxed));
  }

  deletable_object* adopt_abandoned_retired_nodes() {
    if (abandoned_retired_nodes_.load(std::memory_order_relaxed) == nullptr) {
      return nullptr;
    }

    return abandoned_retired_nodes_.exchange(nullptr, std::memory_order_acquire);
  }
};

}

template <class T>
class enable_concurrent_ptr : public detail::deletable_object {
public:
  static constexpr std::size_t number_of_mark_bits = 0;
protected:
  enable_concurrent_ptr() noexcept = default;
  enable_concurrent_ptr(const enable_concurrent_ptr&) noexcept = default;
  enable_concurrent_ptr(enable_concurrent_ptr&&) noexcept = default;
  enable_concurrent_ptr& operator=(const enable_concurrent_ptr&) noexcept = default;
  enable_concurrent_ptr& operator=(enable_concurrent_ptr&&) noexcept = default;
  ~enable_concurrent_ptr() override = default;
private:

  void delete_self() override { delete static_cast<T*>(this); }
};

template <class T, class Reclaimer>
class guard_ptr {
  T* ptr_ = nullptr;

public:
  guard_ptr() noexcept = default;

  explicit guard_ptr(T* p) : ptr_(p) {
    if (ptr_ != nullptr) {
      Reclaimer::local_thread_data().enter_region();
    }
  }
  ~guard_ptr() {
    reset();
  }

  void reset() noexcept {
    if (ptr_ != nullptr)
      Reclaimer::local_thread_data().leave_region();
    ptr_ = nullptr;
  }

  void acquire(const std::atomic<T*>& p, std::memory_order order = std::memory_order_seq_cst) {
    if (p.load(std::memory_order_relaxed) == nullptr) {
      reset();
      return;
    }

    if (ptr_ == nullptr) {
      Reclaimer::local_thread_data().enter_region();
    }
    ptr_ = p.load(order);
    if (ptr_ == nullptr) {
      Reclaimer::local_thread_data().leave_region();
    }
  }

  bool acquire_if_equal(const std::atomic<T*>& p,
                        const T* expected,
                        std::memory_order order = std::memory_order_seq_cst) {
    T* actual = p.load(std::memory_order_relaxed);
    if (actual == nullptr || actual != expected) {
      reset();
      return actual == expected;
    }
    if (ptr_ == nullptr) {
      Reclaimer::local_thread_data().enter_region();
    }
    ptr_ = p.load(order);
    if (ptr_ == nullptr || ptr_ != expected) {
      Reclaimer::local_thread_data().leave_region();
      ptr_ = nullptr;
    }
    return ptr_ == expected;
  }

  void reclaim() {
    assert(ptr_ != nullptr);
    Reclaimer::local_thread_data().add_retired_node(ptr_);
    reset();
  }

public:
  [[nodiscard]] T* get() const noexcept { return ptr_; }
  T* operator->() const noexcept { return ptr_; }
  T& operator*() const noexcept { return *ptr_; }
  explicit operator bool() const noexcept { return ptr_ != nullptr; }
  void swap(guard_ptr& g) noexcept { std::swap(ptr_, g.ptr_); }

  guard_ptr(const guard_ptr& p) : guard_ptr(p.ptr_) {}
  guard_ptr(guard_ptr&& p) noexcept : ptr_(p.ptr_) { p.ptr_ = nullptr; }

  guard_ptr& operator=(const guard_ptr& p) {
    if (&p == this) return *this;
    reset();
    ptr_ = p.ptr_;
    if (ptr_ != nullptr) {
      Reclaimer::local_thread_data().enter_region();
    }
    return *this;
  }

  guard_ptr& operator=(guard_ptr&& p) noexcept {
    if (&p == this) return *this;
    reset();
    ptr_ = p.ptr_;
    p.ptr_ = nullptr;
    return *this;
  }
};

class quiescent_state_based {
private:
  template <class, class>
  friend class guard_ptr;
  struct thread_control_block;
  struct thread_data;

  static constexpr unsigned number_epochs = 3;

  inline static std::atomic<unsigned> global_epoch{0};
  inline static detail::thread_block_list<thread_control_block> global_thread_block_list;

  static thread_data& local_thread_data();

public:
  template <class T>
  using enable_concurrent_ptr = xenium_qsbr::enable_concurrent_ptr<T>;

  struct region_guard {
    region_guard() noexcept;
    ~region_guard() noexcept;
    region_guard(const region_guard&) = delete;
    region_guard(region_guard&&) = delete;
    region_guard& operator=(const region_guard&) = delete;
    region_guard& operator=(region_guard&&) = delete;
  };
};

struct quiescent_state_based::thread_control_block : detail::thread_block_list<thread_control_block>::entry {
  std::atomic<unsigned> local_epoch{0};
};

struct quiescent_state_based::thread_data {
private:
  template <class, class>
  friend class guard_ptr;
  friend class quiescent_state_based;

  unsigned region_entries = 0;
  thread_control_block* control_block = nullptr;
  std::array<detail::deletable_object*, number_epochs> retire_lists = {};

private:

  void ensure_has_control_block() {
    if (control_block == nullptr) {
      control_block = global_thread_block_list.acquire_entry();
      auto epoch = global_epoch.load(std::memory_order_relaxed);
      do {
        control_block->local_epoch.store(epoch, std::memory_order_release);
      } while (!global_epoch.compare_exchange_weak(epoch, epoch, std::memory_order_acq_rel, std::memory_order_relaxed));
    }
  }

  void quiescent_state() {
    auto epoch = global_epoch.load(std::memory_order_acquire);
    bool success = false;
    if (control_block->local_epoch.load(std::memory_order_relaxed) == epoch) {
      const auto new_epoch = (epoch + 1) % number_epochs;
      if (!try_update_epoch(epoch, new_epoch, success)) {
        return;
      }
      epoch = new_epoch;
    }

    detail::delete_objects(retire_lists[epoch]);
    if (success) {
      adopt_orphans();
    }
    control_block->local_epoch.store(epoch, std::memory_order_release);
  }

  bool try_update_epoch(unsigned curr_epoch, unsigned new_epoch, bool& success) {
    const auto old_epoch = (curr_epoch + number_epochs - 1) % number_epochs;
    bool cannot_update = false;
    global_thread_block_list.for_each_entry(
      [&](const thread_control_block& data) {
        if (data.local_epoch.load(QSBR_LOAD_ORDER) == old_epoch && data.is_active(QSBR_LOAD_ORDER))
          cannot_update = true;
      }
    );
    if (cannot_update) {
      return false;
    }
    QSBR_FENCE(std::memory_order_acquire);
    if (global_epoch.load(std::memory_order_relaxed) == curr_epoch) {
      success = global_epoch.compare_exchange_strong(curr_epoch, new_epoch, std::memory_order_acq_rel, std::memory_order_relaxed);
    }
    return true;
  }

  void adopt_orphans() {
    auto* cur = global_thread_block_list.adopt_abandoned_retired_nodes();
    for (detail::deletable_object* next = nullptr; cur != nullptr; cur = next) {
      next = cur->next;
      cur->next = nullptr;
      add_retired_node(cur, static_cast<detail::orphan<number_epochs>*>(cur)->target_epoch);
    }
  }

  void add_retired_node(detail::deletable_object* p, unsigned epoch) {
    assert(epoch < number_epochs);
    p->next = retire_lists[epoch];
    retire_lists[epoch] = p;
  }

public:

  ~thread_data() {
    assert(region_entries == 0);
    if (control_block == nullptr) {
      return;
    }
    bool has_retired = false;
    for (auto* list : retire_lists) {
      if (list != nullptr) {
        has_retired = true;
        break;
      }
    }
    if (has_retired) {
      const auto target_epoch = control_block->local_epoch.load(std::memory_order_relaxed);
      global_thread_block_list.abandon_retired_nodes(new detail::orphan<number_epochs>(target_epoch, retire_lists));
    }
    global_thread_block_list.release_entry(control_block);
    control_block = nullptr;
  }

  void enter_region() {
    ensure_has_control_block();
    ++region_entries;
  }
  void leave_region() {
    assert(region_entries > 0);
    if (--region_entries == 0) {
      quiescent_state();
    }
  }

  void add_retired_node(detail::deletable_object* p) {
    assert(control_block != nullptr);
    add_retired_node(p, control_block->local_epoch.load(std::memory_order_relaxed));
  }

};

inline quiescent_state_based::region_guard::region_guard() noexcept {
  local_thread_data().enter_region();
}

inline quiescent_state_based::region_guard::~region_guard() noexcept {
  local_thread_data().leave_region();
}

inline quiescent_state_based::thread_data& quiescent_state_based::local_thread_data() {
  static thread_local thread_data data;
  return data;
}

#undef QSBR_TSAN
#undef QSBR_LOAD_ORDER
#undef QSBR_FENCE

}
