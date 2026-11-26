// Small dependency-free timing helpers shared by the benchmarks.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

namespace bench {

using clock_type = std::chrono::steady_clock;

inline std::uint64_t now_ns() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(clock_type::now().time_since_epoch())
          .count());
}

inline void keep_alive(const void* p) {
#if defined(__GNUC__) || defined(__clang__)
  asm volatile("" : : "g"(p) : "memory");
#else
  (void)p;
#endif
}

inline void report(const char* name, std::uint64_t operations, double ns_total) {
  const double ns_per_op = ns_total / static_cast<double>(operations);
  const double mops = static_cast<double>(operations) / ns_total * 1000.0;
  std::printf("%-46s %8.2f Mops/s %9.2f ns/op\n", name, mops, ns_per_op);
}

inline void header(const char* title) {
  std::printf("\n== %s ==\n", title);
}

}  // namespace bench
