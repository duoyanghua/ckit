// Mutex-based vs lock-free queues and stacks from C++ Concurrency in Action.
#include <ckit/ccia/lock_free_queue_half.hpp>
#include <ckit/ccia/lock_free_queue_helping.hpp>
#include <ckit/ccia/lock_free_stack_hp.hpp>
#include <ckit/ccia/lock_free_stack_shared_ptr.hpp>
#include <ckit/ccia/threadsafe_queue.hpp>
#include <ckit/ccia/threadsafe_queue_fine_grained.hpp>
#include <ckit/ccia/threadsafe_stack.hpp>

#include <atomic>
#include <thread>
#include <vector>

#include "bench_util.hpp"

namespace {

constexpr int kThreads = 4;
constexpr int kPerProducer = 250000;
constexpr int kTotal = kThreads * kPerProducer;

template <class Queue, class Push, class Pop>
void queue_case(const char* name, Push push, Pop pop) {
  Queue queue;
  std::atomic<int> consumed{0};
  std::atomic<long long> checksum{0};

  auto round = [&](int per_producer) {
    const int total = per_producer * kThreads;
    consumed.store(0);

    std::vector<std::thread> consumers;
    for (int c = 0; c < kThreads; ++c) {
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
    for (int p = 0; p < kThreads; ++p) {
      producers.emplace_back([&, p] {
        for (int i = 0; i < per_producer; ++i) push(queue, p * per_producer + i);
      });
    }
    for (auto& t : producers) t.join();

    for (auto& t : consumers) t.join();
  };

  round(10000);
  const auto start = bench::now_ns();
  round(kPerProducer);
  const auto elapsed = bench::now_ns() - start;
  bench::report(name, kTotal, static_cast<double>(elapsed));
}

template <class Stack>
void stack_case(const char* name) {
  auto round = [&](int per_thread) {
    Stack stack;
    std::atomic<int> popped{0};

    std::vector<std::thread> producers;
    for (int t = 0; t < kThreads; ++t) {
      producers.emplace_back([&, t] {
        for (int i = 0; i < per_thread; ++i) stack.push(t * per_thread + i);
      });
    }
    for (auto& t : producers) t.join();

    std::vector<std::thread> consumers;
    for (int c = 0; c < kThreads; ++c) {
      consumers.emplace_back([&] {
        while (auto value = stack.pop()) {
          popped.fetch_add(1, std::memory_order_relaxed);
        }
      });
    }
    for (auto& t : consumers) t.join();
  };

  const auto start = bench::now_ns();
  round(kPerProducer);
  const auto elapsed = bench::now_ns() - start;
  bench::report(name, 2 * kTotal, static_cast<double>(elapsed));
}

}  // namespace

int main() {
  bench::header("CCIA queues (4P/4C, 1M items)");

  queue_case<ckit::ccia::threadsafe_queue<int>>(
      "threadsafe_queue (mutex + condition_variable)",
      [](auto& q, int v) { q.push(v); },
      [](auto& q, int& v) { return q.try_pop(v); });

  queue_case<ckit::ccia::threadsafe_queue_fine_grained<int>>(
      "threadsafe_queue (two mutexes, fine-grained)",
      [](auto& q, int v) { q.push(v); },
      [](auto& q, int& v) { return q.try_pop(v); });

  queue_case<ckit::ccia::lock_free_queue_half<int>>(
      "lock_free_queue (single-branch push)",
      [](auto& q, int v) { q.push(v); },
      [](auto& q, int& v) {
        auto p = q.pop();
        if (!p) return false;
        v = *p;
        return true;
      });

  queue_case<ckit::ccia::lock_free_queue_helping<int>>(
      "lock_free_queue (helping)",
      [](auto& q, int v) { q.push(v); },
      [](auto& q, int& v) {
        auto p = q.pop();
        if (!p) return false;
        v = *p;
        return true;
      });

  bench::header("CCIA stacks (4P/4C, 1M items)");
  stack_case<ckit::ccia::threadsafe_stack<int>>("threadsafe_stack (mutex)");
  stack_case<ckit::ccia::lock_free_stack_shared_ptr<int>>(
      "lock_free_stack (shared_ptr + atomic free functions)");
  stack_case<ckit::ccia::lock_free_stack_hp<int>>("lock_free_stack (hazard pointers)");

  return 0;
}
