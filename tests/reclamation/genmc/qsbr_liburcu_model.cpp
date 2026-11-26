// SPDX-License-Identifier: LGPL-2.1-or-later
// GenMC model of the liburcu-style QSBR grace period.
// Run: genmc -- qsbr_liburcu_model.cpp

#include <atomic>
#include <cassert>
#include <pthread.h>

namespace {

std::atomic<unsigned long> gp_ctr{1};
std::atomic<unsigned long> ctr{1};
std::atomic<bool> writers_waiting{false};
std::atomic<unsigned> qs_seq{0};

int payload = 0;

bool all_readers_current(unsigned long gp) {
#ifdef SCAN_RELAXED
  const unsigned long c = ctr.load(std::memory_order_relaxed);
#else
  const unsigned long c = ctr.load(std::memory_order_acquire);
#endif
  return c == 0 || c == gp;
}

void quiescent_state() {

  const unsigned long gp = gp_ctr.load(std::memory_order_relaxed);
  if (ctr.load(std::memory_order_relaxed) == gp) {
    return;
  }
#ifdef QS_STORE_RELAXED
  ctr.store(gp, std::memory_order_relaxed);
#else
  ctr.store(gp, std::memory_order_release);
#endif

  std::atomic_thread_fence(std::memory_order_seq_cst);
  if (writers_waiting.load(std::memory_order_relaxed) == 0) {
    return;
  }
  qs_seq.fetch_add(1, std::memory_order_seq_cst);
}

void* reader(void*) {

  const int observed = payload;
  assert(observed == 0);

  quiescent_state();
  quiescent_state();

  ctr.store(0, std::memory_order_seq_cst);
  return nullptr;
}

void* writer(void*) {
  const unsigned long gp = gp_ctr.load(std::memory_order_relaxed) + 2;
  gp_ctr.store(gp, std::memory_order_release);
  writers_waiting.store(true, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_seq_cst);

  for (int i = 0; i < 4; ++i) {
    if (all_readers_current(gp)) {
      payload = 1;
      return nullptr;
    }
    const unsigned s = qs_seq.load(std::memory_order_seq_cst);
    if (all_readers_current(gp)) {
      payload = 1;
      return nullptr;
    }

    for (int k = 0; k < 3 && qs_seq.load(std::memory_order_seq_cst) == s; ++k) {

    }
  }
  return nullptr;
}

}

int main() {
  pthread_t r;
  pthread_t w;
  pthread_create(&w, nullptr, writer, nullptr);
  pthread_create(&r, nullptr, reader, nullptr);
  pthread_join(r, nullptr);
  pthread_join(w, nullptr);
  return 0;
}
