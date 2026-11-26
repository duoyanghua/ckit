// Tests for the Taskflow-style executor: graph ordering, continuation cache,
// spill, repeated / concurrent runs, corun and shutdown.
#include <ckit/threadpool/executor.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <random>
#include <thread>
#include <vector>

#include "test_utils.hpp"

using namespace ckit_test;

namespace {

void test_chain(std::size_t n) {
  tp::Executor executor(4);
  tp::Taskflow taskflow;
  std::atomic<std::size_t> count{0};
  std::vector<tp::Task> tasks;
  tasks.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    tasks.push_back(taskflow.emplace([&count] { count.fetch_add(1); }));
  }
  for (std::size_t i = 0; i + 1 < n; ++i) tasks[i].precede(tasks[i + 1]);
  executor.run(taskflow).wait();
  CKIT_CHECK_EQ(count.load(), n);
  std::printf("  chain %zu: ok\n", n);
}

void test_diamond() {
  tp::Executor executor(4);
  tp::Taskflow taskflow;
  std::atomic<bool> a_done{false}, b_done{false}, c_done{false}, d_done{false};

  auto a = taskflow.emplace([&] { a_done.store(true); });
  auto b = taskflow.emplace([&] {
    CKIT_CHECK(a_done.load());
    b_done.store(true);
  });
  auto c = taskflow.emplace([&] {
    CKIT_CHECK(a_done.load());
    c_done.store(true);
  });
  auto d = taskflow.emplace([&] {
    CKIT_CHECK(b_done.load());
    CKIT_CHECK(c_done.load());
    d_done.store(true);
  });
  a.precede(b);
  a.precede(c);
  b.precede(d);
  c.precede(d);

  executor.run(taskflow).wait();
  CKIT_CHECK(d_done.load());
  std::printf("  diamond: ok\n");
}

void test_random_dag(std::size_t n) {
  tp::Executor executor(4);
  tp::Taskflow taskflow;
  std::mt19937 rng(42);
  std::atomic<std::size_t> sum{0};
  std::vector<tp::Task> tasks;
  tasks.reserve(n);

  for (std::size_t i = 0; i < n; ++i) {
    tasks.push_back(taskflow.emplace([&sum, i] { sum.fetch_add(i); }));
  }
  for (std::size_t i = 1; i < n; ++i) {
    const std::size_t edges = 1 + (rng() % 2);
    for (std::size_t e = 0; e < edges; ++e) {
      tasks[rng() % i].precede(tasks[i]);
    }
  }

  executor.run(taskflow).wait();
  const std::size_t expected = n * (n - 1) / 2;
  CKIT_CHECK_EQ(sum.load(), expected);

  sum.store(0);
  executor.run(taskflow).wait();
  CKIT_CHECK_EQ(sum.load(), expected);
  std::printf("  random DAG %zu: ok\n", n);
}

void test_flat(std::size_t n) {
  tp::Executor executor(4);
  tp::Taskflow taskflow;
  std::atomic<std::size_t> count{0};
  for (std::size_t i = 0; i < n; ++i) {
    taskflow.emplace([&count] { count.fetch_add(1); });
  }
  executor.run(taskflow).wait();
  CKIT_CHECK_EQ(count.load(), n);
  std::printf("  flat %zu: ok\n", n);
}

void test_spill() {
  tp::Executor executor(4);
  tp::Taskflow taskflow;
  std::atomic<bool> root_done{false};
  std::atomic<std::size_t> count{0};

  auto root = taskflow.emplace([&] { root_done.store(true); });
  for (int i = 0; i < 1500; ++i) {
    auto child = taskflow.emplace([&] {
      CKIT_CHECK(root_done.load());
      count.fetch_add(1);
    });
    root.precede(child);
  }
  executor.run(taskflow).wait();
  CKIT_CHECK_EQ(count.load(), 1500u);
  std::printf("  spill 1500 successors: ok\n");
}

void test_repeat_runs() {
  tp::Executor executor(4);
  tp::Taskflow taskflow;
  std::atomic<std::size_t> count{0};
  taskflow.emplace([&count] { count.fetch_add(1); });

  for (int i = 0; i < 50; ++i) executor.run(taskflow).wait();
  CKIT_CHECK_EQ(count.load(), 50u);

  std::vector<tp::Future> futures;
  for (int i = 0; i < 30; ++i) futures.push_back(executor.run(taskflow));
  executor.wait_for_all();
  CKIT_CHECK_EQ(count.load(), 80u);
  CKIT_CHECK_EQ(executor.num_topologies(), 0u);
  std::printf("  repeat/queued runs: ok\n");
}

