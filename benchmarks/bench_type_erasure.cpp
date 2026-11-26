// Invocation and construction cost of the hand-written function wrappers
// versus std::function, for small (SBO-friendly) and large callables.
#include <ckit/type_erasure/function_copyable.hpp>
#include <ckit/type_erasure/function_copyable_sbo_rtti.hpp>

#include <array>
#include <functional>
#include <string>

#include "bench_util.hpp"

namespace {

constexpr int kInvocations = 20000000;
constexpr int kConstructions = 300000;

template <class Fn>
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
int invoke_opaque(const Fn& fn, int value) {
  return fn(value);
}

template <class Fn, class Callable>
void measure(const char* name, const char* kind, Callable callable) {
  int state = 1;
  Fn fn = callable(state);
  long long sum = 0;
  const std::string label = std::string(name) + " (" + kind + ")";

  auto start = bench::now_ns();
  for (int i = 0; i < kInvocations; ++i) sum += invoke_opaque(fn, i);
  auto elapsed = bench::now_ns() - start;
  bench::report((label + " invoke").c_str(), kInvocations, static_cast<double>(elapsed));

  start = bench::now_ns();
  for (int i = 0; i < kConstructions; ++i) {
    Fn local = callable(state + i);
    sum += invoke_opaque(local, i);
  }
  elapsed = bench::now_ns() - start;
  bench::report((label + " construct+invoke").c_str(), kConstructions,
                static_cast<double>(elapsed));

  bench::keep_alive(&sum);
}

template <class Fn>
void run_variant(const char* name) {
  measure<Fn>(name, "small", [](int capture) {
    return [capture](int x) { return x + capture; };
  });
  measure<Fn>(name, "large", [](int capture) {
    return [payload = std::array<char, 96>{}, capture](int x) {
      return x + capture + static_cast<int>(payload[0]);
    };
  });
}

}  // namespace

int main() {
  bench::header("Type-erased function wrappers");
  run_variant<ckit::te::copyable::MyFunction<int(int)>>("MyFunction copyable (heap)");
  run_variant<ckit::te::copyable_sbo_rtti::MyFunction<int(int)>>("MyFunction copyable SBO+target");
  run_variant<std::function<int(int)>>("std::function");

  return 0;
}
