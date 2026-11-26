// SPDX-License-Identifier: LGPL-2.1-or-later -- adapted from liburcu.
#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>

struct rcu_head {
  std::atomic<rcu_head*> next{nullptr};
  void (*func)(rcu_head*) = nullptr;
};

namespace liburcu_qsbr {
namespace detail {

struct reader {
  std::atomic<unsigned long> ctr{0};
  reader* prev = nullptr;
  reader* next = nullptr;
  bool registered = false;
};

inline thread_local reader tls_reader;

class wfcq {
private:
  std::atomic<rcu_head*> head_;
  rcu_head* tail_;
  rcu_head stub_;
public:
  wfcq() : head_(&stub_), tail_(&stub_) {}

  bool empty() const {
    return head_.load(std::memory_order_relaxed) == tail_;
  }

  void push(rcu_head* n) {
    n->next.store(nullptr, std::memory_order_relaxed);
    rcu_head* prev = head_.exchange(n, std::memory_order_acq_rel);
    prev->next.store(n, std::memory_order_release);
  }

  rcu_head* pop() {
    rcu_head* t = tail_;
    rcu_head* next = t->next.load(std::memory_order_acquire);
    if (next == nullptr) {
      if (head_.load(std::memory_order_relaxed) == t)  return nullptr;
      do {
        std::this_thread::yield();
        next = t->next.load(std::memory_order_acquire);
      } while (next == nullptr);
    }
    tail_ = next;
    return t;
  }

  rcu_head* drain() {
    rcu_head* first = nullptr;
    rcu_head* prev = nullptr;
    unsigned count = 0;
    for (;;) {
      rcu_head* n = pop();
      if (n == &stub_) continue;
      if (n == nullptr) {
        rcu_head* t = tail_;
        if (t == &stub_)
          break;
        stub_.next.store(nullptr, std::memory_order_relaxed);
        rcu_head* expected = t;
        if (head_.compare_exchange_strong(expected, &stub_, std::memory_order_acq_rel)) {
          tail_ = &stub_;
          if (prev != nullptr)   prev->next.store(t, std::memory_order_relaxed);
          else                   first = t;
          break;
        }
        continue;
      }
      if (prev != nullptr)   prev->next.store(n, std::memory_order_relaxed);
      else                   first = n;
      prev = n;
      if (++count >= 1024) {
        prev->next.store(nullptr, std::memory_order_relaxed);
        break;
      }
    }
    return first;
  }

  std::atomic<rcu_head*>& head() { return head_;  }
  rcu_head* tail() const { return tail_; }
  void set_tail(rcu_head* t) { tail_ = t; }
  rcu_head* stub() { return &stub_; }
};

}

class domain {
private:

  std::mutex registry_mutex_;
  detail::reader* registry_head_ = nullptr;

  std::atomic<unsigned long> gp_ctr_{1};

  std::mutex gp_mutex_;
  std::atomic<unsigned> writers_waiting_{0};
  std::atomic<std::uint32_t> qs_seq_{0};

  detail::wfcq queue_;
  std::atomic<std::uint32_t> queue_seq_{0};
  std::thread worker_;
  std::atomic<bool> stopping_{false};

  std::mutex barrier_mutex_;
  std::atomic<std::uint32_t> barrier_seq_{0};

private:

  domain() { worker_ = std::thread(&domain::worker_loop, this); }

  bool all_readers_current(unsigned long gp);
  void notify_quiescent();
  void worker_loop();
  static void barrier_callback(rcu_head* head);
  void go_online_unlocked();

public:
  static domain& get() {
    static domain instance;
    return instance;
  }

  domain(const domain&) = delete;
  domain& operator=(const domain&) = delete;
  ~domain();

  void register_thread();
  void unregister_thread();

  void thread_online();
  void thread_offline();
  void quiescent_state();

  void synchronize_rcu();

