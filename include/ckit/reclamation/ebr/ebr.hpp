// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace ebr {

#if defined(__SANITIZE_THREAD__)
  #define EBR_TSAN 1
#elif defined(__has_feature)
  #if __has_feature(thread_sanitizer)
    #define EBR_TSAN 1
  #endif
#endif

#if defined(EBR_TSAN)
  #define EBR_LOAD_ORDER std::memory_order_acquire
  #if defined(__GNUC__) && !defined(__clang__)
    #define EBR_FENCE(order)                        \
      _Pragma("GCC diagnostic push")                \
      _Pragma("GCC diagnostic ignored \"-Wtsan\"")  \
      std::atomic_thread_fence(order);              \
      _Pragma("GCC diagnostic pop")
  #else
    #define EBR_FENCE(order) std::atomic_thread_fence(order)
  #endif
#else
  #define EBR_LOAD_ORDER std::memory_order_relaxed
  #define EBR_FENCE(order) std::atomic_thread_fence(order)
#endif

namespace detail {

struct deletable_object {
  virtual void delete_self() = 0;
  deletable_object* next = nullptr;
protected:
  virtual ~deletable_object() = default;
};

inline void delete_objects(deletable_object* list) {
  while (list != nullptr) {
    deletable_object* next = list->next;
    list->delete_self();
    list = next;
  }
}

template <class Node = deletable_object>
struct retired_nodes {
  Node* first = nullptr;
  Node* last = nullptr;
};

template <class Node = deletable_object>
class retire_list {
  Node* first_ = nullptr;
  Node* last_ = nullptr;

public:
  retire_list() = default;
  retire_list(const retire_list&) = delete;
  retire_list& operator=(const retire_list&) = delete;
  ~retire_list() { assert(first_ == nullptr); }
  [[nodiscard]] bool empty() const { return first_ == nullptr;  }

  void push(Node* node) {
    node->next = first_;
    first_ = node;
    if (last_ == nullptr)   last_ = node;
  }

  retired_nodes<Node> steal() {
    retired_nodes<Node> result{first_, last_};
    first_ = nullptr;
    last_ = nullptr;
    return result;
  }
};

template <class Node = deletable_object>
class counting_retire_list {
private:
  retire_list<Node> list_;
  std::size_t count_ = 0;
public:
  [[nodiscard]] bool empty() const {  return list_.empty();  }
  [[nodiscard]] std::size_t size() const { return count_; }

  void push(Node* node) {
    list_.push(node);
    ++count_;
  }

  retired_nodes<Node> steal() {
    count_ = 0;
    return list_.steal();
  }
};

template <class Node = deletable_object>
class orphan_list {
private:
  std::atomic<Node*> head_{nullptr};
public:

  void add(retired_nodes<Node> nodes) {
    assert(nodes.first != nullptr);
    Node* h = head_.load(std::memory_order_relaxed);
    do {
      nodes.last->next = h;
    } while (!head_.compare_exchange_weak(h, nodes.first, std::memory_order_release, std::memory_order_relaxed));
  }

  Node* adopt() {
    if (head_.load(std::memory_order_relaxed) == nullptr) return nullptr;
    return head_.exchange(nullptr, std::memory_order_acquire);
  }
};

enum class entry_state { free, inactive, active };

template <class T>
class thread_block_list {
private:
  std::atomic<T*> head_{nullptr};
public:

  struct entry {
  private:
    friend class thread_block_list<T>;
    T* next_entry;
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
      if (result->try_adopt(initial_state)) return result;
      result = result->next_entry;
    }
    result = new T();
    result->state.store(initial_state, std::memory_order_relaxed);
    add_entry(result);
    return result;
  }

public:

  T* acquire_entry() { return adopt_or_create_entry(entry_state::active); }

  T* acquire_inactive_entry() { return adopt_or_create_entry(entry_state::inactive); }

  void release_entry(T* entry) {
    entry->abandon();
  }

  [[nodiscard]] T* head() const {
    return head_.load(std::memory_order_acquire);
  }

  [[nodiscard]] static T* next(T* e) {
    return e->next_entry;
  }
};

}

