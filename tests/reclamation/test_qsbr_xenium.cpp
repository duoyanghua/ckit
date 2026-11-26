// Quiescent-state-based reclamation (xenium rewrite): readers publish a
// quiescent state when the outermost guard/region leaves.
#include <ckit/reclamation/qsbr/xenium_qsbr.hpp>

#include <atomic>
#include <thread>

#include "test_utils.hpp"

using namespace ckit_test;

namespace {

using Reclaimer = xenium_qsbr::quiescent_state_based;
using Guard = xenium_qsbr::guard_ptr<struct node, Reclaimer>;

struct node : xenium_qsbr::enable_concurrent_ptr<node> {
  int value;
  node* next;
  static inline std::atomic<int> alive{0};

  explicit node(int v) : value(v), next(nullptr) {
    alive.fetch_add(1, std::memory_order_relaxed);
  }
  ~node() { alive.fetch_sub(1, std::memory_order_relaxed); }
};

std::atomic<node*> head{nullptr};

void push(int value) {
  node* fresh = new node(value);
  node* old = head.load(std::memory_order_relaxed);
  do {
    fresh->next = old;
  } while (!head.compare_exchange_weak(old, fresh, std::memory_order_release,
                                       std::memory_order_relaxed));
}

bool pop(int& out) {
  Guard guard;
  for (;;) {
    guard.acquire(head);
    if (!guard) return false;
    node* expected = guard.get();
    node* next = expected->next;
    if (head.compare_exchange_weak(expected, next, std::memory_order_release,
                                   std::memory_order_relaxed)) {
      out = expected->value;
      guard.reclaim();
      return true;
    }
  }
}

void churn(int rounds) {
  for (int i = 0; i < rounds; ++i) {
    push(i);
    int value = 0;
    while (!pop(value)) {
    }
  }
}

}  // namespace

int main() {
  Watchdog watchdog;
  begin("reclamation/qsbr_xenium");

  for (int i = 0; i < 1000; ++i) push(i);
  int value = 0;
  int count = 0;
  while (pop(value)) {
    CKIT_CHECK_EQ(value, 999 - count);
    ++count;
  }
  CKIT_CHECK_EQ(count, 1000);

  {
    xenium_qsbr::quiescent_state_based::region_guard region;
    Guard guard;
    guard.acquire(head);
    CKIT_CHECK(!guard);
  }

  run_threads(4, [&](int t) {
    for (int i = 0; i < 4000; ++i) {
      push(t * 4000 + i);
      int out = 0;
      if (pop(out)) {
      }
    }
  });

  while (pop(value)) {
  }
  churn(1200);

  CKIT_CHECK(node::alive.load() < 1500);
  std::printf("  qsbr_xenium: ok (alive=%d)\n", node::alive.load());
  return finish();
}
