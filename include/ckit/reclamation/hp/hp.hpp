// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace hp {

#if defined(__SANITIZE_THREAD__)
  #define HP_TSAN 1
#elif defined(__has_feature)
  #if __has_feature(thread_sanitizer)
    #define HP_TSAN 1
  #endif
#endif

#if defined(HP_TSAN)
  #define HP_SLOT_LOAD_ORDER std::memory_order_acquire
  #if defined(__GNUC__) && !defined(__clang__)
    #define HP_FENCE(order)                         \
      _Pragma("GCC diagnostic push")                \
      _Pragma("GCC diagnostic ignored \"-Wtsan\"")  \
      std::atomic_thread_fence(order);              \
      _Pragma("GCC diagnostic pop")
  #else
    #define HP_FENCE(order) std::atomic_thread_fence(order)
  #endif
#else
  #define HP_SLOT_LOAD_ORDER std::memory_order_relaxed
  #define HP_FENCE(order) std::atomic_thread_fence(order)
#endif

namespace detail {

struct deletable_object {
  virtual void delete_self() = 0;
  deletable_object* next = nullptr;
protected:
  virtual ~deletable_object() = default;
};

class slot {
  static constexpr std::uintptr_t kLinkTag = 1;
  std::atomic<std::uintptr_t> value_{0};

public:

  void set_object(deletable_object* obj) {
    value_.store(reinterpret_cast<std::uintptr_t>(obj), std::memory_order_release);
    HP_FENCE(std::memory_order_seq_cst);
  }

  void set_link(slot* link) {
    value_.store(reinterpret_cast<std::uintptr_t>(link) | kLinkTag, std::memory_order_release);
  }

  bool try_get_object(deletable_object*& out) const {
    const std::uintptr_t v = value_.load(HP_SLOT_LOAD_ORDER);
    if ((v & kLinkTag) == 0) {
      out = reinterpret_cast<deletable_object*>(v);
      return true;
    }
    return false;
  }

  bool is_link() const {
    return (value_.load(std::memory_order_relaxed) & kLinkTag) != 0;
  }

  slot* get_link() const {
    assert(is_link());
    return reinterpret_cast<slot*>(value_.load(std::memory_order_relaxed) & ~kLinkTag);
  }
};

template <class Derived>
class thread_block_list {
  enum class entry_state { free, inactive, active };
  std::atomic<Derived*> head_{nullptr};
  alignas(64) std::atomic<deletable_object*> abandoned_retired_nodes_{nullptr};

public:

  class entry {
    friend class thread_block_list;
    Derived* next_entry;
    std::atomic<entry_state> state;

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
  static_assert(std::is_base_of<entry, Derived>::value, "Derived 必须继承 thread_block_list::entry");

private:

  void add_entry(Derived* node) {
    Derived* h = head_.load(std::memory_order_relaxed);
    do {
      node->next_entry = h;
    } while (!head_.compare_exchange_weak(h, node, std::memory_order_release, std::memory_order_relaxed));
  }

  Derived* adopt_or_create_entry(entry_state initial_state) {
    Derived* result = head_.load(std::memory_order_acquire);
    while (result != nullptr) {
      if (result->try_adopt(initial_state))
        return result;
      result = result->next_entry;
    }
    result = new Derived();
    result->state.store(initial_state, std::memory_order_relaxed);
    add_entry(result);
    return result;
  }

public:

  Derived* acquire_entry() {
    return adopt_or_create_entry(entry_state::active);
  }
  Derived* acquire_inactive_entry() {
    return adopt_or_create_entry(entry_state::inactive);
  }

  void release_entry(Derived* entry) {
    entry->abandon();
  }

  template <class Fn>
  void for_each_entry(Fn&& fn) {
    for (Derived* e = head_.load(std::memory_order_acquire); e != nullptr; e = e->next_entry) {
      std::forward<Fn>(fn)(*e);
    }
  }

