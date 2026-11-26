// Read-side overhead of the three reclamation schemes: acquire + release one
// guard around a load of a pointer that is never reclaimed.
#include <ckit/reclamation/ebr/ebr.hpp>
#include <ckit/reclamation/hp/hp.hpp>
#include <ckit/reclamation/qsbr/xenium_qsbr.hpp>

#include <atomic>

#include "bench_util.hpp"

namespace {

constexpr int kIterations = 5000000;

struct node {
  int value;
};

node stable{42};
std::atomic<node*> plain_head{&stable};

struct hp_node : hp::enable_concurrent_ptr<hp_node> {
  int value;
  explicit hp_node(int v) : value(v) {}
};

hp_node hp_stable{42};

void baseline() {
  long long sum = 0;
  const auto start = bench::now_ns();
  for (int i = 0; i < kIterations; ++i) {
    node* p = plain_head.load(std::memory_order_acquire);
    sum += p->value;
  }
  const auto elapsed = bench::now_ns() - start;
  bench::keep_alive(&sum);
  bench::report("raw atomic load", kIterations, static_cast<double>(elapsed));
}

void ebr_read() {
  using Reclaimer = ebr::epoch_based;
  ebr::guard_ptr<node, Reclaimer> guard;
  long long sum = 0;
  const auto start = bench::now_ns();
  for (int i = 0; i < kIterations; ++i) {
    guard.acquire(plain_head);
    sum += guard->value;
    guard.reset();
  }
  const auto elapsed = bench::now_ns() - start;
  bench::keep_alive(&sum);
  bench::report("ebr guard acquire/reset", kIterations, static_cast<double>(elapsed));
}

void hp_read() {
  using Reclaimer = hp::hazard_pointer<>;
  hp::concurrent_ptr<hp_node, Reclaimer> head;
  head.store(&hp_stable);
  hp::guard_ptr<hp_node, Reclaimer> guard;
  long long sum = 0;
  const auto start = bench::now_ns();
  for (int i = 0; i < kIterations; ++i) {
    guard.acquire(head);
    sum += guard->value;
    guard.reset();
  }
  const auto elapsed = bench::now_ns() - start;
  bench::keep_alive(&sum);
  bench::report("hp guard acquire/reset", kIterations, static_cast<double>(elapsed));
}

void qsbr_read() {
  using Reclaimer = xenium_qsbr::quiescent_state_based;
  xenium_qsbr::guard_ptr<node, Reclaimer> guard;
  long long sum = 0;
  const auto start = bench::now_ns();
  for (int i = 0; i < kIterations; ++i) {
    guard.acquire(plain_head);
    sum += guard->value;
    guard.reset();
  }
  const auto elapsed = bench::now_ns() - start;
  bench::keep_alive(&sum);
  bench::report("qsbr guard acquire/reset", kIterations, static_cast<double>(elapsed));
}

}  // namespace

int main() {
  bench::header("Reclamation read-side cost (5M guarded loads)");
  baseline();
  ebr_read();
  hp_read();
  qsbr_read();
  return 0;
}
