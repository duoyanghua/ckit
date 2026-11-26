// Hazard pointers, partial-specialization rewrite (hp_v2):
// same behaviour, raw std::atomic<T*> instead of concurrent_ptr.
// hp.hpp and hp_v2.hpp must never be included in the same translation unit.
#include <ckit/reclamation/hp/hp_v2.hpp>

#include <atomic>
#include <thread>

#include "test_utils.hpp"

using namespace ckit_test;

namespace {

using Reclaimer = hp::hazard_pointer<hp::strategy<3, 2, 100, false>>;
using Guard = hp::guard_ptr<struct node, Reclaimer>;

struct node : hp::deletable_obj_with_new_delete<node> {
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

}  // namespace

int main() {
  Watchdog watchdog;
  begin("reclamation/hp_v2");

  for (int i = 0; i < 2000; ++i) push(i);
  int value = 0;
  int count = 0;
  while (pop(value)) {
    CKIT_CHECK_EQ(value, 1999 - count);
    ++count;
  }
  CKIT_CHECK_EQ(count, 2000);

  run_threads(4, [&](int t) {
    for (int i = 0; i < 2000; ++i) {
      push(t * 2000 + i);
      int out = 0;
      if (pop(out)) {
      }
    }
  });

  while (pop(value)) {
  }
  for (int i = 0; i < 1000; ++i) {
    push(i);
    while (!pop(value)) {
    }
  }

  CKIT_CHECK(node::alive.load() < 2000);
  std::printf("  hp_v2: ok (alive=%d)\n", node::alive.load());
  return finish();
}
