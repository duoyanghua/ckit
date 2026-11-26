// SPDX-License-Identifier: MIT
// GenMC model of the EBR three-epoch protocol.
// Run: genmc -- ebr_model.cpp

#include <atomic>
#include <cassert>
#include <pthread.h>

#ifdef MODEL_DEBUG
#include <cstdio>
#endif

namespace {

constexpr int kThreads = 2;
constexpr unsigned kEpochs = 3;
constexpr unsigned kScanFreq = 1;

std::atomic<unsigned> global_epoch{0};
std::atomic<bool> in_cs[kThreads];
std::atomic<unsigned> local_epoch[kThreads];

struct Node {
  Node* next;
  unsigned retired_epoch;
  bool freed;
};

Node n0{nullptr, 0, false};

Node* retire_lists[kThreads][kEpochs];

Node* orphans[kEpochs];

unsigned local_idx[kThreads];
unsigned nested[kThreads];
unsigned since_scan[kThreads];

int freed_count = 0;
std::atomic<bool> holds[kThreads];
std::atomic<bool> unlinked{false};
std::atomic<bool> b_left{false};

void free_node(Node* n) {
  assert(n != nullptr);
  assert(!n->freed);
#ifdef MODEL_DEBUG
  fprintf(stderr,
          "free n=%p retired=%u | t0 in=%d local=%u | t1 in=%d local=%u\n",
          (void*)n,
          n->retired_epoch,
          (int)in_cs[0].load(std::memory_order_acquire),
          local_epoch[0].load(std::memory_order_acquire),
          (int)in_cs[1].load(std::memory_order_acquire),
          local_epoch[1].load(std::memory_order_acquire));
#endif

  for (int t = 0; t < kThreads; ++t) {
    assert(!holds[t].load(std::memory_order_acquire));
  }
  n->freed = true;
  ++freed_count;
}

void free_list(Node* list) {
  while (list != nullptr) {
    Node* next = list->next;
    free_node(list);
    list = next;
  }
}

unsigned idx_of(unsigned epoch) { return epoch % kEpochs; }

void update_local(int t, unsigned new_epoch) {
  const unsigned old_epoch = local_epoch[t].load(std::memory_order_relaxed);
  assert(new_epoch > old_epoch);
  local_epoch[t].store(new_epoch, std::memory_order_release);

  unsigned diff = new_epoch - old_epoch;
  if (diff > kEpochs) {
    diff = kEpochs;
  }
  unsigned idx = local_idx[t];
  for (int i = static_cast<int>(diff) - 1; i >= 0; --i) {
    idx = (new_epoch - static_cast<unsigned>(i)) % kEpochs;
    Node* list = retire_lists[t][idx];
    retire_lists[t][idx] = nullptr;
    free_list(list);
  }
  local_idx[t] = idx;
}

bool scan(unsigned epoch) {
#ifdef BUG_SCAN

  (void)epoch;
  return true;
#else
  for (int t = 0; t < kThreads; ++t) {
    if (in_cs[t].load(std::memory_order_acquire) &&
        local_epoch[t].load(std::memory_order_acquire) != epoch) {
      return false;
    }
  }
  std::atomic_thread_fence(std::memory_order_acquire);
  return true;
#endif
}

void reclaim_orphans(unsigned epoch) {
  Node* list = orphans[idx_of(epoch)];
  orphans[idx_of(epoch)] = nullptr;
  free_list(list);
}

unsigned advance(unsigned curr) {
  if (global_epoch.load(std::memory_order_relaxed) == curr) {
    std::atomic_thread_fence(std::memory_order_acquire);
    unsigned expected = curr;
    if (global_epoch.compare_exchange_strong(
          expected, curr + 1, std::memory_order_release, std::memory_order_relaxed)) {
      reclaim_orphans(curr + 1);
      return curr + 1;
    }
  }
  return global_epoch.load(std::memory_order_relaxed);
}

unsigned try_advance() {
  const unsigned epoch = global_epoch.load(std::memory_order_relaxed);
  if (!scan(epoch)) {
    return epoch;
  }
  return advance(epoch);
}

void abandon(int t) {
  for (unsigned i = 0; i < kEpochs; ++i) {
    if (retire_lists[t][i] != nullptr) {

      Node* list = retire_lists[t][i];
      retire_lists[t][i] = nullptr;
      while (list != nullptr) {
        Node* next = list->next;
        list->next = orphans[i];
        orphans[i] = list;
        list = next;
      }
    }
  }
}

void t_enter(int t) {
  if (nested[t]++ != 0) {
    return;
  }
  in_cs[t].store(true, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_seq_cst);

  const unsigned epoch = global_epoch.load(std::memory_order_acquire);
  if (local_epoch[t].load(std::memory_order_relaxed) != epoch) {
    since_scan[t] = 0;
    update_local(t, epoch);
  } else if (since_scan[t]++ == kScanFreq) {
    since_scan[t] = 0;
    if (scan(epoch)) {
      const unsigned next = advance(epoch);
      update_local(t, next);
    }
  }
}

void t_leave(int t) {
  if (--nested[t] != 0) {
    return;
  }
  in_cs[t].store(false, std::memory_order_release);
  abandon(t);
}

void retire(int t, Node* n) {
  n->retired_epoch = local_epoch[t].load(std::memory_order_relaxed);
  n->next = retire_lists[t][local_idx[t]];
  retire_lists[t][local_idx[t]] = n;
}

void* thread_a(void*) {
  t_enter(0);
  unlinked.store(true, std::memory_order_release);
  retire(0, &n0);
  t_leave(0);

  try_advance();
  t_enter(0);
  t_leave(0);
  try_advance();
  t_enter(0);
  t_leave(0);
  try_advance();
  t_enter(0);
  t_leave(0);

  while (!b_left.load(std::memory_order_acquire)) {

  }

  try_advance();
  t_enter(0);
  t_leave(0);
  try_advance();
  t_enter(0);
  t_leave(0);
  try_advance();
  t_enter(0);
  t_leave(0);

  assert(n0.freed);
  assert(freed_count == 1);
  return nullptr;
}

void* thread_b(void*) {
  t_enter(1);
  if (!unlinked.load(std::memory_order_acquire)) {
    holds[1].store(true, std::memory_order_release);
  }
  while (!unlinked.load(std::memory_order_acquire)) {

  }
  holds[1].store(false, std::memory_order_release);
  t_leave(1);
  b_left.store(true, std::memory_order_release);
  return nullptr;
}

}

int main() {
  for (int t = 0; t < kThreads; ++t) {
    in_cs[t].store(false, std::memory_order_relaxed);
    local_epoch[t].store(0, std::memory_order_relaxed);
    local_idx[t] = 0;
    nested[t] = 0;
    since_scan[t] = 0;
    for (unsigned i = 0; i < kEpochs; ++i) {
      retire_lists[t][i] = nullptr;
    }
  }
  for (unsigned i = 0; i < kEpochs; ++i) {
    orphans[i] = nullptr;
  }

  pthread_t a, b;
  pthread_create(&a, nullptr, thread_a, nullptr);
  pthread_create(&b, nullptr, thread_b, nullptr);
  pthread_join(a, nullptr);
  pthread_join(b, nullptr);

  assert(freed_count == 1);
  return 0;
}
