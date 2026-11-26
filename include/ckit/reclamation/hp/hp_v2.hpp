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

class bad_hazard_pointer_alloc : public std::runtime_error {
  using std::runtime_error::runtime_error;
};

template<std::size_t K_, std::size_t A_ = 2, std::size_t B_ = 100, bool is_dynamic_ = false>
struct strategy{
  inline static constexpr std::size_t K = K_;
  inline static constexpr bool is_dynamic = is_dynamic_;
  inline static std::atomic<std::size_t> active_hps{0};

  static std::size_t active_hp_count() {
    return active_hps.load(std::memory_order_relaxed);
  }
  static std::size_t scan_threshold() {
    return A_ * active_hp_count() + B_;
  }
};

namespace detail {

struct deletable_object {
  virtual void delete_self() = 0;
  deletable_object* next = nullptr;
protected:
  virtual ~deletable_object() = default;
};

class slot{
  static constexpr std::uintptr_t tag = 1;
  std::atomic<std::uintptr_t> hp{0};
public:

  void set_hp(deletable_object* ptr){
    hp.store(reinterpret_cast<std::uintptr_t>(ptr), std::memory_order_release);
    HP_FENCE(std::memory_order_seq_cst);
  }

  void set_link(slot* link){
    hp.store(reinterpret_cast<std::uintptr_t>(link) | tag,std::memory_order_release);
  }

  bool try_get_obj_ptr(deletable_object* &out) const{
    const std::uintptr_t v = hp.load(HP_SLOT_LOAD_ORDER);
    if(v & tag) return false;
    out = reinterpret_cast<deletable_object*>(v);
    return true;
  }

  bool is_link() const {
    return (hp.load(std::memory_order_relaxed) & tag) != 0;
  }

  slot* get_link() const {
    assert(is_link());
    return reinterpret_cast<slot*>(hp.load(std::memory_order_relaxed) & ~tag);
  }

};

inline void link_free_list(slot* first, slot* last, slot* tail) {
  for (slot* it = first; it != last; ++it) it->set_link(it + 1);
  last->set_link(tail);
}

template<class Strategy>
struct Array_of_slot;

template<std::size_t K_, std::size_t A_, std::size_t B_>
struct alignas(64) Array_of_slot<strategy<K_ ,A_, B_, false>> {
private:
  using slot = detail::slot;
  using Strategy = strategy<K_ ,A_, B_, false>;
  static constexpr std::size_t K = Strategy::K;
  slot arr_of_slots[K]{};
  slot* hint = nullptr;

  slot* begin() { return &arr_of_slots[0]; }
  slot* end() { return &arr_of_slots[K]; }
  const slot* begin() const { return &arr_of_slots[0]; }
  const slot* end() const { return &arr_of_slots[K]; }
public:
  void initialize() {
    Strategy::active_hps.fetch_add(K, std::memory_order_relaxed);
    link_free_list(begin(), end() - 1, nullptr);
    hint = begin();
  }

  void abandon() {
    Strategy::active_hps.fetch_sub(K, std::memory_order_relaxed);
  }

  void gather(std::vector<const deletable_object*>& out) const {
    for(const slot* it = begin(); it != end(); ++it){
      deletable_object* obj_ptr = nullptr;
      if(it->try_get_obj_ptr(obj_ptr)){
        out.push_back(obj_ptr);
      }
    }
  }

  slot* alloc() {
    slot* result = hint;
    if(result == nullptr) {
      throw bad_hazard_pointer_alloc("hazard pointer pool exceeded");
    }
    hint = result->get_link();
    return result;
  }

  void release(slot* &hp) {
    if( hp == nullptr) return;
    hp->set_link(hint);
    hint = hp;
    hp = nullptr;
  }

};

template<std::size_t K_, std::size_t A_, std::size_t B_>
struct alignas(64) Array_of_slot<strategy<K_ ,A_, B_, true>> {
private:
  using slot = detail::slot;
  using Strategy = strategy<K_ ,A_, B_, true>;
  struct block;

  static constexpr std::size_t K = Strategy::K;
  slot arr_of_slots[K]{};
  slot* hint = nullptr;

  std::size_t total_nums_of_slots = K;
  std::atomic<block*> block_ptr{nullptr};

  slot* begin() { return &arr_of_slots[0]; }
  slot* end() { return &arr_of_slots[K]; }
  const slot* begin() const { return &arr_of_slots[0]; }
  const slot* end() const { return &arr_of_slots[K]; }
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

