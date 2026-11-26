// 4P/4C throughput of the three bounded MPMC queues.
#include <ckit/mpmc/mpmc_rigtorp.hpp>
#include <ckit/mpmc/mpmc_vyukov.hpp>
#include <ckit/mpmc/mpmc_xenium.hpp>

#include <atomic>
#include <thread>
#include <vector>

#include "bench_util.hpp"

namespace {

constexpr int kCapacity = 4096;
constexpr int kProducers = 4;
constexpr int kConsumers = 4;
constexpr int kPerProducer = 1000000;

template <class Queue, class Push, class Pop>
void run_case(const char* name, Push push, Pop pop) {
  Queue queue(kCapacity);
  std::atomic<long long> checksum{0};

  auto round = [&](int per_producer) {
    const int total = per_producer * kProducers;
    std::atomic<int> consumed{0};

    std::vector<std::thread> consumers;
    for (int c = 0; c < kConsumers; ++c) {
      consumers.emplace_back([&] {
        long long sum = 0;
        for (;;) {
          if (consumed.load(std::memory_order_acquire) >= total) break;
          int value = 0;
          if (pop(queue, value)) {
            sum += value;
            consumed.fetch_add(1, std::memory_order_release);
          } else {
            std::this_thread::yield();
          }
        }
        checksum.fetch_add(sum, std::memory_order_relaxed);
      });
    }

    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
      producers.emplace_back([&, p] {
        for (int i = 0; i < per_producer; ++i) push(queue, p * per_producer + i);
      });
    }

    for (auto& t : producers) t.join();
    for (auto& t : consumers) t.join();
  };

  round(50000);
  const auto start = bench::now_ns();
  round(kPerProducer);
  const auto elapsed = bench::now_ns() - start;
  bench::report(name, kProducers * kPerProducer, static_cast<double>(elapsed));
}

}  // namespace

int main() {
  bench::header("MPMC bounded queues (4 producers / 4 consumers, capacity 4096)");

  run_case<mpmc::vyukov::mpmc_bounded_queue<int>>(
      "mpmc::vyukov::mpmc_bounded_queue",
      [](auto& q, int v) {
        while (!q.enqueue(v)) std::this_thread::yield();
      },
      [](auto& q, int& v) { return q.dequeue(v); });

  run_case<mpmc::MPMCQueue<int>>(
      "mpmc::rigtorp::Queue (MPMCQueue)",
      [](auto& q, int v) {
        while (!q.try_push(v)) std::this_thread::yield();
      },
      [](auto& q, int& v) { return q.try_pop(v); });

  run_case<mpmc::xenium::vyukov_bounded_queue<int>>(
      "mpmc::xenium::vyukov_bounded_queue",
      [](auto& q, int v) {
        while (!q.try_push_strong(v)) std::this_thread::yield();
      },
      [](auto& q, int& v) { return q.try_pop_strong(v); });

  return 0;
}
