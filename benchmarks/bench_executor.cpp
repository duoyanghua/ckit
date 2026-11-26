// Task throughput of the Taskflow-style executor vs a serial loop.
#include <ckit/threadpool/executor.hpp>

#include <atomic>

#include "bench_util.hpp"

namespace {

constexpr int kFlatTasks = 200000;
constexpr int kChainTasks = 50000;
constexpr int kSerialTasks = 200000;

void serial_baseline() {
  std::atomic<long long> counter{0};
  const auto start = bench::now_ns();
  for (int i = 0; i < kSerialTasks; ++i) counter.fetch_add(1, std::memory_order_relaxed);
  const auto elapsed = bench::now_ns() - start;
  bench::report("serial loop (atomic increment)", kSerialTasks, static_cast<double>(elapsed));
}

void flat_case() {
  tp::Executor executor(4);
  tp::Taskflow taskflow;
  std::atomic<long long> counter{0};
  for (int i = 0; i < kFlatTasks; ++i) {
    taskflow.emplace([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
  }
  executor.run(taskflow).wait();

  const auto start = bench::now_ns();
  executor.run(taskflow).wait();
  const auto elapsed = bench::now_ns() - start;
  bench::report("Executor flat graph (4 workers)", kFlatTasks, static_cast<double>(elapsed));
}

void chain_case() {
  tp::Executor executor(4);
  tp::Taskflow taskflow;
  std::atomic<long long> counter{0};
  tp::Task previous;
  for (int i = 0; i < kChainTasks; ++i) {
    tp::Task task = taskflow.emplace([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
    if (i > 0) previous.precede(task);
    previous = task;
  }
  executor.run(taskflow).wait();

  const auto start = bench::now_ns();
  executor.run(taskflow).wait();
  const auto elapsed = bench::now_ns() - start;
  bench::report("Executor chain (4 workers)", kChainTasks, static_cast<double>(elapsed));
}

}  // namespace

int main() {
  bench::header("Executor task throughput");
  serial_baseline();
  flat_case();
  chain_case();
  return 0;
}
