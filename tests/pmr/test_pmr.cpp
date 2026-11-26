// Tests for the teaching implementation of <memory_resource>:
// monotonic buffer, unsynchronized / synchronized pool, polymorphic_allocator.
#include <ckit/pmr/pmr_teaching.hpp>
#include <ckit/pmr/pmr_teaching_allocator.hpp>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "test_utils.hpp"

using namespace ckit_test;

namespace {

class counting_resource : public pmrt::memory_resource {
 public:
  void* do_allocate(std::size_t bytes, std::size_t alignment) override {
    allocations_.fetch_add(1, std::memory_order_relaxed);
    bytes_.fetch_add(static_cast<long long>(bytes), std::memory_order_relaxed);
    live_.fetch_add(static_cast<long long>(bytes), std::memory_order_relaxed);
    return pmrt::new_delete_resource()->allocate(bytes, alignment);
  }

  void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override {
    deallocations_.fetch_add(1, std::memory_order_relaxed);
    live_.fetch_sub(static_cast<long long>(bytes), std::memory_order_relaxed);
    pmrt::new_delete_resource()->deallocate(p, bytes, alignment);
  }

  bool do_is_equal(const pmrt::memory_resource& other) const noexcept override {
    return this == &other;
  }

  long long allocations() const { return allocations_.load(); }
  long long deallocations() const { return deallocations_.load(); }
  long long bytes() const { return bytes_.load(); }
  long long live() const { return live_.load(); }

