// Tests for the four Chase-Lev work-stealing deques:
// paper translation, Taskflow BoundedWSQ/UnboundedWSQ, charlietsao, xenium.
#include <ckit/chase_lev/chase_charlietsao.hpp>
#include <ckit/chase_lev/chase_paper.hpp>
#include <ckit/chase_lev/chase_taskflow.hpp>
#include <ckit/chase_lev/chase_xenium.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include "test_utils.hpp"

using namespace ckit_test;

namespace {

#ifndef CKIT_CHASE_N
#define CKIT_CHASE_N 20000
#endif

void test_paper() {
  chase::paper::WorkStealingDeque<int> queue(2);
  queue.push(1);
  queue.push(2);
  queue.push(3);
  CKIT_CHECK_EQ(queue.size(), 3u);

  auto a = queue.steal();
  auto b = queue.take();
  auto c = queue.steal();
  CKIT_CHECK(a && *a == 1);
  CKIT_CHECK(b && *b == 3);
  CKIT_CHECK(c && *c == 2);
  CKIT_CHECK(!queue.take());
  CKIT_CHECK(!queue.steal());

  for (int i = 0; i < 1000; ++i) queue.push(i);
  for (int i = 999; i >= 0; --i) {
    auto value = queue.take();
    CKIT_CHECK(value && *value == i);
  }
  CKIT_CHECK(queue.empty());

  for (int i = 0; i < 1000; ++i) queue.push(i);
  for (int i = 0; i < 1000; ++i) {
    auto value = queue.steal();
    CKIT_CHECK(value && *value == i);
  }
  CKIT_CHECK(queue.empty());
  std::printf("  chase_paper: ok\n");
}

void test_taskflow() {
  {
    chase::taskflow::BoundedWSQ<int, 2> queue;
    for (int i = 0; i < 4; ++i) CKIT_CHECK(queue.try_push(i));
    CKIT_CHECK(!queue.try_push(4));
    auto a = queue.pop();
    auto b = queue.pop();
    auto c = queue.steal();
    auto d = queue.steal();
    CKIT_CHECK(a && *a == 3);
    CKIT_CHECK(b && *b == 2);
    CKIT_CHECK(c && *c == 0);
    CKIT_CHECK(d && *d == 1);
    CKIT_CHECK(!queue.pop());
  }
  {
    chase::taskflow::UnboundedWSQ<int> queue(2);
    for (int i = 0; i < 1000; ++i) queue.push(i);
    CKIT_CHECK_EQ(queue.size(), 1000u);
    CKIT_CHECK(queue.capacity() >= 1024u);
    for (int i = 999; i >= 0; --i) {
      auto value = queue.pop();
      CKIT_CHECK(value && *value == i);
    }
    CKIT_CHECK(queue.empty());
    for (int i = 0; i < 1000; ++i) queue.push(i);
    for (int i = 0; i < 1000; ++i) {
      auto value = queue.steal();
      CKIT_CHECK(value && *value == i);
    }
  }
  std::printf("  chase_taskflow: ok\n");
}

void test_charlietsao() {
  chase::charlietsao::WorkStealingQueue<int> queue(2);
  for (int i = 0; i < 1000; ++i) queue.push(i);
  CKIT_CHECK_EQ(queue.size(), 1000u);
  CKIT_CHECK(queue.capacity() >= 1024);
  for (int i = 999; i >= 0; --i) {
    auto value = queue.pop();
    CKIT_CHECK(value && *value == i);
  }
  CKIT_CHECK(queue.empty());
  for (int i = 0; i < 1000; ++i) queue.push(i);
  for (int i = 0; i < 1000; ++i) {
    auto value = queue.steal();
    CKIT_CHECK(value && *value == i);
  }
  std::printf("  chase_charlietsao: ok\n");
}

void test_xenium() {
  constexpr int count = 500;
  std::vector<int> values(count);
  for (int i = 0; i < count; ++i) values[static_cast<std::size_t>(i)] = i;

  chase::xenium::chase_work_stealing_deque<int> queue;
  for (int i = 0; i < count; ++i) {
    CKIT_CHECK(queue.try_push(&values[static_cast<std::size_t>(i)]));
  }
  CKIT_CHECK_EQ(queue.size(), static_cast<std::size_t>(count));

  for (int i = count - 1; i >= 0; --i) {
    int* value = nullptr;
    CKIT_CHECK(queue.try_pop(value) && value && *value == i);
  }
  int* value = nullptr;
  CKIT_CHECK(!queue.try_pop(value));

  for (int i = 0; i < count; ++i) queue.try_push(&values[static_cast<std::size_t>(i)]);
  for (int i = 0; i < count; ++i) {
    int* stolen = nullptr;
    CKIT_CHECK(queue.try_steal(stolen) && stolen && *stolen == i);
  }
  std::printf("  chase_xenium: ok\n");
}

template <class Queue, class Push, class Pop, class Steal>
void test_threaded(Queue& queue, int total, int thieves, Push push, Pop pop, Steal steal) {
  std::unique_ptr<std::atomic<std::uint8_t>[]> seen(
      new std::atomic<std::uint8_t>[static_cast<std::size_t>(total)]);
  for (int i = 0; i < total; ++i) seen[static_cast<std::size_t>(i)].store(0);

  std::atomic<int> consumed{0};
  std::atomic<bool> ok{true};
  std::atomic<int> ready{0};
  std::atomic<bool> go{false};

  auto consume = [&](int value) {
    if (value < 0 || value >= total ||
        seen[static_cast<std::size_t>(value)].fetch_add(1) != 0) {
      ok.store(false, std::memory_order_relaxed);
    }
    consumed.fetch_add(1, std::memory_order_relaxed);
  };

  std::thread owner([&] {
    ready.fetch_add(1, std::memory_order_release);
    while (!go.load(std::memory_order_acquire)) {
    }
    for (int i = 0; i < total; ++i) push(queue, i);
    while (consumed.load(std::memory_order_relaxed) < total) {
      auto value = pop(queue);
      if (value) {
        consume(*value);
      } else {
        std::this_thread::yield();
      }
    }
  });

  std::vector<std::thread> thief_threads;
  for (int t = 0; t < thieves; ++t) {
    thief_threads.emplace_back([&] {
      ready.fetch_add(1, std::memory_order_release);
      while (!go.load(std::memory_order_acquire)) {
      }
      while (consumed.load(std::memory_order_relaxed) < total) {
        auto value = steal(queue);
        if (value) {
          consume(*value);
        } else {
          std::this_thread::yield();
        }
      }
    });
  }

  while (ready.load(std::memory_order_acquire) < thieves + 1) std::this_thread::yield();
  go.store(true, std::memory_order_release);
  owner.join();
  for (auto& t : thief_threads) t.join();

  CKIT_CHECK(consumed.load() == total);
  CKIT_CHECK(ok.load());
}

}  // namespace