    explicit block(std::size_t n): size(n) {
      for(std::size_t i = 0; i < size; ++i){
        ::new (slot_at(i)) slot();
      }
    }
  };
  slot* initialize_block(){
    block* b = block_ptr.load(std::memory_order_relaxed);
    if(b == nullptr) return nullptr;
    slot* result = b->begin();
    while (b) {
      link_free_list(b->begin(), b->end() - 1, b->next ? b->next->begin() : nullptr);
      b = b->next;
    }
    return result;
  }

  void gather_block(std::vector<const deletable_object*>& out) const {
    block* b = block_ptr.load(std::memory_order_relaxed);
    if(b == nullptr) return;
    while (b) {
      for(slot* it = b->begin(); it != b->end(); ++it) {
        deletable_object* obj_ptr = nullptr;
        if(it->try_get_obj_ptr(obj_ptr)){
          out.push_back(obj_ptr);
        }
      }
      b = b->next;
    }
  }

  slot* alloc_block(){
    const std::size_t hps = std::max(static_cast<std::size_t>(K_), total_nums_of_slots/2);
    total_nums_of_slots += hps;
    Strategy::active_hps.fetch_add(hps, std::memory_order_relaxed);

    void* raw = ::operator new(sizeof(block) + hps * sizeof(slot), std::align_val_t{alignof(block)});
    block* b = ::new (raw) block(hps);
    link_free_list(b->begin(), b->end() - 1, nullptr);
    b->next = block_ptr.load(std::memory_order_relaxed);
    block_ptr.store(b, std::memory_order_release);
    return b->begin();
  }
public:
  void initialize() {
    Strategy::active_hps.fetch_add(total_nums_of_slots, std::memory_order_relaxed);
    link_free_list(begin(), end() - 1, initialize_block());
    hint = begin();
  }

  void abandon() {
    Strategy::active_hps.fetch_sub(total_nums_of_slots, std::memory_order_relaxed);
  }

  void gather(std::vector<const deletable_object*>& out) const {
    for(const slot* it = begin(); it != end(); ++it){
      deletable_object* obj_ptr = nullptr;
      if(it->try_get_obj_ptr(obj_ptr)){
        out.push_back(obj_ptr);
      }
    }
    gather_block(out);
  }

  slot* alloc() {
    slot* result = hint;
    if(result == nullptr) {
      result = alloc_block();
    }
    hint = result->get_link();
    return result;
  }

  void release(slot* &hp) {
    if( hp == nullptr) return;
    hp->set_link(hint);
    hint = hp;
    hp = nullptr;
  }
};

enum class TCB_state { free, inactive, active };

template<class Strategy>
struct TCB;

template <class Strategy>
class thread_block_list;

template<std::size_t K_, std::size_t A_, std::size_t B_, bool is_dynamic_>
struct TCB<strategy<K_, A_, B_, is_dynamic_>> {
private:
  friend class thread_block_list<strategy<K_, A_, B_, is_dynamic_>>;
  TCB<strategy<K_, A_, B_, is_dynamic_>>* next_TCB{nullptr};
  Array_of_slot<strategy<K_, A_, B_, is_dynamic_>> arr;
  std::atomic<TCB_state> state;

public:
  TCB() : arr{}, state(TCB_state::active) {}

  bool is_active(std::memory_order order = std::memory_order_relaxed) const {
    return state.load(order) == TCB_state::active;
  }

  bool try_adopt(TCB_state initial_state) {
    if(state.load(std::memory_order_relaxed) == TCB_state::free){
      auto expected = TCB_state::free;
      return state.compare_exchange_strong(expected, initial_state, std::memory_order_acquire);
    }
    return false;
  }

  void activate() {
    assert(state.load(std::memory_order_relaxed) == TCB_state::inactive);
    state.store(TCB_state::active, std::memory_order_release);
  }

  void initialize() {
    arr.initialize();
  }
  void abandon() {
    assert(is_active());
    arr.abandon();
    state.store(TCB_state::free, std::memory_order_release);
  }
  void gather(std::vector<const deletable_object*>& out) const {
    arr.gather(out);
  }
  slot* alloc() {
    return arr.alloc();
  }
  void release(slot* &hp) {
    arr.release(hp);
  }
};

template <class Strategy>
class thread_block_list;

template<std::size_t K_, std::size_t A_, std::size_t B_, bool is_dynamic_>
class thread_block_list<strategy<K_, A_, B_, is_dynamic_>> {
  using TCB_TYPE = TCB<strategy<K_, A_, B_, is_dynamic_>>;
  std::atomic<TCB_TYPE*> head{nullptr};
  alignas(64) std::atomic<deletable_object*> abandoned_retired{nullptr};

private:
  void add_TCB(TCB_TYPE* v) {
    TCB_TYPE* h = head.load(std::memory_order_relaxed);
    do {
      v->next_TCB = h;
    } while (!head.compare_exchange_weak(h, v, std::memory_order_release, std::memory_order_relaxed));

  }

