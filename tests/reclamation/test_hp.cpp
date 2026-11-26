// Hazard pointers: same Treiber stack, nodes protected by hp::guard_ptr and
// reclaimed once no hazard pointer references them.
#include <ckit/reclamation/hp/hp.hpp>

#include <atomic>
#include <thread>
#include <vector>

#include "test_utils.hpp"

using namespace ckit_test;

namespace {

using Reclaimer = hp::hazard_pointer<>;
using Guard = hp::guard_ptr<struct node, Reclaimer>;
using Ptr = hp::concurrent_ptr<struct node, Reclaimer>;

struct node : hp::enable_concurrent_ptr<node> {
  int value;
  node* next;
  static inline std::atomic<int> alive{0};

  explicit node(int v) : value(v), next(nullptr) {
    alive.fetch_add(1, std::memory_order_relaxed);
  }
  ~node() { alive.fetch_sub(1, std::memory_order_relaxed); }
};

Ptr head;
std::atomic<long long> pushed_sum{0};
std::atomic<long long> popped_sum{0};

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

void test_single_thread() {
  for (int i = 0; i < 1000; ++i) {
    pushed_sum.fetch_add(i);
    push(i);
  }
  int value = 0;
  int count = 0;
  while (pop(value)) {
    popped_sum.fetch_add(value);
    ++count;
  }
  CKIT_CHECK_EQ(count, 1000);
  CKIT_CHECK_EQ(popped_sum.load(), pushed_sum.load());

  churn(1000);
  CKIT_CHECK(node::alive.load() < 1000);
  std::printf("  hp single thread: ok (alive=%d)\n", node::alive.load());
}

void test_multi_thread() {
  constexpr int threads = 4;
  constexpr int per_thread = 4000;
  std::atomic<int> pops{0};

  run_threads(threads, [&](int t) {
    for (int i = 0; i < per_thread; ++i) {
      push(t * per_thread + i);
      if (i % 3 == 0) {
        int out = 0;
        if (pop(out)) pops.fetch_add(1, std::memory_order_relaxed);
      }
    }
  });

  int value = 0;
  while (pop(value)) pops.fetch_add(1, std::memory_order_relaxed);
  churn(1500);

  CKIT_CHECK(pops.load() > 0);
  CKIT_CHECK(node::alive.load() < 2000);
  std::printf("  hp %d threads: ok (popped=%d, alive=%d)\n", threads, pops.load(),
              node::alive.load());
}

}  // namespace

int main() {
  Watchdog watchdog;
  begin("reclamation/hp");

  test_single_thread();
  test_multi_thread();

  return finish();
}