int main() {
  Watchdog watchdog;
  begin("chase_lev");

  test_paper();
  test_taskflow();
  test_charlietsao();
  test_xenium();

  constexpr int total = CKIT_CHASE_N;
  constexpr int thieves = 3;
  std::printf("  threaded owner + %d thieves x %d\n", thieves, total);

  {
    chase::paper::WorkStealingDeque<int> queue(64);
    test_threaded(
        queue, total, thieves, [](auto& q, int v) { q.push(v); },
        [](auto& q) -> std::optional<int> { return q.take(); },
        [](auto& q) -> std::optional<int> { return q.steal(); });
  }
  {
    chase::taskflow::UnboundedWSQ<int> queue(8);
    test_threaded(
        queue, total, thieves, [](auto& q, int v) { q.push(v); },
        [](auto& q) -> std::optional<int> { return q.pop(); },
        [](auto& q) -> std::optional<int> { return q.steal(); });
  }
  {
    chase::charlietsao::WorkStealingQueue<int> queue(64);
    test_threaded(
        queue, total, thieves, [](auto& q, int v) { q.push(v); },
        [](auto& q) -> std::optional<int> { return q.pop(); },
        [](auto& q) -> std::optional<int> { return q.steal(); });
  }
  {
    std::vector<int> values(static_cast<std::size_t>(total));
    for (int i = 0; i < total; ++i) values[static_cast<std::size_t>(i)] = i;
    chase::xenium::chase_work_stealing_deque<int, 65536> queue;
    test_threaded(
        queue, total, thieves,
        [&](auto& q, int v) { q.try_push(&values[static_cast<std::size_t>(v)]); },
        [](auto& q) -> std::optional<int> {
          int* p = nullptr;
          if (q.try_pop(p)) return *p;
          return std::nullopt;
        },
        [](auto& q) -> std::optional<int> {
          int* p = nullptr;
          if (q.try_steal(p)) return *p;
          return std::nullopt;
        });
  }

  return finish();
}