  TCB_TYPE* adopt_or_create_TCB(TCB_state initial_state) {
    TCB_TYPE* result = head.load(std::memory_order_acquire);
    while(result != nullptr){
      if(result->try_adopt(initial_state))
        return result;
      result = result->next_TCB;
    }
    result = new TCB_TYPE();
    result->state.store(initial_state, std::memory_order_relaxed);
    add_TCB(result);
    return result;
  }

public:
  TCB_TYPE* acquire_TCB(){
    return adopt_or_create_TCB(TCB_state::active);
  }

  TCB_TYPE* acquire_inactive_TCB(){
    return adopt_or_create_TCB(TCB_state::inactive);
  }
  void release_TCB(TCB_TYPE* v) {
    v->abandon();
  }
  template<class Func>
  void for_each_TCB(Func&& fn) {
    for(TCB_TYPE* e = head.load(std::memory_order_acquire); e != nullptr; e = e->next_TCB) {
      std::forward<Func>(fn)(*e);
    }
  }
  void abandon_retired_nodes(deletable_object* list) {
    deletable_object* last = list;
    while(last->next != nullptr) last = last->next;
    deletable_object* h = abandoned_retired.load(std::memory_order_relaxed);
    do {
      last->next = h;
    } while(!abandoned_retired.compare_exchange_weak(h, list, std::memory_order_release, std::memory_order_relaxed));

  }
  deletable_object* adopt_abandoned_retired_nodes() {
    if(abandoned_retired.load(std::memory_order_relaxed) == nullptr)
      return nullptr;
    return abandoned_retired.exchange(nullptr, std::memory_order_acquire);

  }
};

}

template <class T, class Reclaimer>
class guard_ptr;

template<class Strategy>
class hazard_pointer;

template<std::size_t K_, std::size_t A_, std::size_t B_, bool is_dynamic_>
class hazard_pointer<strategy<K_, A_, B_, is_dynamic_>> {
public:
  using Strategy = strategy<K_, A_, B_, is_dynamic_>;
  using TCB_TYPE = detail::TCB<Strategy>;
  using TCB_list = detail::thread_block_list<Strategy>;
  using slot = detail::slot;
  struct thread_data;
private:
  template <class, class>
  friend class guard_ptr;
  inline static TCB_list global_TCB_list;
  inline static thread_local thread_data local;
public:
  struct thread_data {
  private:
    detail::deletable_object* retire_list = nullptr;
    std::size_t length = 0;
    TCB_TYPE* tcb_ptr = nullptr;

    void ensure_has_control_block() {
      if(tcb_ptr == nullptr) {
        tcb_ptr = global_TCB_list.acquire_TCB();
        tcb_ptr->initialize();
      }
    }
    void reclaim_nodes(detail::deletable_object* list,
      const std::vector<const detail::deletable_object*>& vec) {
        while (list != nullptr) {
          auto* cur = list;
          list = list->next;
          if(std::binary_search(vec.begin(), vec.end(), cur)){
            add_retired_node(cur);
          }
          else {
            cur->delete_self();
          }
        }
      }
  public:
    slot* alloc() {
      ensure_has_control_block();
      return tcb_ptr->alloc();
    }
    void release(slot* &hp) {
      if(hp == nullptr)
        return;
      tcb_ptr->release(hp);
    }
    std::size_t add_retired_node(detail::deletable_object* p) {
      p->next = retire_list;
      retire_list = p;
      return ++length;
    }
    void scan() {
      std::vector<const detail::deletable_object*> vec;
      vec.reserve(Strategy::active_hp_count());

      HP_FENCE(std::memory_order_seq_cst);

      global_TCB_list.for_each_TCB(
        [&vec](const auto & e) {
          if(e.is_active(HP_SLOT_LOAD_ORDER))  e.gather(vec);
        }
      );

      HP_FENCE(std::memory_order_acquire);

      std::sort(vec.begin(), vec.end());

      auto * adopted_list = global_TCB_list.adopt_abandoned_retired_nodes();
      auto* list = retire_list;
      retire_list = nullptr;
      length = 0;
      reclaim_nodes(list, vec);
      reclaim_nodes(adopted_list, vec);
    }
    ~thread_data() {
      if(retire_list != nullptr) {
        scan();
        if(retire_list != nullptr) global_TCB_list.abandon_retired_nodes(retire_list);
        retire_list = nullptr;
      }
      if(tcb_ptr != nullptr){
        global_TCB_list.release_TCB(tcb_ptr);
        tcb_ptr = nullptr;
      }
    }
  };
};