  void abandon_retired_nodes(deletable_object* list) {
    auto* last = list;
    while (last->next != nullptr) {
      last = last->next;
    }
    auto* h = abandoned_retired_nodes_.load(std::memory_order_relaxed);
    do {
      last->next = h;
    } while (!abandoned_retired_nodes_.compare_exchange_weak(h, list, std::memory_order_release, std::memory_order_relaxed));
  }

  deletable_object* adopt_abandoned_retired_nodes() {
    if (abandoned_retired_nodes_.load(std::memory_order_relaxed) == nullptr) {
      return nullptr;
    }
    return abandoned_retired_nodes_.exchange(nullptr, std::memory_order_acquire);
  }
};

}

class bad_hazard_pointer_alloc : public std::runtime_error {
  using std::runtime_error::runtime_error;
};

namespace detail {

template <class Strategy>
struct static_thread_control_block;
template <class Strategy>
struct dynamic_thread_control_block;

}

template <std::size_t K_, std::size_t A_ = 2, std::size_t B_ = 100>
struct static_strategy {

  using thread_control_block = detail::static_thread_control_block<static_strategy>;

  static constexpr std::size_t K = K_;
  static constexpr bool is_dynamic = false;
  inline static std::atomic<std::size_t> active_hps{0};

  static std::size_t active_hp_count() {
    return active_hps.load(std::memory_order_relaxed);
  }
  static std::size_t retired_nodes_threshold() {
    return A_ * active_hp_count() + B_;
  }
};

template <std::size_t K_, std::size_t A_ = 2, std::size_t B_ = 100>
struct dynamic_strategy {

  using thread_control_block = detail::dynamic_thread_control_block<dynamic_strategy>;
  static constexpr std::size_t K = K_;
  static constexpr bool is_dynamic = true;
  inline static std::atomic<std::size_t> active_hps{0};

  static std::size_t active_hp_count() {
    return active_hps.load(std::memory_order_relaxed);
  }
  static std::size_t retired_nodes_threshold() {
    return A_ * active_hp_count() + B_;
  }
};

namespace detail {

template <class Strategy, class Derived>
struct alignas(64) basic_thread_control_block : thread_block_list<Derived>::entry {
  using slot = detail::slot;
protected:
  slot pointers_[Strategy::K];

  slot* begin() { return &pointers_[0]; }
  slot* end() { return &pointers_[Strategy::K]; }
  const slot* begin() const { return &pointers_[0]; }
  const slot* end() const { return &pointers_[Strategy::K]; }

  Derived& self() { return static_cast<Derived&>(*this); }

  template <class Block>
  static slot* initialize_block(Block& block) {
    slot* first = block.begin();
    slot* last = block.end() - 1;
    for (slot* it = first; it != last; ++it) {
      it->set_link(it + 1);
    }
    last->set_link(block.initialize_next_block());
    return first;
  }

  static void
  gather_protected_pointers(std::vector<const deletable_object*>& out, const slot* first, const slot* last) {
    for (const slot* it = first; it != last; ++it) {
      deletable_object* obj = nullptr;
      if (it->try_get_object(obj)) {
        out.push_back(obj);
      }
    }
  }

public:

  void initialize(slot*& hint) {
    Strategy::active_hps.fetch_add(self().number_of_hps(), std::memory_order_relaxed);
    hint = initialize_block(self());
  }

  void abandon() {
    Strategy::active_hps.fetch_sub(self().number_of_hps(), std::memory_order_relaxed);
    thread_block_list<Derived>::entry::abandon();
  }

  slot* alloc(slot*& hint) {
    slot* result = hint;
    if (result == nullptr) {
      result = self().need_more_hps();
    }
    hint = result->get_link();
    return result;
  }

  void release(slot*& hp, slot*& hint) {
    if (hp == nullptr)
      return;
    hp->set_link(hint);
    hint = hp;
    hp = nullptr;
  }
};

template <class Strategy>
struct static_thread_control_block : basic_thread_control_block<Strategy, static_thread_control_block<Strategy>> {
  using base = basic_thread_control_block<Strategy, static_thread_control_block<Strategy>>;
  using slot = typename base::slot;
  friend base;

