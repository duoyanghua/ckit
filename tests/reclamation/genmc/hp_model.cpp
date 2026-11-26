// SPDX-License-Identifier: MIT
// GenMC model of the hazard-pointer publish/validate/scan protocol.
// Run: genmc -- hp_model.cpp

#include <atomic>
#include <cassert>
#include <pthread.h>

namespace {

constexpr int kThreads = 3;

struct Node {
  Node* next;
  int value;
  std::atomic<bool> freed;
  Node(Node* n, int v, bool f) : next(n), value(v), freed(f) {}
};

Node n0{nullptr, 42, false};

std::atomic<Node*> src{&n0};
std::atomic<Node*> hp[kThreads];
std::atomic<bool> holds[kThreads];

void free_node(Node* n) {
  assert(n != nullptr);
  assert(!n->freed.load(std::memory_order_acquire));

  for (int t = 0; t < kThreads; ++t) {
    assert(!holds[t].load(std::memory_order_acquire));
  }

  n->freed.store(true, std::memory_order_release);
}

void reader_body(int t) {
  Node* p = src.load(std::memory_order_relaxed);
  if (p == nullptr) {
    return;
  }

  hp[t].store(p, std::memory_order_release);

#ifndef BUG_NO_READER_FENCE
  std::atomic_thread_fence(std::memory_order_seq_cst);
#endif

#ifndef BUG_NO_VALIDATE

  Node* p2 = src.load(std::memory_order_seq_cst);
  if (p2 != p) {
    hp[t].store(nullptr, std::memory_order_release);
    return;
  }
#endif

  holds[t].store(true, std::memory_order_release);
  assert(!p->freed.load(std::memory_order_acquire));
  int v = p->value;
  (void)v;
  holds[t].store(false, std::memory_order_release);
  hp[t].store(nullptr, std::memory_order_release);
}

void* reader_b(void*) {
  reader_body(1);
  return nullptr;
}

void* reader_c(void*) {
  reader_body(2);
  return nullptr;
}

void* writer(void*) {
  Node* old = src.load(std::memory_order_relaxed);
  if (old == nullptr) {
    return nullptr;
  }

  src.store(old->next, std::memory_order_release);

#ifndef BUG_NO_WRITER_FENCE
  std::atomic_thread_fence(std::memory_order_seq_cst);
#endif

#ifndef BUG_NO_SCAN

  for (int t = 0; t < kThreads; ++t) {
    if (hp[t].load(std::memory_order_acquire) == old) {
      return nullptr;
    }
  }
#endif

  free_node(old);
  return nullptr;
}

}

int main() {
  for (int t = 0; t < kThreads; ++t) {
    hp[t].store(nullptr, std::memory_order_relaxed);
    holds[t].store(false, std::memory_order_relaxed);
  }

  pthread_t a;
  pthread_t b;
  pthread_t c;
  pthread_create(&a, nullptr, writer, nullptr);
  pthread_create(&b, nullptr, reader_b, nullptr);
  pthread_create(&c, nullptr, reader_c, nullptr);
  pthread_join(a, nullptr);
  pthread_join(b, nullptr);
  pthread_join(c, nullptr);
  return 0;
}
