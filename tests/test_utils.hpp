// Minimal dependency-free test harness shared by all ckit tests.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ckit_test {

inline std::atomic<int> failures{0};

inline void report(bool ok, const char* expr, const char* file, int line) {
  if (!ok) {
    std::printf("FAIL %s:%d  %s\n", file, line, expr);
    ++failures;
  }
}

#define CKIT_CHECK(cond) ::ckit_test::report(static_cast<bool>(cond), #cond, __FILE__, __LINE__)

#define CKIT_CHECK_EQ(a, b)                                                          \
  do {                                                                               \
    const auto ckit_va = (a);                                                        \
    const auto ckit_vb = (b);                                                        \
    if (!(ckit_va == ckit_vb)) {                                                     \
      std::printf("FAIL %s:%d  %s == %s\n", __FILE__, __LINE__, #a, #b);             \
      ++::ckit_test::failures;                                                       \
    }                                                                                \
  } while (0)

template <class F>
void run_threads(int n, F&& body) {
  std::vector<std::thread> threads;
  threads.reserve(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) {
    threads.emplace_back([&body, i] { body(i); });
  }
  for (auto& t : threads) t.join();
}

inline bool wait_for(const std::atomic<int>& counter, int target, int spins = 200000) {
  for (int i = 0; i < spins; ++i) {
    if (counter.load(std::memory_order_acquire) >= target) return true;
    std::this_thread::yield();
  }
  return counter.load(std::memory_order_acquire) >= target;
}

struct Watchdog {
  std::thread thread;
  explicit Watchdog(unsigned seconds = 180) {
    thread = std::thread([seconds] {
      std::this_thread::sleep_for(std::chrono::seconds(seconds));
      std::printf("TIMEOUT: test exceeded %u s\n", seconds);
      std::_Exit(2);
    });
  }
  ~Watchdog() { thread.detach(); }
};

inline int begin(const char* name) {
  std::printf("== %s ==\n", name);
  return 0;
}

inline int finish() {
  if (failures.load() == 0) {
    std::printf("ALL PASS\n");
    return 0;
  }
  std::printf("%d FAILURE(S)\n", failures.load());
  return 1;
}

}  // namespace ckit_test