  void gather_protected_pointers(std::vector<const deletable_object*>& out) const {
    base::gather_protected_pointers(out, this->begin(), this->end());
  }
private:
  slot* need_more_hps() {
    throw bad_hazard_pointer_alloc("hazard pointer pool exceeded");
  }
  [[nodiscard]] constexpr std::size_t number_of_hps() const {
    return Strategy::K;
  }
  [[nodiscard]] constexpr slot* initialize_next_block() const {
    return nullptr;
  }
};

template <class Strategy>
struct dynamic_thread_control_block : basic_thread_control_block<Strategy, dynamic_thread_control_block<Strategy>> {
  using base = basic_thread_control_block<Strategy, dynamic_thread_control_block<Strategy>>;
  using slot = typename base::slot;
  friend base;

  void gather_protected_pointers(std::vector<const deletable_object*>& out) const {
    gather_block(*this, out);
  }
private:

  struct alignas(64) block {
    std::size_t size;
    block* next = nullptr;

    slot* slot_at(std::size_t i) { return reinterpret_cast<slot*>(this + 1) + i; }
    const slot* slot_at(std::size_t i) const { return reinterpret_cast<const slot*>(this + 1) + i; }
    slot* begin() { return slot_at(0); }
    slot* end() { return slot_at(size); }
    const slot* begin() const { return slot_at(0); }
    const slot* end() const { return slot_at(size); }

    const block* next_block() const {
      return next;
    }

    slot* initialize_next_block() {
      return next != nullptr ? base::initialize_block(*next) : nullptr;
    }
    explicit block(std::size_t n) : size(n) {
      for (std::size_t i = 0; i < size; ++i) {
        ::new (slot_at(i)) slot();
      }
    }
  };

private:
  std::size_t total_number_of_hps_ = Strategy::K;
  std::atomic<block*> hp_block_{nullptr};

  [[nodiscard]] block* next_block() const {
    return hp_block_.load(std::memory_order_acquire);
  }

  [[nodiscard]] std::size_t number_of_hps() const {
    return total_number_of_hps_;
  }
  slot* need_more_hps() {
    return allocate_new_hazard_pointer_block();
  }

  slot* initialize_next_block() {
    block* b = hp_block_.load(std::memory_order_relaxed);
    return b != nullptr ? base::initialize_block(*b) : nullptr;
  }

  template <class Block>
  static void gather_block(const Block& blk, std::vector<const deletable_object*>& out) {
    base::gather_protected_pointers(out, blk.begin(), blk.end());
    if (const block* n = blk.next_block()) {
      gather_block(*n, out);
    }
  }

