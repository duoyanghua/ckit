// Tests for the six hand-written std::function replacements:
// copyable / move-only x plain heap / SBO / runtime type query.
// Every variant lives in its own namespace, so all six can be tested together.
#include <ckit/type_erasure/function_copyable.hpp>
#include <ckit/type_erasure/function_copyable_sbo_rtti.hpp>
#include <ckit/type_erasure/function_move_only.hpp>
#include <ckit/type_erasure/function_move_only_sbo_rtti.hpp>

#include <array>
#include <functional>
#include <memory>
#include <stdexcept>

#include "test_utils.hpp"

using namespace ckit_test;

namespace {

struct Adder {
  int base;
  int add(int x) const { return base + x; }
};

struct Triple {
  int operator()(int x) const { return x * 3; }
};

int free_function(int x) { return x - 1; }

template <class Fn>
void test_basic(const char* name) {
  Fn empty;
  CKIT_CHECK(!empty);
  bool threw = false;
  try {
    (void)empty(1);
  } catch (const std::bad_function_call&) {
    threw = true;
  }
  CKIT_CHECK(threw);

  Fn f = [](int x) { return x * 2; };
  CKIT_CHECK(static_cast<bool>(f));
  CKIT_CHECK_EQ(f(21), 42);

  Fn fp(free_function);
  CKIT_CHECK_EQ(fp(10), 9);

  int calls = 0;
  Fn mut = [calls](int x) mutable {
    calls += x;
    return calls;
  };
  CKIT_CHECK_EQ(mut(3), 3);
  CKIT_CHECK_EQ(mut(4), 7);

  long long sum = 0;
  ckit::te::copyable::MyFunction<void(int)> sink = [&sum](int x) { sum += x; };
  sink(5);
  sink(6);
  CKIT_CHECK_EQ(sum, 11);

  Fn tripled(Triple{});
  CKIT_CHECK_EQ(tripled(4), 12);
  std::printf("  %s: basic ok\n", name);
}

template <class Fn>
void test_member_pointer(const char* name) {
  Fn mem(&Adder::add);
  Adder adder{30};
  CKIT_CHECK_EQ(mem(adder, 12), 42);
  std::printf("  %s: member pointer ok\n", name);
}

template <class Fn>
void test_copyable(const char* name) {
  Fn a = [](int x) { return x + 1; };
  Fn b = a;
  CKIT_CHECK_EQ(b(1), 2);
  CKIT_CHECK_EQ(a(1), 2);

  Fn c;
  c = b;
  CKIT_CHECK_EQ(c(2), 3);

  c = c;
  CKIT_CHECK_EQ(c(3), 4);
  std::printf("  %s: copy ok\n", name);
}

template <class Fn, bool Copyable>
void test_sbo(const char* name) {
  int captured = 7;
  Fn small = [captured](int x) { return x + captured; };
  CKIT_CHECK_EQ(small(1), 8);

  Fn big = [payload = std::array<char, 64>{}, captured](int x) {
    return x + captured + static_cast<int>(payload[0]);
  };
  CKIT_CHECK_EQ(big(1), 8);

  if constexpr (Copyable) {
    Fn copy = big;
    CKIT_CHECK_EQ(copy(2), 9);
  }

  Fn moved = std::move(big);
  CKIT_CHECK_EQ(moved(3), 10);
  CKIT_CHECK(!big);
  std::printf("  %s: SBO/heap ok\n", name);
}

template <class Fn, bool Copyable>
void test_rtti(const char* name) {
  Fn f(Triple{});
  auto* direct = f.template target<Triple>();
  CKIT_CHECK(direct != nullptr);
  CKIT_CHECK_EQ((*direct)(2), 6);
  CKIT_CHECK(f.template target<int>() == nullptr);

  const Fn& const_ref = f;
  CKIT_CHECK(const_ref.template target<Triple>() != nullptr);

  Fn other = [](int x) { return x; };
  CKIT_CHECK(other.template target<Triple>() == nullptr);

  if constexpr (Copyable) {
    Fn copy = f;
    CKIT_CHECK(copy.template target<Triple>() != nullptr);
  }
  std::printf("  %s: target<T> ok\n", name);
}

template <class Fn>
void test_move_only(const char* name) {
  Fn a = [p = std::make_unique<int>(40)](int x) { return *p + x; };
  CKIT_CHECK_EQ(a(2), 42);

  Fn b = std::move(a);
  CKIT_CHECK(!a);
  CKIT_CHECK_EQ(b(2), 42);

  Fn c;
  CKIT_CHECK(!c);
  c = std::move(b);
  CKIT_CHECK(!b);
  CKIT_CHECK_EQ(c(3), 43);

  CKIT_CHECK_EQ(c(4), 44);
  std::printf("  %s: move ok\n", name);
}

}  // namespace

int main() {
  Watchdog watchdog;
  begin("type_erasure");

  test_basic<ckit::te::copyable::MyFunction<int(int)>>("copyable");
  test_basic<ckit::te::copyable_sbo_rtti::MyFunction<int(int)>>("copyable_sbo_rtti");
  test_basic<ckit::te::move_only::MyFunction<int(int)>>("move_only");
  test_basic<ckit::te::move_only_sbo_rtti::MyFunction<int(int)>>("move_only_sbo_rtti");

  test_copyable<ckit::te::copyable::MyFunction<int(int)>>("copyable");
  test_copyable<ckit::te::copyable_sbo_rtti::MyFunction<int(int)>>("copyable_sbo_rtti");

  test_member_pointer<ckit::te::copyable::MyFunction<int(Adder&, int)>>("copyable");
  test_member_pointer<ckit::te::copyable_sbo_rtti::MyFunction<int(Adder&, int)>>("copyable_sbo_rtti");
  test_member_pointer<ckit::te::move_only::MyFunction<int(Adder&, int)>>("move_only");
  test_member_pointer<ckit::te::move_only_sbo_rtti::MyFunction<int(Adder&, int)>>("move_only_sbo_rtti");

  test_move_only<ckit::te::move_only::MyFunction<int(int)>>("move_only");
  test_move_only<ckit::te::move_only_sbo_rtti::MyFunction<int(int)>>("move_only_sbo_rtti");

  test_sbo<ckit::te::copyable_sbo_rtti::MyFunction<int(int)>, true>("copyable_sbo_rtti");
  test_sbo<ckit::te::move_only_sbo_rtti::MyFunction<int(int)>, false>("move_only_sbo_rtti");

  test_rtti<ckit::te::copyable_sbo_rtti::MyFunction<int(int)>, true>("copyable_sbo_rtti");
  test_rtti<ckit::te::move_only_sbo_rtti::MyFunction<int(int)>, false>("move_only_sbo_rtti");

  return finish();
}
