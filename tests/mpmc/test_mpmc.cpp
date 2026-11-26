// Tests for the three bounded MPMC queues:
// Vyukov's original, rigtorp/MPMCQueue and xenium's vyukov_bounded_queue.
#include <ckit/mpmc/mpmc_rigtorp.hpp>
#include <ckit/mpmc/mpmc_vyukov.hpp>
#include <ckit/mpmc/mpmc_xenium.hpp>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "test_utils.hpp"

using namespace ckit_test;

namespace {

constexpr int kCapacity = 1024;
constexpr int kProducers = 4;
constexpr int kConsumers = 4;
constexpr int kPerProducer = 5000;

template <class Queue, class Push, class Pop>
void test_threaded(const char* name, Push push, Pop pop) {
  constexpr int total = kProducers * kPerProducer;
  Queue queue(kCapacity);
  std::unique_ptr<std::atomic<int>[]> seen(new std::atomic<int>[total]);
  for (int i = 0; i < total; ++i) seen[i].store(0, std::memory_order_relaxed);

  std::atomic<int> consumed{0};
  std::atomic<bool> ok{true};

  std::vector<std::thread> consumers;
  for (int c = 0; c < kConsumers; ++c) {
    consumers.emplace_back([&] {
      for (;;) {
        if (consumed.load(std::memory_order_acquire) >= total) break;
        int value = -1;
        if (pop(queue, value)) {
          if (value < 0 || value >= total ||
              seen[value].fetch_add(1, std::memory_order_relaxed) != 0) {
            ok.store(false, std::memory_order_relaxed);
          }
          consumed.fetch_add(1, std::memory_order_release);
        } else {
          std::this_thread::yield();
        }
      }
    });
  }

  run_threads(kProducers, [&](int t) {
    for (int i = 0; i < kPerProducer; ++i) {
      push(queue, t * kPerProducer + i);
    }
  });

  for (auto& t : consumers) t.join();
  CKIT_CHECK(ok.load());
  CKIT_CHECK_EQ(consumed.load(), total);
  for (int i = 0; i < total; ++i) {
    if (seen[i].load() != 1) {
      CKIT_CHECK(false);
      break;
    }
  }
  std::printf("  %s (%dP/%dC, %d items): ok\n", name, kProducers, kConsumers, total);
}

void test_vyukov() {
  mpmc::vyukov::mpmc_bounded_queue<int> queue(kCapacity);
  for (int i = 0; i < kCapacity; ++i) CKIT_CHECK(queue.enqueue(i));
  int value = 0;
  CKIT_CHECK(!queue.enqueue(1));
  for (int i = 0; i < kCapacity; ++i) {
    CKIT_CHECK(queue.dequeue(value));
    CKIT_CHECK_EQ(value, i);
  }
  CKIT_CHECK(!queue.dequeue(value));

  test_threaded<mpmc::vyukov::mpmc_bounded_queue<int>>(
      "mpmc_vyukov",
      [](auto& q, int v) {
        while (!q.enqueue(v)) std::this_thread::yield();
      },
      [](auto& q, int& v) { return q.dequeue(v); });
}

void test_rigtorp() {
  mpmc::MPMCQueue<int> queue(kCapacity);
  for (int i = 0; i < kCapacity; ++i) CKIT_CHECK(queue.try_push(i));
  CKIT_CHECK(!queue.try_push(1));
  CKIT_CHECK_EQ(queue.size(), kCapacity);

  for (int i = 0; i < kCapacity; ++i) {
    int value = -1;
    CKIT_CHECK(queue.try_pop(value));
    CKIT_CHECK_EQ(value, i);
  }
  int value = -1;
  CKIT_CHECK(!queue.try_pop(value));

  for (int i = 0; i < 100; ++i) queue.push(i);
  for (int i = 0; i < 100; ++i) {
    CKIT_CHECK_EQ(queue.size(), 100 - i);
    queue.pop(value);
    CKIT_CHECK_EQ(value, i);
  }

  test_threaded<mpmc::MPMCQueue<int>>(
      "mpmc_rigtorp",
      [](auto& q, int v) {
        while (!q.try_push(v)) std::this_thread::yield();
      },
      [](auto& q, int& v) { return q.try_pop(v); });
}

void test_xenium() {
  mpmc::xenium::vyukov_bounded_queue<int> queue(kCapacity);
  for (int i = 0; i < kCapacity; ++i) CKIT_CHECK(queue.try_push(i));
  CKIT_CHECK(!queue.try_push(1));
  CKIT_CHECK(!queue.try_push_weak(2));

  for (int i = 0; i < kCapacity; ++i) {
    int value = -1;
    CKIT_CHECK(queue.try_pop(value));
    CKIT_CHECK_EQ(value, i);
  }
  int value = -1;
  CKIT_CHECK(!queue.try_pop(value));
  CKIT_CHECK(!queue.try_pop_weak(value));

  test_threaded<mpmc::xenium::vyukov_bounded_queue<int>>(
      "mpmc_xenium",
      [](auto& q, int v) {
        while (!q.try_push_strong(v)) std::this_thread::yield();
      },
      [](auto& q, int& v) { return q.try_pop_strong(v); });
}

}  // namespace

int main() {
  Watchdog watchdog;
  begin("mpmc");

  test_vyukov();
  test_rigtorp();
  test_xenium();

  return finish();
}
