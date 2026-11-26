// SPDX-License-Identifier: MIT
// GenMC model of the xenium-style QSBR epoch protocol.
// Run: genmc -- qsbr_xenium_model.cpp

#include <atomic>
#include <cassert>
#include <pthread.h>

#ifdef MODEL_DEBUG
  #include <cstdio>
#endif

namespace {

constexpr int kThreads = 2;
constexpr unsigned kEpochs = 3;

struct Node {
  Node* next;
  unsigned retired_epoch;
  bool freed;
};

std::atomic<unsigned> global_epoch{0};
std::atomic<unsigned> local_epoch[kThreads];
std::atomic<bool> active[kThreads];
std::atomic<bool> holds[kThreads];

unsigned region_entries[kThreads];
bool has_block[kThreads];
Node* retire_lists[kThreads][kEpochs];

Node n0{nullptr, 0, false};
int freed_count = 0;

std::atomic<bool> unlinked{false};

void free_node(Node* n) {
  assert(n != nullptr);
  assert(!n->freed);
#ifdef MODEL_DEBUG
  std::fprintf(stderr,
               "free n retired=%u | t0 local=%u | t1 local=%u holds1=%d\n",
               n->retired_epoch,
               local_epoch[0].load(std::memory_order_acquire),
               local_epoch[1].load(std::memory_order_acquire),
               static_cast<int>(holds[1].load(std::memory_order_acquire)));
#endif

  for (int t = 0; t < kThreads; ++t) {
    assert(!holds[t].load(std::memory_order_acquire));
  }
  n->freed = true;
  ++freed_count;
}

void free_list(Node*& list) {
  Node* cur = list;
  while (cur != nullptr) {
    Node* next = cur->next;
    free_node(cur);
    cur = next;
  }
  list = nullptr;
}

bool try_update_epoch(unsigned curr_epoch, unsigned new_epoch) {
  const auto old_epoch = (curr_epoch + kEpochs - 1) % kEpochs;

#ifndef BUG_NO_OLD_EPOCH_CHECK
  for (int t = 0; t < kThreads; ++t) {
    if (local_epoch[t].load(std::memory_order_acquire) == old_epoch &&
        active[t].load(std::memory_order_acquire)) {
      return false;
    }
  }
#endif

  if (global_epoch.load(std::memory_order_relaxed) == curr_epoch) {
    std::atomic_thread_fence(std::memory_order_acquire);

    unsigned expected = curr_epoch;

    if (global_epoch.compare_exchange_strong(
          expected, new_epoch, std::memory_order_acq_rel, std::memory_order_relaxed)) {

    }
  }

  return true;
}

void quiescent_state(int t) {

  auto epoch = global_epoch.load(std::memory_order_acquire);

  if (local_epoch[t].load(std::memory_order_relaxed) == epoch) {
    const auto new_epoch = (epoch + 1) % kEpochs;
    if (!try_update_epoch(epoch, new_epoch)) {
      return;
    }
    epoch = new_epoch;
  }

  local_epoch[t].store(epoch, std::memory_order_release);
  free_list(retire_lists[t][epoch]);
}

void ensure_block(int t) {
  if (has_block[t]) {
    return;
  }
  has_block[t] = true;

  auto epoch = global_epoch.load(std::memory_order_relaxed);
  do {
    local_epoch[t].store(epoch, std::memory_order_relaxed);

  } while (!global_epoch.compare_exchange_weak(
    epoch, epoch, std::memory_order_acq_rel, std::memory_order_relaxed));
}

void enter_region(int t) {
  ensure_block(t);
  ++region_entries[t];
}

void leave_region(int t) {
  assert(region_entries[t] > 0);
  if (--region_entries[t] == 0) {
    quiescent_state(t);
  }
}

void retire(int t, Node* n) {
  n->retired_epoch = local_epoch[t].load(std::memory_order_relaxed);
  assert(n->retired_epoch < kEpochs);
  n->next = retire_lists[t][n->retired_epoch];
  retire_lists[t][n->retired_epoch] = n;
}

void pump(int t, int rounds) {
  for (int i = 0; i < rounds; ++i) {
    enter_region(t);
    leave_region(t);
  }
}

void* thread_a(void*) {
  enter_region(0);
  unlinked.store(true, std::memory_order_release);
  retire(0, &n0);
  leave_region(0);

  pump(0, 3);
  return nullptr;
}

void* thread_b(void*) {
  enter_region(1);
  if (!unlinked.load(std::memory_order_acquire)) {
    holds[1].store(true, std::memory_order_release);
  }
  while (!unlinked.load(std::memory_order_acquire)) {

  }
  holds[1].store(false, std::memory_order_release);
  leave_region(1);
  return nullptr;
}

}

int main() {
  for (int t = 0; t < kThreads; ++t) {
    local_epoch[t].store(0, std::memory_order_relaxed);

    active[t].store(true, std::memory_order_relaxed);
    holds[t].store(false, std::memory_order_relaxed);
    region_entries[t] = 0;
    has_block[t] = false;
    for (unsigned i = 0; i < kEpochs; ++i) {
      retire_lists[t][i] = nullptr;
    }
  }

  pthread_t a;
  pthread_t b;
  pthread_create(&a, nullptr, thread_a, nullptr);
  pthread_create(&b, nullptr, thread_b, nullptr);
  pthread_join(a, nullptr);
  pthread_join(b, nullptr);

  assert(freed_count <= 1);
  return 0;
}
