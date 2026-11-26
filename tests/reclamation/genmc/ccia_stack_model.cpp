// SPDX-License-Identifier: MIT
// GenMC model of a hazard-pointer Treiber stack.
// Run: genmc -- ccia_stack_model.cpp

#include <atomic>
#include <cassert>
#include <cstdint>
#include <pthread.h>

namespace {

#ifndef THREADS
  #define THREADS 2
#endif
constexpr int kThreads = THREADS;

struct Node {
  std::atomic<Node*> next;
  std::atomic<int> value;
  std::atomic<bool> freed;
  Node(Node* n, int v, bool f) : next(n), value(v), freed(f) {}
};

Node* const kFreedSentinel = reinterpret_cast<Node*>(static_cast<std::uintptr_t>(1));

Node n0{nullptr, 42, false};

std::atomic<Node*> head{&n0};
std::atomic<void*> hp[kThreads];
std::atomic<Node*> reclaim_list{nullptr};

bool outstanding(void* p) {
#ifndef BUG_SCAN_NO_FENCE
  std::atomic_thread_fence(std::memory_order_seq_cst);
#endif
  for (int i = 0; i < kThreads; ++i) {
    if (hp[i].load(std::memory_order_relaxed) == p) {
      return true;
    }
  }
#ifdef FIX_ACQUIRE_FENCE

  std::atomic_thread_fence(std::memory_order_acquire);
#endif
  return false;
}

void free_node(Node* n) {
  assert(n != nullptr);
  assert(!n->freed.load(std::memory_order_acquire));
  n->freed.store(true, std::memory_order_release);
  n->next.store(kFreedSentinel, std::memory_order_seq_cst);
}

void add_to_reclaim(Node* n) {
  Node* expected = reclaim_list.load(std::memory_order_relaxed);
  for (;;) {
    n->next.store(expected, std::memory_order_relaxed);
    if (reclaim_list.compare_exchange_weak(
          expected, n, std::memory_order_release, std::memory_order_relaxed)) {
      return;
    }
  }
}

void delete_nodes_with_no_hazards() {
  Node* current = reclaim_list.exchange(nullptr, std::memory_order_acquire);
  while (current != nullptr) {
    Node* next = current->next.load(std::memory_order_relaxed);
    if (!outstanding(current)) {
      free_node(current);
    } else {
      add_to_reclaim(current);
    }
    current = next;
  }
}

void pop_body(int t) {
  Node* old = head.load(std::memory_order_relaxed);
  for (;;) {
#ifndef BUG_NO_VALIDATE
    Node* temp;
    do {
      temp = old;
      hp[t].store(old, std::memory_order_seq_cst);
      old = head.load(std::memory_order_seq_cst);
    } while (old != temp);
#else
    hp[t].store(old, std::memory_order_seq_cst);
#endif

    if (old == nullptr) {
      hp[t].store(nullptr, std::memory_order_relaxed);
      break;
    }

    Node* next = old->next.load(std::memory_order_seq_cst);
    assert(next != kFreedSentinel);

    if (!head.compare_exchange_strong(
          old, next, std::memory_order_acquire, std::memory_order_relaxed)) {
      continue;
    }

    hp[t].store(nullptr, std::memory_order_relaxed);
    assert(!old->freed.load(std::memory_order_acquire));
    int v = old->value.load(std::memory_order_relaxed);
    (void)v;

#ifdef BUG_NO_SCAN
    free_node(old);
#else
    if (outstanding(old)) {
      add_to_reclaim(old);
    } else {
      free_node(old);
    }
#endif
    delete_nodes_with_no_hazards();
    break;
  }
}

void* pop_a(void*) {
  pop_body(0);
  return nullptr;
}

void* pop_b(void*) {
  pop_body(1);
  return nullptr;
}

#if THREADS >= 3
void* pop_c(void*) {
  pop_body(2);
  return nullptr;
}
#endif

}

int main() {
  for (int t = 0; t < kThreads; ++t) {
    hp[t].store(nullptr, std::memory_order_relaxed);
  }

  pthread_t a;
  pthread_t b;
  pthread_create(&a, nullptr, pop_a, nullptr);
  pthread_create(&b, nullptr, pop_b, nullptr);
#if THREADS >= 3
  pthread_t c;
  pthread_create(&c, nullptr, pop_c, nullptr);
#endif
  pthread_join(a, nullptr);
  pthread_join(b, nullptr);
#if THREADS >= 3
  pthread_join(c, nullptr);
#endif
  return 0;
}