template <class T>
struct enable_concurrent_ptr : public detail::deletable_object {
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

enum class region_extension {
  none,
  eager,
  lazy,
};

struct scan_all_threads {
  template <class Reclaimer>
  struct type {
    bool scan(typename Reclaimer::epoch_t epoch) {
      for (auto* e = Reclaimer::global_thread_block_list.head(); e != nullptr; e = Reclaimer::global_thread_block_list.next(e)) {
        if (e->is_in_critical_region.load(EBR_LOAD_ORDER) && e->local_epoch.load(EBR_LOAD_ORDER) != epoch)
          return false;
      }
      return true;
    }
    void reset() {}
  };
};

template <unsigned N>
struct scan_n_threads {
  template <class Reclaimer>
  struct type {
    using tcb = typename Reclaimer::thread_control_block;
  private:
    tcb* cursor = nullptr;
    bool need_reset = false;
  public:
    type() {
      reset();
    }
    void reset() {
      need_reset = true;
    }
    bool scan(typename Reclaimer::epoch_t epoch) {
      if(need_reset) {
        cursor = Reclaimer::global_thread_block_list.head();
        need_reset = false;
      }
      if (cursor == nullptr) {
        return true;
      }

      for (unsigned i = 0; i < N; ++i) {
        if (cursor->is_in_critical_region.load(EBR_LOAD_ORDER) && cursor->local_epoch.load(EBR_LOAD_ORDER) != epoch)
          return false;

        cursor = Reclaimer::global_thread_block_list.next(cursor);
        if (cursor == nullptr)
          return true;
      }
      return false;
    }
  };
};
using scan_one_thread = scan_n_threads<1>;

struct abandon_never {
  using retire_list = detail::retire_list<>;
  static void apply(retire_list&, detail::orphan_list<>&) {}
};

struct abandon_always {
  using retire_list = detail::retire_list<>;
  static void apply(retire_list& list, detail::orphan_list<>& orphans) {
    if (!list.empty()) {
      orphans.add(list.steal());
    }
  }
};

template <std::size_t Threshold>
struct abandon_when_exceeds_threshold {
  using retire_list = detail::counting_retire_list<>;
  static void apply(retire_list& list, detail::orphan_list<>& orphans) {
    if (list.size() >= Threshold) {
      orphans.add(list.steal());
    }
  }
};

template <class T, class Reclaimer>
class guard_ptr {
private:
  T* ptr_ = nullptr;

public:
  guard_ptr() noexcept = default;

  explicit guard_ptr(T* p) noexcept : ptr_(p) {
    if (ptr_ != nullptr) Reclaimer::local_thread_data.enter_critical();
  }
  void reset() noexcept {
    if (ptr_ != nullptr) Reclaimer::local_thread_data.leave_critical();
    ptr_ = nullptr;
  }

  void acquire(const std::atomic<T*>& p, std::memory_order order = std::memory_order_seq_cst) noexcept {
    if (p.load(std::memory_order_relaxed) == nullptr) {
      reset();
      return;
    }

    if (ptr_ == nullptr)  Reclaimer::local_thread_data.enter_critical();
    ptr_ = p.load(order);
    if (ptr_ == nullptr) Reclaimer::local_thread_data.leave_critical();
  }
  bool acquire_if_equal(const std::atomic<T*>& p, const T* expected, std::memory_order order = std::memory_order_seq_cst) noexcept {
    T* actual = p.load(std::memory_order_relaxed);
    if (actual == nullptr || actual != expected) {
      reset();
      return actual == expected;
    }
    if (ptr_ == nullptr) Reclaimer::local_thread_data.enter_critical();
    ptr_ = p.load(order);
    if (ptr_ == nullptr || ptr_ != expected) {
      Reclaimer::local_thread_data.leave_critical();
      ptr_ = nullptr;
    }
    return ptr_ == expected;
  }

  void reclaim() noexcept {
    assert(ptr_ != nullptr);
    Reclaimer::local_thread_data.add_retired_node(ptr_);
    reset();
  }

public:

  guard_ptr(const guard_ptr& p) noexcept : guard_ptr(p.ptr_) {}
  guard_ptr(guard_ptr&& p) noexcept : ptr_(p.ptr_) { p.ptr_ = nullptr; }

  ~guard_ptr() { reset(); }

  guard_ptr& operator=(const guard_ptr& p) noexcept {
    if (&p == this) return *this;
    reset();
    ptr_ = p.ptr_;
    if (ptr_ != nullptr) Reclaimer::local_thread_data.enter_critical();
    return *this;
  }