  void call_rcu(rcu_head* head, void (*func)(rcu_head*));
  void barrier();
};

inline void domain::go_online_unlocked() {
  const unsigned long gp = gp_ctr_.load(std::memory_order_acquire);
  detail::tls_reader.ctr.store(gp, std::memory_order_relaxed);
}

inline void domain::register_thread() {
  assert(!detail::tls_reader.registered);
  assert(detail::tls_reader.ctr.load(std::memory_order_relaxed) == 0);
  detail::tls_reader.registered = true;
  go_online_unlocked();
  std::lock_guard<std::mutex> lock(registry_mutex_);
  detail::tls_reader.next = registry_head_;
  detail::tls_reader.prev = nullptr;
  if (registry_head_ != nullptr) registry_head_->prev = &detail::tls_reader;
  registry_head_ = &detail::tls_reader;
}

inline void domain::thread_online() {
  assert(detail::tls_reader.registered);
  go_online_unlocked();
  std::atomic_thread_fence(std::memory_order_seq_cst);

}

inline void domain::thread_offline() {
  assert(detail::tls_reader.registered);
  detail::tls_reader.ctr.store(0, std::memory_order_release);
  notify_quiescent();
}

inline void domain::unregister_thread() {
  assert(detail::tls_reader.registered);
  thread_offline();
  std::lock_guard<std::mutex> lock(registry_mutex_);
  if (detail::tls_reader.prev != nullptr)  detail::tls_reader.prev->next = detail::tls_reader.next;
  else                                     registry_head_ = detail::tls_reader.next;
  if (detail::tls_reader.next != nullptr)  detail::tls_reader.next->prev = detail::tls_reader.prev;
  detail::tls_reader.prev = nullptr;
  detail::tls_reader.next = nullptr;
  detail::tls_reader.registered = false;
}

inline void domain::quiescent_state() {
  assert(detail::tls_reader.registered);

  const unsigned long gp = gp_ctr_.load(std::memory_order_relaxed);
  if (gp == detail::tls_reader.ctr.load(std::memory_order_relaxed)) {
    return;
  }
  detail::tls_reader.ctr.store(gp, std::memory_order_release);
  notify_quiescent();
}

inline void domain::notify_quiescent() {
  std::atomic_thread_fence(std::memory_order_seq_cst);
  if (writers_waiting_.load(std::memory_order_relaxed) == 0) {
    return;
  }
  qs_seq_.fetch_add(1, std::memory_order_release);
  qs_seq_.notify_all();
}

inline bool domain::all_readers_current(unsigned long gp) {
  std::lock_guard<std::mutex> lock(registry_mutex_);
  for (detail::reader* r = registry_head_; r != nullptr; r = r->next) {
    const unsigned long ctr = r->ctr.load(std::memory_order_acquire);
    if (ctr != 0 && ctr != gp)
      return false;
  }
  return true;
}

inline void domain::synchronize_rcu() {
  const bool was_online = detail::tls_reader.ctr.load(std::memory_order_relaxed) != 0;
  if (was_online) {
    thread_offline();
  }
  std::unique_lock<std::mutex> gp_lock(gp_mutex_);
  const unsigned long gp = gp_ctr_.load(std::memory_order_relaxed) + 2;
  gp_ctr_.store(gp, std::memory_order_release);

  writers_waiting_.store(1, std::memory_order_relaxed);

  std::atomic_thread_fence(std::memory_order_seq_cst);

  while (!all_readers_current(gp)) {

    const std::uint32_t seq = qs_seq_.load(std::memory_order_acquire);
    if (all_readers_current(gp)) {
      break;
    }
    qs_seq_.wait(seq, std::memory_order_acquire);
  }
  writers_waiting_.store(0, std::memory_order_relaxed);
  gp_lock.unlock();
  if (was_online) {
    thread_online();
  }
}

inline domain::~domain() {
  stopping_.store(true, std::memory_order_relaxed);
  queue_seq_.fetch_add(1, std::memory_order_release);
  queue_seq_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
}

inline void domain::call_rcu(rcu_head* head, void (*func)(rcu_head*)) {
  assert(head != nullptr && func != nullptr);
  head->func = func;
  queue_.push(head);
  queue_seq_.fetch_add(1, std::memory_order_release);
  queue_seq_.notify_one();
}

inline void domain::worker_loop() {
  for (;;) {
    rcu_head* batch = queue_.drain();
    if (batch == nullptr) {
      if (stopping_.load(std::memory_order_relaxed)) {
        return;
      }

      const std::uint32_t seq = queue_seq_.load(std::memory_order_acquire);

      if (queue_.empty() && !stopping_.load(std::memory_order_relaxed)) {
        queue_seq_.wait(seq, std::memory_order_acquire);
      }
      continue;
    }
    synchronize_rcu();
    while (batch != nullptr) {
      rcu_head* next = batch->next.load(std::memory_order_relaxed);
      batch->func(batch);
      batch = next;
    }
  }
}

inline void domain::barrier_callback(rcu_head* ) {
  domain& d = get();
  d.barrier_seq_.fetch_add(1, std::memory_order_release);
  d.barrier_seq_.notify_all();
}

inline void domain::barrier() {
  const bool was_online = detail::tls_reader.ctr.load(std::memory_order_relaxed) != 0;
  if (was_online) {
    thread_offline();
  }
  std::unique_lock<std::mutex> lock(barrier_mutex_);
  const std::uint32_t seq = barrier_seq_.load(std::memory_order_acquire);
  rcu_head sentinel;
  call_rcu(&sentinel, &domain::barrier_callback);

  while (barrier_seq_.load(std::memory_order_acquire) == seq) {
    barrier_seq_.wait(seq, std::memory_order_acquire);
  }
  lock.unlock();
  if (was_online) {
    thread_online();
  }
}

}

inline void rcu_register_thread() {
  liburcu_qsbr::domain::get().register_thread();
}
inline void rcu_unregister_thread() {
  liburcu_qsbr::domain::get().unregister_thread();
}
inline void rcu_read_lock() {
  assert(liburcu_qsbr::detail::tls_reader.registered);
  assert(liburcu_qsbr::detail::tls_reader.ctr.load(std::memory_order_relaxed) != 0);
}
inline void rcu_read_unlock() {
  assert(liburcu_qsbr::detail::tls_reader.registered);
  assert(liburcu_qsbr::detail::tls_reader.ctr.load(std::memory_order_relaxed) != 0);
}
inline void rcu_quiescent_state() {
  liburcu_qsbr::domain::get().quiescent_state();
}
inline void rcu_thread_offline() {
  liburcu_qsbr::domain::get().thread_offline();
}
inline void rcu_thread_online() {
  liburcu_qsbr::domain::get().thread_online();
}
inline int rcu_read_ongoing() {
  return liburcu_qsbr::detail::tls_reader.ctr.load(std::memory_order_relaxed) != 0 ? 1 : 0;
}
inline void synchronize_rcu() {
  liburcu_qsbr::domain::get().synchronize_rcu();
}
inline void call_rcu(struct rcu_head* head, void (*func)(struct rcu_head*)) {
  liburcu_qsbr::domain::get().call_rcu(head, func);
}
inline void rcu_barrier() {
  liburcu_qsbr::domain::get().barrier();
}
