// SPDX-License-Identifier: MIT
// GenMC model of the executor's in-flight counter.
// Run: genmc -- inflight_model.cpp

#include <atomic>
#include <cassert>
#include <cstddef>
#include <pthread.h>

namespace {

std::atomic<std::size_t> inflight {1};
std::atomic<bool> c_done {false};
std::atomic<bool> done {false};

void* worker(void*) {

#ifdef ADD_AFTER_SUB
  if (inflight.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    done.store(true, std::memory_order_release);
  }
  inflight.fetch_add(1, std::memory_order_relaxed);
#else
  inflight.fetch_add(1, std::memory_order_relaxed);
  if (inflight.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    done.store(true, std::memory_order_release);
  }
#endif

  c_done.store(true, std::memory_order_release);
  if (inflight.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    done.store(true, std::memory_order_release);
  }

  return nullptr;
}

void* observer(void*) {
  while (!done.load(std::memory_order_acquire)) {
  }
  assert(c_done.load(std::memory_order_acquire) &&
         "topology torn down before its last successor ran");
  return nullptr;
}

}

int main() {
  pthread_t w;
  pthread_t o;
  pthread_create(&w, nullptr, worker, nullptr);
  pthread_create(&o, nullptr, observer, nullptr);
  pthread_join(w, nullptr);
  pthread_join(o, nullptr);
  return 0;
}