template <class T>
class deletable_obj_with_new_delete : public detail::deletable_object {
public:

  static constexpr std::size_t number_of_mark_bits = 0;
protected:
  deletable_obj_with_new_delete() noexcept = default;
  deletable_obj_with_new_delete(const deletable_obj_with_new_delete&) noexcept = default;
  deletable_obj_with_new_delete(deletable_obj_with_new_delete&&) noexcept = default;
  deletable_obj_with_new_delete& operator=(const deletable_obj_with_new_delete&) noexcept = default;
  deletable_obj_with_new_delete& operator=(deletable_obj_with_new_delete&&) noexcept = default;
  ~deletable_obj_with_new_delete() override = default;
private:
  void delete_self() override { delete static_cast<T*>(this); }
};

template <class T, class Reclaimer = hazard_pointer<strategy<3, 2, 100, false>>>
class guard_ptr {
  using slot = typename Reclaimer::slot;
private:
  T* ptr = nullptr;
  slot* slot_ptr = nullptr;
public:
  guard_ptr() noexcept = default;

  explicit guard_ptr(T* p) : ptr(p) {
    if (ptr == nullptr) return;
    slot_ptr = Reclaimer::local.alloc();
    slot_ptr->set_hp(ptr);

  }

  guard_ptr(const guard_ptr& p) : guard_ptr(p.ptr) {}
  guard_ptr(guard_ptr&& p) noexcept :ptr(p.ptr), slot_ptr(p.slot_ptr) {
    p.ptr = nullptr;
    p.slot_ptr = nullptr;
  }
  guard_ptr& operator=(const guard_ptr& p) {
    if(p.ptr == nullptr) {
      reset();
      return *this;
    }
    if(&p == this) return *this;
    if(slot_ptr == nullptr){
      slot_ptr = Reclaimer::local.alloc();
    }
    ptr = p.ptr;
    slot_ptr->set_hp(ptr);
    return *this;
  }
  guard_ptr& operator=(guard_ptr&& p) noexcept {
    if(p.ptr == nullptr) {
      reset();
      return *this;
    }
    if(&p == this) return *this;
    reset();
    ptr = p.ptr;
    slot_ptr = p.slot_ptr;
    p.ptr = nullptr;
    p.slot_ptr = nullptr;
    return *this;
  }

  ~guard_ptr() { reset(); }

  void reset() noexcept {
    if(slot_ptr != nullptr) {
      Reclaimer::local.release(slot_ptr);
    }
    ptr = nullptr;
  }

  void acquire(const std::atomic<T*>& p) {
    T* p1 = p.load(std::memory_order_relaxed);
    if(p1 == ptr) return;
    if(p1 == nullptr) {
      reset();
      return;
    }
    if(slot_ptr == nullptr) slot_ptr = Reclaimer::local.alloc();
    T* p2 = p1;
    do {
      if(p2 == nullptr) {
        reset();
        return;
      }
      p1 = p2;
      slot_ptr->set_hp(p1);
      p2 = p.load(std::memory_order_acquire);
    } while (p1 != p2);
    ptr = p2;
  }

  bool acquire_if_equal(const std::atomic<T*>& p,const T* expected) {
    T* p1 = p.load(std::memory_order_relaxed);
    if (p1 == nullptr || p1 != expected) {
      reset();
      return p1 == expected;
    }
    if(slot_ptr == nullptr) slot_ptr = Reclaimer::local.alloc();
    slot_ptr->set_hp(p1);
    T* p2 = p.load(std::memory_order_acquire);
    if(p1 != p2) {
      reset();
      return false;
    }
    ptr = p2;
    return true;
  }
  void reclaim() {
    assert(ptr != nullptr);
    auto* obj = static_cast<detail::deletable_object*>(ptr);
    reset();
    if(Reclaimer::local.add_retired_node(obj) >= Reclaimer::Strategy::scan_threshold()) {
      Reclaimer::local.scan();
    }
  }
  T* get() const noexcept { return ptr; }
  T* operator->() const noexcept { return ptr; }
  T& operator*() const noexcept { return *ptr; }
  explicit operator bool() const noexcept { return ptr != nullptr; }
  void swap(guard_ptr& g) noexcept {
    std::swap(ptr, g.ptr);
    std::swap(slot_ptr, g.slot_ptr);
  }
};

#undef HP_TSAN
#undef HP_SLOT_LOAD_ORDER
#undef HP_FENCE

}