  guard_ptr& operator=(guard_ptr&& p) noexcept {
    if (&p == this) return *this;
    reset();
    ptr_ = p.ptr_;
    p.ptr_ = nullptr;
    return *this;
  }
  [[nodiscard]] T* get() const noexcept { return ptr_; }
  T* operator->() const noexcept { return ptr_; }
  T& operator*() const noexcept { return *ptr_; }
  explicit operator bool() const noexcept { return ptr_ != nullptr; }
  void swap(guard_ptr& g) noexcept { std::swap(ptr_, g.ptr_); }
};

template <std::size_t ScanFrequency = 100,
          class ScanStrategy = scan_all_threads,
          class AbandonStrategy = abandon_never,
          region_extension RegionExtension = region_extension::eager>
class generic_epoch_based {
public:
  using epoch_t = std::size_t;
  static constexpr std::size_t number_epochs = 3;
  static constexpr std::size_t scan_frequency = ScanFrequency;
  static constexpr region_extension region_extension_type = RegionExtension;
  using abandon_strategy = AbandonStrategy;
  template <class T>
  using enable_concurrent_ptr = ebr::enable_concurrent_ptr<T>;

  struct region_guard {
    region_guard() noexcept;
    ~region_guard() noexcept;
    region_guard(const region_guard&) = delete;
    region_guard(region_guard&&) = delete;
    region_guard& operator=(const region_guard&) = delete;
    region_guard& operator=(region_guard&&) = delete;
  };

  struct thread_control_block;
  struct thread_data;

  inline static detail::thread_block_list<thread_control_block> global_thread_block_list;

private:
  template <class, class>
  friend class guard_ptr;

  inline static std::atomic<epoch_t> global_epoch{0};
  inline static std::array<detail::orphan_list<>, number_epochs> orphans{};
  inline static thread_local thread_data local_thread_data;
};

using epoch_based = generic_epoch_based<100, scan_all_threads, abandon_never, region_extension::none>;

using new_epoch_based = generic_epoch_based<100, scan_all_threads, abandon_never, region_extension::eager>;

using debra = generic_epoch_based<20, scan_one_thread, abandon_never, region_extension::none>;

template <std::size_t SF, class SS, class AS, region_extension RE>
struct generic_epoch_based<SF, SS, AS, RE>::thread_control_block : detail::thread_block_list<thread_control_block>::entry {
  std::atomic<bool> is_in_critical_region;
  std::atomic<epoch_t> local_epoch;

  thread_control_block() : is_in_critical_region(false), local_epoch(number_epochs) {}
};

template <std::size_t SF, class SS, class AS, region_extension RE>
struct generic_epoch_based<SF, SS, AS, RE>::thread_data {
public:
  unsigned critical_entries_since_update = 0;

  unsigned nested_critical_entries = 0;

  unsigned region_entries = 0;

  typename SS::template type<generic_epoch_based> scan_strategy;
  thread_control_block* control_block = nullptr;
  std::array<typename AS::retire_list, number_epochs> retire_lists = {};
  std::array<detail::retire_list<>, number_epochs> orphan_lists = {};

  epoch_t local_epoch_idx = 0;

  ~thread_data() {
    if (control_block == nullptr)
      return;
    for (std::size_t i = 0; i < number_epochs; ++i) {
      if (!retire_lists[i].empty()) orphans[i].add(retire_lists[i].steal());

      if (!orphan_lists[i].empty()) orphans[i].add(orphan_lists[i].steal());
    }
    assert(control_block->is_in_critical_region.load(std::memory_order_relaxed) == false);
    global_thread_block_list.release_entry(control_block);
    control_block = nullptr;
  }

private:
  void ensure_has_control_block() {
    if (control_block == nullptr) acquire_control_block();
  }

  void acquire_control_block() {
    assert(control_block == nullptr);
    control_block = global_thread_block_list.acquire_entry();
    assert(control_block->is_in_critical_region.load() == false);
    const epoch_t epoch = global_epoch.load(std::memory_order_relaxed);
    control_block->local_epoch.store(epoch, std::memory_order_release);
    local_epoch_idx = epoch % number_epochs;
    scan_strategy.reset();
  }

  void set_critical_region_flag() {
    assert(!control_block->is_in_critical_region.load(std::memory_order_relaxed));
    control_block->is_in_critical_region.store(true, std::memory_order_relaxed);
    EBR_FENCE(std::memory_order_seq_cst);
  }

