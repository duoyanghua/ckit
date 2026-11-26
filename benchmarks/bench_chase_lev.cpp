// Owner push/pop and owner + thieves throughput of the four work-stealing deques.
#include <ckit/chase_lev/chase_charlietsao.hpp>
#include <ckit/chase_lev/chase_paper.hpp>
#include <ckit/chase_lev/chase_taskflow.hpp>
#include <ckit/chase_lev/chase_xenium.hpp>

#include <atomic>
#include <optional>
#include <thread>
#include <vector>

#include "bench_util.hpp"

namespace {

constexpr int kItems = 1000000;
constexpr int kThieves = 3;

template <class Queue, class Push, class Pop>
void owner_only(const char* name, Queue& queue, Push push, Pop pop) {
  auto round = [&](int count) {
    for (int i = 0; i < count; ++i) push(queue, i);
    for (int i = 0; i < count; ++i) {
      while (!pop(queue)) std::this_thread::yield();
    }
  };

  round(100000);
  const auto start = bench::now_ns();
  round(kItems);
  const auto elapsed = bench::now_ns() - start;
  bench::report(name, 2 * kItems, static_cast<double>(elapsed));
}

template <class Queue, class Push, class Pop, class Steal>
void stealing(const char* name, Queue& queue, Push push, Pop pop, Steal steal) {
  auto round = [&](int total) {
    std::atomic<int> consumed{0};
    std::atomic<bool> go{false};

    std::thread owner([&] {
      while (!go.load(std::memory_order_acquire)) {
      }
      for (int i = 0; i < total; ++i) push(queue, i);
      while (consumed.load(std::memory_order_relaxed) < total) {
        auto value = pop(queue);
        if (value) {
          consumed.fetch_add(1, std::memory_order_relaxed);
        } else {
          std::this_thread::yield();
        }
      }
    });

    std::vector<std::thread> thieves;
    for (int t = 0; t < kThieves; ++t) {
      thieves.emplace_back([&] {
        while (!go.load(std::memory_order_acquire)) {
        }
        while (consumed.load(std::memory_order_relaxed) < total) {
          auto value = steal(queue);
          if (value) {
            consumed.fetch_add(1, std::memory_order_relaxed);
          } else {
            std::this_thread::yield();
          }
        }
      });
    }

    go.store(true, std::memory_order_release);
    owner.join();
    for (auto& t : thieves) t.join();
  };

  round(100000);
  const auto start = bench::now_ns();
  round(kItems);
  const auto elapsed = bench::now_ns() - start;
  bench::report(name, kItems, static_cast<double>(elapsed));
}

}  // namespace

int main() {
  bench::header("Chase-Lev deques (1M items, 3 thieves)");

  {
    chase::paper::WorkStealingDeque<int> queue(64);
    owner_only("chase::paper (owner push+pop)",
               queue, [](auto& q, int v) { q.push(v); },
               [](auto& q) -> std::optional<int> { return q.take(); });
    stealing("chase::paper (owner + 3 thieves)",
             queue, [](auto& q, int v) { q.push(v); },
             [](auto& q) -> std::optional<int> { return q.take(); },
             [](auto& q) -> std::optional<int> { return q.steal(); });
  }
  {
    chase::taskflow::UnboundedWSQ<int> queue(10);
    owner_only("chase::taskflow UnboundedWSQ (owner push+pop)",
               queue, [](auto& q, int v) { q.push(v); },
               [](auto& q) -> std::optional<int> { return q.pop(); });
    stealing("chase::taskflow UnboundedWSQ (owner + 3 thieves)",
             queue, [](auto& q, int v) { q.push(v); },
             [](auto& q) -> std::optional<int> { return q.pop(); },
             [](auto& q) -> std::optional<int> { return q.steal(); });
  }
  {
    chase::charlietsao::WorkStealingQueue<int> queue(64);
    owner_only("chase::charlietsao (owner push+pop)",
               queue, [](auto& q, int v) { q.push(v); },
               [](auto& q) -> std::optional<int> { return q.pop(); });
    stealing("chase::charlietsao (owner + 3 thieves)",
             queue, [](auto& q, int v) { q.push(v); },
             [](auto& q) -> std::optional<int> { return q.pop(); },
             [](auto& q) -> std::optional<int> { return q.steal(); });
  }
  {
    std::vector<int> values(kItems);
    for (int i = 0; i < kItems; ++i) values[static_cast<std::size_t>(i)] = i;
    chase::xenium::chase_work_stealing_deque<int, 65536> queue;
    owner_only(
        "chase::xenium (owner push+pop)", queue,
        [&](auto& q, int v) { q.try_push(&values[static_cast<std::size_t>(v)]); },
        [](auto& q) -> std::optional<int> {
          int* p = nullptr;
          return q.try_pop(p) ? std::optional<int>(*p) : std::nullopt;
        });
    stealing(
        "chase::xenium (owner + 3 thieves)", queue,
        [&](auto& q, int v) { q.try_push(&values[static_cast<std::size_t>(v)]); },
        [](auto& q) -> std::optional<int> {
          int* p = nullptr;
          return q.try_pop(p) ? std::optional<int>(*p) : std::nullopt;
        },
        [](auto& q) -> std::optional<int> {
          int* p = nullptr;
          return q.try_steal(p) ? std::optional<int>(*p) : std::nullopt;
        });
  }

  return 0;
}
