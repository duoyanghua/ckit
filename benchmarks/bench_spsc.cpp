// 1P/1C throughput of the three SPSC ring buffers.
#include <ckit/spsc/spsc_boost.hpp>
#include <ckit/spsc/spsc_folly.hpp>
#include <ckit/spsc/spsc_rigtorp.hpp>

#include <atomic>
#include <thread>

#include "bench_util.hpp"

namespace {

constexpr int kCapacity = 4096;
constexpr int kItems = 5000000;

template <class Queue, class Push, class Pop>
void run_case(const char* name, Push push, Pop pop) {
  Queue queue(kCapacity);
  std::atomic<long long> checksum{0};

  auto round = [&](int count) {
    std::thread producer([&] {
      for (int i = 0; i < count; ++i) push(queue, i);
    });
    std::thread consumer([&] {
      long long sum = 0;
      for (int i = 0; i < count; ++i) {
        int value = 0;
        while (!pop(queue, value)) std::this_thread::yield();
        sum += value;
      }
      checksum.fetch_add(sum, std::memory_order_relaxed);
    });
    producer.join();
    consumer.join();
  };

  round(200000);
  const auto start = bench::now_ns();
  round(kItems);
  const auto elapsed = bench::now_ns() - start;
  bench::report(name, kItems, static_cast<double>(elapsed));
}

}  // namespace

int main() {
  bench::header("SPSC ring buffers (1 producer / 1 consumer, capacity 4096)");

  run_case<spsc::rigtorp::SPSCQueue<int>>(
      "spsc::rigtorp::SPSCQueue",
      [](auto& q, int v) {
        while (!q.try_push(v)) std::this_thread::yield();
      },
      [](auto& q, int& v) {
        int* p = q.front();
        if (!p) return false;
        v = *p;
        q.pop();
        return true;
      });

  run_case<spsc::boostv::spsc_queue<int>>(
      "spsc::boostv::spsc_queue",
      [](auto& q, int v) {
        while (!q.push(v)) std::this_thread::yield();
      },
      [](auto& q, int& v) { return q.pop(v); });

  run_case<spsc::follyv::ProducerConsumerQueue<int>>(
      "spsc::follyv::ProducerConsumerQueue",
      [](auto& q, int v) {
        while (!q.write(v)) std::this_thread::yield();
      },
      [](auto& q, int& v) { return q.read(v); });

  return 0;
}