 private:
  std::atomic<long long> allocations_{0};
  std::atomic<long long> deallocations_{0};
  std::atomic<long long> bytes_{0};
  std::atomic<long long> live_{0};
};

void test_default_resource() {
  auto* original = pmrt::get_default_resource();
  counting_resource counter;
  CKIT_CHECK(pmrt::set_default_resource(&counter) == original);
  CKIT_CHECK(pmrt::get_default_resource() == &counter);

  void* p = pmrt::get_default_resource()->allocate(64, 8);
  CKIT_CHECK_EQ(reinterpret_cast<std::uintptr_t>(p) % 8, 0u);
  pmrt::get_default_resource()->deallocate(p, 64, 8);
  CKIT_CHECK_EQ(counter.allocations(), 1);
  CKIT_CHECK_EQ(counter.live(), 0);

  pmrt::set_default_resource(nullptr);
  CKIT_CHECK(pmrt::get_default_resource() == pmrt::new_delete_resource());
  pmrt::set_default_resource(original);
  std::printf("  default resource: ok\n");
}

void test_null_resource() {
  bool threw = false;
  try {
    (void)pmrt::null_memory_resource()->allocate(8, 8);
  } catch (const std::bad_alloc&) {
    threw = true;
  }
  CKIT_CHECK(threw);
  std::printf("  null resource: ok\n");
}

void test_monotonic() {
  counting_resource upstream;
  {
    pmrt::monotonic_buffer_resource mono(&upstream);
    std::vector<void*> blocks;
    for (int i = 0; i < 500; ++i) {
      const std::size_t size = static_cast<std::size_t>(i % 97 + 1);
      const std::size_t alignment = std::size_t{1} << (i % 6);
      void* p = mono.allocate(size, alignment);
      CKIT_CHECK(reinterpret_cast<std::uintptr_t>(p) % alignment == 0);
      blocks.push_back(p);
    }
    CKIT_CHECK(upstream.live() > 0);
    mono.release();
    CKIT_CHECK_EQ(upstream.live(), 0);
  }
  CKIT_CHECK_EQ(upstream.allocations(), upstream.deallocations());

  alignas(64) unsigned char buffer[512];
  {
    pmrt::monotonic_buffer_resource mono(buffer, sizeof(buffer), &upstream);
    void* first = mono.allocate(16, 8);
    CKIT_CHECK(first >= static_cast<void*>(buffer) &&
               first < static_cast<void*>(buffer + sizeof(buffer)));
    for (int i = 0; i < 100; ++i) (void)mono.allocate(64, 64);
    CKIT_CHECK(upstream.allocations() > 0);
    mono.release();
  }
  CKIT_CHECK_EQ(upstream.allocations(), upstream.deallocations());
  std::printf("  monotonic_buffer_resource: ok\n");
}

void test_unsynchronized_pool() {
  counting_resource upstream;
  {
    pmrt::pool_options options;
    options.max_blocks_per_chunk = 128;
    options.largest_required_pool_block = 1024;
    pmrt::unsynchronized_pool_resource pool(options, &upstream);

    struct block {
      void* p;
      std::size_t size;
    };
    std::vector<block> blocks;
    for (int i = 0; i < 4000; ++i) {
      const std::size_t size = static_cast<std::size_t>(i % 128) * 8 + 8;
      void* p = pool.allocate(size, 8);
      CKIT_CHECK(reinterpret_cast<std::uintptr_t>(p) % 8 == 0);
      blocks.push_back({p, size});
    }
    CKIT_CHECK(upstream.allocations() < 4000);
    for (auto& b : blocks) pool.deallocate(b.p, b.size, 8);
    pool.release();
    CKIT_CHECK_EQ(upstream.live(), 0);
  }
  CKIT_CHECK_EQ(upstream.allocations(), upstream.deallocations());

  pmrt::unsynchronized_pool_resource big_pool;
  void* huge = big_pool.allocate(1 << 20, 64);
  CKIT_CHECK(reinterpret_cast<std::uintptr_t>(huge) % 64 == 0);
  big_pool.deallocate(huge, 1 << 20, 64);
  std::printf("  unsynchronized_pool_resource: ok\n");
}

void test_synchronized_pool() {
  counting_resource upstream;
  {
    pmrt::synchronized_pool_resource pool({}, &upstream);

    struct block {
      void* p;
      std::size_t size;
    };
    std::vector<std::vector<block>> per_thread(8);

    run_threads(8, [&](int t) {
      per_thread[static_cast<std::size_t>(t)].reserve(500);
      for (int i = 0; i < 500; ++i) {
        const std::size_t size = static_cast<std::size_t>(t * 37 + i % 61) + 8;
        per_thread[static_cast<std::size_t>(t)].push_back({pool.allocate(size, 8), size});
      }
    });

    for (auto& blocks : per_thread) {
      for (auto& b : blocks) pool.deallocate(b.p, b.size, 8);
    }
    pool.release();
    CKIT_CHECK_EQ(upstream.live(), 0);
  }
  std::printf("  synchronized_pool_resource: ok\n");
}

void test_polymorphic_allocator() {
  counting_resource upstream;
  {
    pmrt::monotonic_buffer_resource mono(&upstream);
    pmrt::polymorphic_allocator<int> alloc(&mono);
    CKIT_CHECK(alloc.resource() == &mono);

    pmrt::vector<int> values(alloc);
    for (int i = 0; i < 10000; ++i) values.push_back(i);
    CKIT_CHECK_EQ(values.size(), 10000u);
    CKIT_CHECK_EQ(values[9999], 9999);
    CKIT_CHECK(values.get_allocator().resource() == &mono);
    CKIT_CHECK(upstream.live() > 0);

    pmrt::vector<int> copy(values);
    CKIT_CHECK(copy.get_allocator().resource() == pmrt::get_default_resource());
    CKIT_CHECK_EQ(copy.size(), values.size());

    int* object = alloc.new_object<int>(123);
    CKIT_CHECK_EQ(*object, 123);
    alloc.delete_object(object);

    void* raw = alloc.allocate_bytes(256, 128);
    CKIT_CHECK(reinterpret_cast<std::uintptr_t>(raw) % 128 == 0);
    alloc.deallocate_bytes(raw, 256, 128);

    pmrt::polymorphic_allocator<std::string> string_alloc(&mono);
    auto* text = string_alloc.new_object<std::string>("ckit");
    CKIT_CHECK_EQ(*text, std::string("ckit"));
    string_alloc.delete_object(text);
  }
  std::printf("  polymorphic_allocator: ok\n");
}

}  // namespace

int main() {
  Watchdog watchdog;
  begin("pmr");

  test_default_resource();
  test_null_resource();
  test_monotonic();
  test_unsynchronized_pool();
  test_synchronized_pool();
  test_polymorphic_allocator();

  return finish();
}