void test_concurrent_runs() {
  tp::Executor executor(4);
  tp::Taskflow taskflow;
  std::atomic<std::size_t> count{0};
  taskflow.emplace([&count] { count.fetch_add(1); });
  executor.run(taskflow).wait();
  count.store(0);

  run_threads(8, [&](int) {
    for (int i = 0; i < 20; ++i) executor.run(taskflow);
  });
  executor.wait_for_all();
  CKIT_CHECK_EQ(count.load(), 160u);
  std::printf("  concurrent runs: ok\n");
}

void test_corun() {
  tp::Executor executor(1);
  tp::Taskflow outer;
  std::atomic<std::size_t> inner_count{0};
  std::atomic<int> seen_worker{-2};

  outer.emplace([&] {
    seen_worker.store(executor.this_worker_id());
    tp::Taskflow inner;
    for (int i = 0; i < 200; ++i) {
      inner.emplace([&inner_count] { inner_count.fetch_add(1); });
    }
    executor.corun(inner);
    CKIT_CHECK_EQ(inner_count.load(), 200u);
  });

  executor.run(outer).wait();
  CKIT_CHECK_EQ(inner_count.load(), 200u);
  CKIT_CHECK_EQ(seen_worker.load(), 0);
  std::printf("  corun (1 worker, 200 tasks): ok\n");
}

void test_corun_nested() {
  tp::Executor executor(1);
  std::atomic<std::size_t> count{0};

  tp::Taskflow innermost;
  innermost.emplace([&count] { count.fetch_add(2); });
  tp::Taskflow middle;
  middle.emplace([&] { executor.corun(innermost); });
  tp::Taskflow outer;
  outer.emplace([&] { executor.corun(middle); });

  executor.run(outer).wait();
  CKIT_CHECK_EQ(count.load(), 2u);
  std::printf("  corun nested 3 levels: ok\n");
}

void test_wakeup() {
  tp::Executor executor(1);
  tp::Taskflow taskflow;
  std::atomic<std::size_t> count{0};
  taskflow.emplace([&count] { count.fetch_add(1); });

  for (int i = 0; i < 50; ++i) {
    executor.run(taskflow).wait();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (executor.num_waiters() == 0 && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    CKIT_CHECK_EQ(executor.num_waiters(), 1u);
  }
  CKIT_CHECK_EQ(count.load(), 50u);
  std::printf("  wakeup x50: ok\n");
}

void test_shutdown_order() {
  std::atomic<bool> done{false};
  tp::Taskflow taskflow;
  {
    tp::Executor executor(4);
    taskflow.emplace([&done] {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      done.store(true);
    });
    executor.run(taskflow);
  }
  CKIT_CHECK(done.load());
  std::printf("  shutdown waits for topologies: ok\n");
}

void test_multi_executor() {
  constexpr int executors_count = 4;
  std::atomic<std::size_t> done{0};
  std::vector<std::unique_ptr<tp::Taskflow>> taskflows;
  std::vector<std::unique_ptr<tp::Executor>> executors;

  for (int e = 0; e < executors_count; ++e) {
    taskflows.push_back(std::make_unique<tp::Taskflow>());
    auto executor = std::make_unique<tp::Executor>(2);
    for (int i = 0; i < 100; ++i) {
      taskflows.back()->emplace([&done] { done.fetch_add(1); });
    }
    executor->run(*taskflows.back());
    executors.push_back(std::move(executor));
  }
  for (auto& executor : executors) executor->wait_for_all();
  CKIT_CHECK_EQ(done.load(), 400u);
  std::printf("  multiple executors: ok\n");
}

}  // namespace

int main() {
  Watchdog watchdog;
  begin("threadpool/executor");

  test_chain(5000);
  test_diamond();
  test_random_dag(1000);
  test_flat(20000);
  test_spill();
  test_repeat_runs();
  test_concurrent_runs();
  test_corun();
  test_corun_nested();
  test_wakeup();
  test_shutdown_order();
  test_multi_executor();

  return finish();
}
