// SPDX-License-Identifier: MIT
// GenMC model of the executor's two-phase notifier.
// Run: genmc -- notifier_model.cpp

#include <atomic>
#include <cassert>
#include <pthread.h>

namespace {

std::atomic<unsigned> waiters {0};
std::atomic<unsigned> epoch {0};
std::atomic<bool> pred {false};
std::atomic<bool> notifier_done {false};

std::atomic<bool> slept {false};
std::atomic<unsigned> slept_epoch {0};

void* waiter(void*) {
  waiters.fetch_add(1, std::memory_order_relaxed);
  const unsigned e = epoch.load(std::memory_order_relaxed);

#ifndef WAITER_NO_FENCE
  std::atomic_thread_fence(std::memory_order_seq_cst);
#endif

  if (pred.load(std::memory_order_relaxed)) {
    waiters.fetch_sub(1, std::memory_order_relaxed);
    return nullptr;
  }

  if (epoch.load(std::memory_order_relaxed) == e) {
    slept.store(true, std::memory_order_relaxed);
    slept_epoch.store(e, std::memory_order_relaxed);

    while (epoch.load(std::memory_order_relaxed) == e &&
           !notifier_done.load(std::memory_order_acquire)) {
    }
  }

  waiters.fetch_sub(1, std::memory_order_relaxed);
  return nullptr;
}

void* notifier(void*) {
  pred.store(true, std::memory_order_relaxed);

#ifndef NOTIFIER_NO_FENCE
  std::atomic_thread_fence(std::memory_order_seq_cst);
#endif

  if (waiters.load(std::memory_order_relaxed) != 0) {
    epoch.fetch_add(1, std::memory_order_relaxed);
  }

  notifier_done.store(true, std::memory_order_release);
  return nullptr;
}

}

int main() {
  pthread_t w;
  pthread_t n;
  pthread_create(&w, nullptr, waiter, nullptr);
  pthread_create(&n, nullptr, notifier, nullptr);
  pthread_join(w, nullptr);
  pthread_join(n, nullptr);

  assert(!(slept.load() && pred.load() && epoch.load() == slept_epoch.load()));
  return 0;
}
