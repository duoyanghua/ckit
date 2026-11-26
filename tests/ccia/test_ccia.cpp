// Correctness tests for the C++ Concurrency in Action containers:
// mutex-based stack / queues / lookup table / list and the lock-free
// stack and queue variants.
#include <ckit/ccia/lock_free_queue_half.hpp>
#include <ckit/ccia/lock_free_queue_helping.hpp>
#include <ckit/ccia/lock_free_stack_atomic_shared_ptr.hpp>
#include <ckit/ccia/lock_free_stack_hp.hpp>
#include <ckit/ccia/lock_free_stack_shared_ptr.hpp>
#include <ckit/ccia/lock_free_stack_split_ref_count.hpp>
#include <ckit/ccia/threadsafe_list.hpp>
#include <ckit/ccia/threadsafe_lookup_table.hpp>
#include <ckit/ccia/threadsafe_queue.hpp>
#include <ckit/ccia/threadsafe_queue_fine_grained.hpp>
#include <ckit/ccia/threadsafe_stack.hpp>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "test_utils.hpp"

using namespace ckit_test;

namespace {

constexpr int kThreads = 4;
constexpr int kPerThread = 2000;

template <class Stack>
void test_lock_free_stack(const char* name) {
  Stack stack;
  std::atomic<long long> pushed_sum{0};
  run_threads(kThreads, [&](int t) {
    for (int i = 0; i < kPerThread; ++i) {
      const int value = t * kPerThread + i;
      pushed_sum.fetch_add(value, std::memory_order_relaxed);
      stack.push(value);
    }
  });

  std::atomic<int> popped{0};
  std::atomic<long long> popped_sum{0};
  run_threads(kThreads, [&](int) {
    while (auto value = stack.pop()) {
      popped.fetch_add(1, std::memory_order_relaxed);
      popped_sum.fetch_add(*value, std::memory_order_relaxed);
    }
  });

  CKIT_CHECK_EQ(popped.load(), kThreads * kPerThread);
  CKIT_CHECK_EQ(popped_sum.load(), pushed_sum.load());
  std::printf("  %s: ok\n", name);
}

template <class Queue>
void test_lock_free_queue(const char* name) {
  Queue queue;
  constexpr int total = kThreads * kPerThread;
  std::atomic<int> consumed{0};
  std::atomic<long long> consumed_sum{0};
  std::atomic<long long> pushed_sum{0};

  std::vector<std::thread> consumers;
  for (int c = 0; c < kThreads; ++c) {
    consumers.emplace_back([&] {
      while (consumed.load(std::memory_order_relaxed) < total) {
        auto value = queue.pop();
        if (value) {
          consumed.fetch_add(1, std::memory_order_relaxed);
          consumed_sum.fetch_add(*value, std::memory_order_relaxed);
        } else {
          std::this_thread::yield();
        }
      }
    });
  }

  run_threads(kThreads, [&](int t) {
    for (int i = 0; i < kPerThread; ++i) {
      const int value = t * kPerThread + i;
      pushed_sum.fetch_add(value, std::memory_order_relaxed);
      queue.push(value);
    }
  });

  for (auto& t : consumers) t.join();
  CKIT_CHECK_EQ(consumed.load(), total);
  CKIT_CHECK_EQ(consumed_sum.load(), pushed_sum.load());
  std::printf("  %s: ok\n", name);
}

void test_threadsafe_stack() {
  ckit::ccia::threadsafe_stack<int> stack;
  CKIT_CHECK(stack.empty());
  CKIT_CHECK(!stack.pop());

  stack.push(1);
  int two = 2;
  stack.push(two);
  CKIT_CHECK_EQ(*stack.pop(), 2);
  CKIT_CHECK_EQ(*stack.pop(), 1);
  CKIT_CHECK(stack.empty());

  ckit::ccia::threadsafe_stack<int> copy(stack);
  CKIT_CHECK(copy.empty());
  std::printf("  threadsafe_stack: ok\n");
}

void test_threadsafe_queue() {
  ckit::ccia::threadsafe_queue<int> queue;
  CKIT_CHECK(queue.empty());
  CKIT_CHECK(!queue.try_pop());

  constexpr int total = kThreads * kPerThread;
  std::atomic<int> got{0};
  std::atomic<long long> sum{0};
  std::atomic<long long> expected{0};

  std::vector<std::thread> consumers;
  for (int c = 0; c < kThreads; ++c) {
    consumers.emplace_back([&] {
      for (;;) {
        int value = 0;
        queue.wait_and_pop(value);
        if (value < 0) break;
        got.fetch_add(1, std::memory_order_relaxed);
        sum.fetch_add(value, std::memory_order_relaxed);
      }
    });
  }

  run_threads(kThreads, [&](int t) {
    for (int i = 0; i < kPerThread; ++i) {
      const int value = t * kPerThread + i;
      expected.fetch_add(value, std::memory_order_relaxed);
      queue.push(value);
    }
  });
  for (int c = 0; c < kThreads; ++c) queue.push(-1);

  for (auto& t : consumers) t.join();
  CKIT_CHECK_EQ(got.load(), total);
  CKIT_CHECK_EQ(sum.load(), expected.load());

  queue.push(7);
  int out = 0;
  CKIT_CHECK(queue.try_pop(out));
  CKIT_CHECK_EQ(out, 7);
  CKIT_CHECK(!queue.try_pop(out));
  std::printf("  threadsafe_queue (condition_variable): ok\n");
}

void test_threadsafe_queue_fine_grained() {
  ckit::ccia::threadsafe_queue_fine_grained<int> queue;
  CKIT_CHECK(queue.empty());
  CKIT_CHECK(!queue.try_pop());

  constexpr int total = kThreads * kPerThread;
  std::atomic<int> got{0};
  std::atomic<long long> sum{0};
  std::atomic<long long> expected{0};

  std::vector<std::thread> consumers;
  for (int c = 0; c < kThreads; ++c) {
    consumers.emplace_back([&] {
      for (;;) {
        int value = 0;
        if (queue.try_pop(value)) {
          if (value < 0) break;
          got.fetch_add(1, std::memory_order_relaxed);
          sum.fetch_add(value, std::memory_order_relaxed);
        } else {
          std::this_thread::yield();
        }
      }
    });
  }

  run_threads(kThreads, [&](int t) {
    for (int i = 0; i < kPerThread; ++i) {
      const int value = t * kPerThread + i;
      expected.fetch_add(value, std::memory_order_relaxed);
      queue.push(value);
    }
  });
  for (int c = 0; c < kThreads; ++c) queue.push(-1);

  for (auto& t : consumers) t.join();
  CKIT_CHECK_EQ(got.load(), total);
  CKIT_CHECK_EQ(sum.load(), expected.load());

  ckit::ccia::threadsafe_queue_fine_grained<std::unique_ptr<int>> move_only;
  move_only.push(std::make_unique<int>(11));
  auto value = move_only.try_pop();
  CKIT_CHECK(value && **value == 11);
  std::printf("  threadsafe_queue (fine-grained): ok\n");
}

void test_lookup_table() {
  ckit::ccia::threadsafe_lookup_table<int, int> table(8);
  CKIT_CHECK_EQ(table.value_for(1, -1), -1);

  run_threads(kThreads, [&](int t) {
    for (int i = 0; i < 1000; ++i) {
      table.add_or_update_mapping(t * 1000 + i, i);
    }
  });
  CKIT_CHECK_EQ(table.get_map().size(), 4000u);
  CKIT_CHECK_EQ(table.value_for(0, -1), 0);
  CKIT_CHECK_EQ(table.value_for(3999, -1), 999);

  table.add_or_update_mapping(0, 42);
  CKIT_CHECK_EQ(table.value_for(0, -1), 42);

  run_threads(kThreads, [&](int t) {
    for (int i = 0; i < 1000; i += 2) {
      table.remove_mapping(t * 1000 + i);
    }
  });
  CKIT_CHECK_EQ(table.get_map().size(), 2000u);
  std::printf("  threadsafe_lookup_table: ok\n");
}

void test_threadsafe_list() {
  ckit::ccia::threadsafe_list<int> list;
  for (int i = 0; i < 100; ++i) list.push_front(i);

  long long sum = 0;
  list.for_each([&](int value) { sum += value; });
  CKIT_CHECK_EQ(sum, 4950);

  auto found = list.find_first_if([](int value) { return value == 77; });
  CKIT_CHECK(found && *found == 77);
  CKIT_CHECK(!list.find_first_if([](int value) { return value == 1000; }));

  list.insert_first_if(500, [](int value) { return value == 10; });
  sum = 0;
  list.for_each([&](int value) { sum += value; });
  CKIT_CHECK_EQ(sum, 5450);

  list.remove_if([](int value) { return value % 2 == 0; });
  int count = 0;
  list.for_each([&](int) { ++count; });
  CKIT_CHECK_EQ(count, 50);

  run_threads(kThreads, [&](int t) {
    for (int i = 0; i < 500; ++i) list.push_front(t * 500 + i);
  });
  int total = 0;
  list.for_each([&](int) { ++total; });
  CKIT_CHECK_EQ(total, 50 + kThreads * 500);
  std::printf("  threadsafe_list: ok\n");
}

}  // namespace

int main() {
  Watchdog watchdog;
  begin("ccia");

  test_threadsafe_stack();
  test_threadsafe_queue();
  test_threadsafe_queue_fine_grained();
  test_lookup_table();
  test_threadsafe_list();
  test_lock_free_stack<ckit::ccia::lock_free_stack_shared_ptr<int>>("lock_free_stack_shared_ptr");
  test_lock_free_stack<ckit::ccia::lock_free_stack_atomic_shared_ptr<int>>(
      "lock_free_stack_atomic_shared_ptr");
  test_lock_free_stack<ckit::ccia::lock_free_stack_split_ref_count<int>>(
      "lock_free_stack_split_ref_count");
  test_lock_free_stack<ckit::ccia::lock_free_stack_hp<int>>("lock_free_stack_hp");
  test_lock_free_queue<ckit::ccia::lock_free_queue_half<int>>("lock_free_queue_half");
  test_lock_free_queue<ckit::ccia::lock_free_queue_helping<int>>("lock_free_queue_helping");

  return finish();
}