  void clear_critical_region_flag() {
    assert(control_block != nullptr);
    assert(control_block->is_in_critical_region.load(std::memory_order_relaxed));
    control_block->is_in_critical_region.store(false, std::memory_order_release);
    for (std::size_t i = 0; i < number_epochs; ++i)
      AS::apply(retire_lists[i], orphans[i]);
  }

public:

  void add_retired_node(detail::deletable_object* p) {
    retire_lists[local_epoch_idx].push(p);
  }

  void reclaim_orphans(epoch_t epoch) {
    const auto idx = epoch % number_epochs;
    auto* nodes = orphans[idx].adopt();
    while (nodes != nullptr) {
      auto* next = nodes->next;
      orphan_lists[idx].push(nodes);
      nodes = next;
    }
  }

  void enter_region() {
    ensure_has_control_block();
    if constexpr (RE != region_extension::none) {
      if (++region_entries == 1) {
        if constexpr (RE == region_extension::eager) {
          set_critical_region_flag();
        }
      }
    }
  }
  void leave_region() {
    if constexpr (RE != region_extension::none) {
      if (--region_entries == 0 && control_block->is_in_critical_region.load(std::memory_order_relaxed))
        clear_critical_region_flag();
    }
  }

  void enter_critical() {
    enter_region();
    if (++nested_critical_entries == 1)
      do_enter_critical();
  }
  void leave_critical() {
    if (--nested_critical_entries == 0 && RE == region_extension::none)
      clear_critical_region_flag();
    leave_region();
  }

  void do_enter_critical() {
    if constexpr (RE == region_extension::none) {
      set_critical_region_flag();
    } else if constexpr (RE == region_extension::lazy) {
      if (!control_block->is_in_critical_region.load(std::memory_order_relaxed))
        set_critical_region_flag();
    }
    assert(control_block->is_in_critical_region.load(std::memory_order_relaxed));
    epoch_t epoch = global_epoch.load(std::memory_order_acquire);
    if (control_block->local_epoch.load(std::memory_order_relaxed) != epoch) {
      critical_entries_since_update = 0;
      update_local_epoch(epoch);
    }
    else if (critical_entries_since_update++ == SF) {

      EBR_FENCE(std::memory_order_seq_cst);
      critical_entries_since_update = 0;
      if (scan_strategy.scan(epoch)) {
        EBR_FENCE(std::memory_order_acquire);
        bool success = false;
        epoch = update_global_epoch(epoch, epoch + 1, success);
        update_local_epoch(epoch);
        if (success) reclaim_orphans(epoch);

      }
    }
  }

  epoch_t update_global_epoch(epoch_t curr_epoch, epoch_t new_epoch, bool& success) {
    if (global_epoch.load(std::memory_order_relaxed) == curr_epoch) {
      success = global_epoch.compare_exchange_strong(curr_epoch, new_epoch, std::memory_order_release, std::memory_order_relaxed);
    }

    return new_epoch;

  }

  void update_local_epoch(epoch_t new_epoch) {
    const epoch_t old_epoch = control_block->local_epoch.load(std::memory_order_relaxed);
    assert(new_epoch > old_epoch);
    control_block->local_epoch.store(new_epoch, std::memory_order_release);
    const int diff = std::min<int>(static_cast<int>(number_epochs), static_cast<int>(new_epoch - old_epoch));
    for (int i = 0; i < diff; ++i) {
      const epoch_t idx = (new_epoch - static_cast<epoch_t>(i)) % number_epochs;
      detail::delete_objects(retire_lists[idx].steal().first);
      detail::delete_objects(orphan_lists[idx].steal().first);
    }
    local_epoch_idx = new_epoch % number_epochs;
    scan_strategy.reset();
  }
};

template <std::size_t SF, class SS, class AS, region_extension RE>
generic_epoch_based<SF, SS, AS, RE>::region_guard::region_guard() noexcept {
  local_thread_data.enter_region();
}

template <std::size_t SF, class SS, class AS, region_extension RE>
generic_epoch_based<SF, SS, AS, RE>::region_guard::~region_guard() noexcept {
  local_thread_data.leave_region();
}

#undef EBR_TSAN
#undef EBR_LOAD_ORDER

#undef EBR_FENCE

}
