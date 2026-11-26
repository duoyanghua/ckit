// QSBR/RCU (liburcu rewrite): read-side sections, explicit quiescent states,
// synchronize_rcu, call_rcu and rcu_barrier.
#include <ckit/reclamation/qsbr/liburcu_qsbr.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>
#include <vector>

#include "test_utils.hpp"

using namespace ckit_test;

namespace {

struct payload {
  rcu_head head;  // must stay first: the callback casts rcu_head* back
  int value;
  static inline std::atomic<int> freed{0};

  explicit payload(int v) : head{}, value(v) {}
  ~payload() { freed.fetch_add(1, std::memory_order_relaxed); }
};

static_assert(offsetof(payload, head) == 0);

void delete_payload(rcu_head* head) {
  delete reinterpret_cast<payload*>(head);
}

void test_single_thread() {
  rcu_register_thread();
  CKIT_CHECK_EQ(rcu_read_ongoing(), 1);

  rcu_read_lock();
  rcu_read_unlock();
  rcu_quiescent_state();

  payload* one = new payload(1);
  call_rcu(&one->head, delete_payload);
  rcu_barrier();
  CKIT_CHECK_EQ(payload::freed.load(), 1);

  std::vector<payload*> batch;
  for (int i = 0; i < 100; ++i) {
    payload* item = new payload(i);
    batch.push_back(item);
    call_rcu(&item->head, delete_payload);
  }
  rcu_barrier();
  CKIT_CHECK_EQ(payload::freed.load(), 101);

  rcu_thread_offline();
  CKIT_CHECK_EQ(rcu_read_ongoing(), 0);
  rcu_thread_online();
  CKIT_CHECK_EQ(rcu_read_ongoing(), 1);

  rcu_unregister_thread();
  std::printf("  liburcu call_rcu/barrier: ok\n");
}

void test_readers() {
  std::atomic<payload*> shared{nullptr};
  std::atomic<bool> stop{false};
  std::atomic<long long> reads{0};
  payload::freed.store(0);

  rcu_register_thread();

  std::vector<std::thread> readers;
  for (int t = 0; t < 4; ++t) {
    readers.emplace_back([&] {
      rcu_register_thread();
      while (!stop.load(std::memory_order_acquire)) {
        rcu_read_lock();
        payload* current = shared.load(std::memory_order_acquire);
        if (current != nullptr) {
          reads.fetch_add(current->value, std::memory_order_relaxed);
        }
        rcu_read_unlock();
        rcu_quiescent_state();
      }
      rcu_unregister_thread();
    });
  }

  payload* first = new payload(1);
  shared.store(first, std::memory_order_release);
  std::this_thread::sleep_for(std::chrono::milliseconds(30));

  payload* second = new payload(2);
  shared.store(second, std::memory_order_release);
  synchronize_rcu();
  call_rcu(&first->head, delete_payload);
  rcu_barrier();
  CKIT_CHECK_EQ(payload::freed.load(), 1);

  stop.store(true, std::memory_order_release);
  for (auto& t : readers) t.join();

  shared.store(nullptr, std::memory_order_release);
  call_rcu(&second->head, delete_payload);
  rcu_barrier();
  CKIT_CHECK_EQ(payload::freed.load(), 2);
  rcu_unregister_thread();

  std::printf("  liburcu 4 readers: ok (freed=%d, reads=%lld)\n", payload::freed.load(),
              reads.load());
}

}  // namespace

int main() {
  Watchdog watchdog;
  begin("reclamation/qsbr_liburcu");

  test_single_thread();
  test_readers();

  return finish();
}
