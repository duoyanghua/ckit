// Tests for the three single-producer / single-consumer ring buffers:
// rigtorp/SPSCQueue, boost::lockfree::spsc_queue (extracted) and
// folly::ProducerConsumerQueue.
#include <ckit/spsc/spsc_boost.hpp>
#include <ckit/spsc/spsc_folly.hpp>
#include <ckit/spsc/spsc_rigtorp.hpp>

#include <atomic>
#include <thread>
#include <vector>

#include "test_utils.hpp"

using namespace ckit_test;

namespace {

void test_rigtorp() {
  spsc::rigtorp::SPSCQueue<int> queue(8);
  CKIT_CHECK_EQ(queue.capacity(), 8u);
  CKIT_CHECK(queue.empty());
  CKIT_CHECK(queue.front() == nullptr);

  for (int i = 0; i < 8; ++i) CKIT_CHECK(queue.try_push(i));
  CKIT_CHECK(!queue.try_push(99));
  CKIT_CHECK_EQ(queue.size(), 8u);

  for (int i = 0; i < 8; ++i) {
    int* value = queue.front();
    CKIT_CHECK(value && *value == i);
    queue.pop();
  }
  CKIT_CHECK(queue.empty());
  CKIT_CHECK(queue.front() == nullptr);

  for (int round = 0; round < 3; ++round) {
    for (int i = 0; i < 8; ++i) queue.emplace(i);
    for (int i = 0; i < 8; ++i) {
      CKIT_CHECK(queue.front() && *queue.front() == i);
      queue.pop();
    }
  }
  std::printf("  spsc_rigtorp: ok\n");
}

void test_boost() {
  spsc::boostv::spsc_queue<int> queue(8);
  CKIT_CHECK(queue.empty());
  CKIT_CHECK_EQ(queue.write_available(), 8u);

  for (int i = 0; i < 8; ++i) CKIT_CHECK(queue.push(i));
  CKIT_CHECK(!queue.push(99));
  CKIT_CHECK_EQ(queue.read_available(), 8u);

  for (int i = 0; i < 8; ++i) {
    int value = -1;
    CKIT_CHECK(queue.pop(value));
    CKIT_CHECK_EQ(value, i);
  }
  CKIT_CHECK(queue.empty());

  int input[8] = {0, 1, 2, 3, 4, 5, 6, 7};
  CKIT_CHECK_EQ(queue.push(input, 4), 4u);
  int output[8] = {};
  CKIT_CHECK_EQ(queue.pop(output, 4), 4u);
  for (int i = 0; i < 4; ++i) CKIT_CHECK_EQ(output[i], i);

  int sum = 0;
  queue.push(input, 3);
  CKIT_CHECK_EQ(queue.consume_all([&sum](int value) { sum += value; }), 3u);
  CKIT_CHECK_EQ(sum, 3);
  CKIT_CHECK(queue.empty());
  std::printf("  spsc_boost: ok\n");
}

void test_folly() {
  spsc::follyv::ProducerConsumerQueue<int> queue(8);
  CKIT_CHECK(queue.isEmpty());
  CKIT_CHECK_EQ(queue.capacity(), 8u);

  for (int i = 0; i < 8; ++i) CKIT_CHECK(queue.write(i));
  CKIT_CHECK(!queue.write(99));
  CKIT_CHECK(queue.isFull());

  for (int i = 0; i < 8; ++i) {
    int value = -1;
    CKIT_CHECK(queue.read(value));
    CKIT_CHECK_EQ(value, i);
  }
  CKIT_CHECK(queue.isEmpty());

  for (int round = 0; round < 3; ++round) {
    for (int i = 0; i < 8; ++i) queue.write(i);
    for (int i = 0; i < 8; ++i) {
      int* value = queue.frontPtr();
      CKIT_CHECK(value && *value == i);
      queue.popFront();
    }
  }
  std::printf("  spsc_folly: ok\n");
}

template <class Queue, class Push, class Pop>
void test_threaded(const char* name, Push push, Pop pop) {
  constexpr int total = 200000;
  Queue queue(1024);
  std::atomic<bool> ok{true};

  std::thread producer([&] {
    for (int i = 0; i < total; ++i) push(queue, i);
  });
  std::thread consumer([&] {
    for (int i = 0; i < total; ++i) {
      int value = -1;
      while (!pop(queue, value)) std::this_thread::yield();
      if (value != i) ok.store(false, std::memory_order_relaxed);
    }
  });
  producer.join();
  consumer.join();
  CKIT_CHECK(ok.load());
  std::printf("  %s (1P/1C, %d items): ok\n", name, total);
}

}  // namespace

int main() {
  Watchdog watchdog;
  begin("spsc");

  test_rigtorp();
  test_boost();
  test_folly();

  test_threaded<spsc::rigtorp::SPSCQueue<int>>(
      "spsc_rigtorp",
      [](auto& q, int v) {
        while (!q.try_push(v)) std::this_thread::yield();
      },
      [](auto& q, int& v) {
        int* value = q.front();
        if (!value) return false;
        v = *value;
        q.pop();
        return true;
      });

  test_threaded<spsc::boostv::spsc_queue<int>>(
      "spsc_boost",
      [](auto& q, int v) {
        while (!q.push(v)) std::this_thread::yield();
      },
      [](auto& q, int& v) { return q.pop(v); });

  test_threaded<spsc::follyv::ProducerConsumerQueue<int>>(
      "spsc_folly",
      [](auto& q, int v) {
        while (!q.write(v)) std::this_thread::yield();
      },
      [](auto& q, int& v) { return q.read(v); });

  return finish();
}