  slot* allocate_new_hazard_pointer_block() {
    const std::size_t hps = std::max(static_cast<std::size_t>(Strategy::K), total_number_of_hps_ / 2);
    total_number_of_hps_ += hps;
    Strategy::active_hps.fetch_add(hps, std::memory_order_relaxed);
    void* raw = ::operator new(sizeof(block) + hps * sizeof(slot), std::align_val_t{alignof(block)});
    auto* b = ::new (raw) block(hps);
    slot* first = base::initialize_block(*b);
    b->next = hp_block_.load(std::memory_order_relaxed);
    hp_block_.store(b, std::memory_order_release);
    return first;
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
class guard_ptr;

template <class T, class Reclaimer>
class concurrent_ptr {
private:
  std::atomic<T*> ptr_;
public:
  using guard_ptr = hp::guard_ptr<T, Reclaimer>;
  concurrent_ptr() noexcept : ptr_(nullptr) {}
  concurrent_ptr(const concurrent_ptr&) = delete;
  concurrent_ptr(concurrent_ptr&&) = delete;
  concurrent_ptr& operator=(const concurrent_ptr&) = delete;
  concurrent_ptr& operator=(concurrent_ptr&&) = delete;

  [[nodiscard]] T* load(std::memory_order order = std::memory_order_seq_cst) const { return ptr_.load(order); }

  void store(T* src, std::memory_order order = std::memory_order_seq_cst) { ptr_.store(src, order); }

  void store(const guard_ptr& src, std::memory_order order = std::memory_order_seq_cst) {
    ptr_.store(src.get(), order);
  }

  bool  compare_exchange_weak(T*& expected, T* desired, std::memory_order order = std::memory_order_seq_cst) {
    return ptr_.compare_exchange_weak(expected, desired, order);
  }
  bool compare_exchange_weak(T*& expected, T* desired, std::memory_order success, std::memory_order failure) {
    return ptr_.compare_exchange_weak(expected, desired, success, failure);
  }
  bool compare_exchange_strong(T*& expected, T* desired, std::memory_order order = std::memory_order_seq_cst) {
    return ptr_.compare_exchange_strong(expected, desired, order);
  }
  bool compare_exchange_strong(T*& expected, T* desired, std::memory_order success, std::memory_order failure) {
    return ptr_.compare_exchange_strong(expected, desired, success, failure);
  }
};

template <class T, class Reclaimer>
class guard_ptr {
  using slot = typename Reclaimer::slot;
  using concurrent_ptr_t = hp::concurrent_ptr<T, Reclaimer>;
private:
  T* ptr_ = nullptr;
  slot* hp_ = nullptr;

public:

  guard_ptr() noexcept = default;
  explicit guard_ptr(T* p) : ptr_(p) {
    if (ptr_ != nullptr) {
      hp_ = Reclaimer::local_thread_data.alloc_hazard_pointer();
      hp_->set_object(ptr_);
    }
  }
  ~guard_ptr() { reset(); }

  void reset() noexcept {
    if (hp_ != nullptr) {
      Reclaimer::local_thread_data.release_hazard_pointer(hp_);
    }
    ptr_ = nullptr;
  }

  guard_ptr(const guard_ptr& p) : guard_ptr(p.ptr_) {}
  guard_ptr(guard_ptr&& p) noexcept : ptr_(p.ptr_), hp_(p.hp_) {
    p.ptr_ = nullptr;
    p.hp_ = nullptr;
  }
  guard_ptr& operator=(const guard_ptr& p) {
    if (&p == this) {
      return *this;
    }
    if (hp_ == nullptr) {
      hp_ = Reclaimer::local_thread_data.alloc_hazard_pointer();
    }
    ptr_ = p.ptr_;
    hp_->set_object(ptr_);
    return *this;
  }
  guard_ptr& operator=(guard_ptr&& p) noexcept {
    if (&p == this) {
      return *this;
    }
    reset();
    ptr_ = p.ptr_;
    hp_ = p.hp_;
    p.ptr_ = nullptr;
    p.hp_ = nullptr;
    return *this;
  }

  void acquire(const concurrent_ptr_t& p, std::memory_order order = std::memory_order_seq_cst) {
    T* p1 = p.load(std::memory_order_relaxed);
    if (p1 == nullptr) {
        reset();
        return;
    }
    if (p1 == ptr_) {
      return;
    }
    if (hp_ == nullptr) {
      hp_ = Reclaimer::local_thread_data.alloc_hazard_pointer();
    }
    T* p2 = p1;
    do {
      if (p2 == nullptr) {
        reset();
        return;
      }
      p1 = p2;
      hp_->set_object(p1);
      p2 = p.load(order);
    } while (p1 != p2);
    ptr_ = p2;
  }

  bool acquire_if_equal(const concurrent_ptr_t& p,
                        const T* expected,
                        std::memory_order order = std::memory_order_seq_cst) {
    T* p1 = p.load(std::memory_order_relaxed);
    if (p1 == nullptr || p1 != expected) {
      reset();
      return p1 == expected;
    }
    if (hp_ == nullptr) {
      hp_ = Reclaimer::local_thread_data.alloc_hazard_pointer();
    }
    hp_->set_object(p1);
    T* p2 = p.load(order);
    if (p2 != p1) {
      reset();
      return false;
    }
    ptr_ = p2;
    return true;
  }

  void reclaim() {
    assert(ptr_ != nullptr);
    auto* obj = static_cast<detail::deletable_object*>(ptr_);
    reset();
    if (Reclaimer::local_thread_data.add_retired_node(obj) >= Reclaimer::strategy::retired_nodes_threshold()) {
      Reclaimer::local_thread_data.scan();
    }
  }

  [[nodiscard]] T* get() const noexcept { return ptr_; }
  T* operator->() const noexcept { return ptr_; }
  T& operator*() const noexcept { return *ptr_; }
  explicit operator bool() const noexcept { return ptr_ != nullptr; }

  void swap(guard_ptr& g) noexcept {
    std::swap(ptr_, g.ptr_);
    std::swap(hp_, g.hp_);
  }
};

template <class Strategy = static_strategy<3>>
class hazard_pointer {
public:
  using strategy = Strategy;
  using thread_control_block = typename Strategy::thread_control_block;
  using slot = detail::slot;

  template <class T>
  using enable_concurrent_ptr = hp::enable_concurrent_ptr<T>;

  template <class T>
  using concurrent_ptr = hp::concurrent_ptr<T, hazard_pointer>;

  struct thread_data;

private:
  template <class, class>
  friend class guard_ptr;

  inline static detail::thread_block_list<thread_control_block> global_thread_block_list;
  inline static thread_local thread_data local_thread_data;
};

template <class Strategy>
struct hazard_pointer<Strategy>::thread_data {

  ~thread_data() {
    if (retire_list != nullptr) {
      scan();
      if (retire_list != nullptr) {

        global_thread_block_list.abandon_retired_nodes(retire_list);
      }
      retire_list = nullptr;
    }
    if (control_block != nullptr) {
      global_thread_block_list.release_entry(control_block);
      control_block = nullptr;
    }
  }

  slot* alloc_hazard_pointer() {
    ensure_has_control_block();
    return control_block->alloc(hint);
  }

  void release_hazard_pointer(slot*& hp) {
    if (hp == nullptr) {
      return;
    }
    control_block->release(hp, hint);
  }

  std::size_t add_retired_node(detail::deletable_object* p) {
    p->next = retire_list;
    retire_list = p;
    return ++number_of_retired_nodes;
  }

  void scan() {
    std::vector<const detail::deletable_object*> protected_pointers;
    protected_pointers.reserve(Strategy::active_hp_count());

    HP_FENCE(std::memory_order_seq_cst);

    global_thread_block_list.for_each_entry([&protected_pointers](const auto& entry) {
      if (entry.is_active(HP_SLOT_LOAD_ORDER)) {
        entry.gather_protected_pointers(protected_pointers);
      }
    });

    HP_FENCE(std::memory_order_acquire);

    std::sort(protected_pointers.begin(), protected_pointers.end());

    auto* adopted_nodes = global_thread_block_list.adopt_abandoned_retired_nodes();

    auto* list = retire_list;
    retire_list = nullptr;
    number_of_retired_nodes = 0;
    reclaim_nodes(list, protected_pointers);
    reclaim_nodes(adopted_nodes, protected_pointers);
  }

private:
  void ensure_has_control_block() {
    if (control_block == nullptr) {
      control_block = global_thread_block_list.acquire_entry();
      control_block->initialize(hint);
    }
  }
  void reclaim_nodes(detail::deletable_object* list,
                     const std::vector<const detail::deletable_object*>& protected_pointers) {
    while (list != nullptr) {
      auto* cur = list;
      list = list->next;
      if (std::binary_search(protected_pointers.begin(), protected_pointers.end(), cur)) {
        add_retired_node(cur);
      } else {
        cur->delete_self();
      }
    }
  }

  detail::deletable_object* retire_list = nullptr;
  std::size_t number_of_retired_nodes = 0;
  slot* hint = nullptr;
  thread_control_block* control_block = nullptr;
};

#undef HP_TSAN
#undef HP_SLOT_LOAD_ORDER
#undef HP_FENCE

}
