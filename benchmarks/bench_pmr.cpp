// Allocation throughput: new/delete vs the teaching memory resources,
// plus vector growth with polymorphic_allocator.
#include <ckit/pmr/pmr_teaching.hpp>
#include <ckit/pmr/pmr_teaching_allocator.hpp>

#include <cstddef>
#include <vector>

#include "bench_util.hpp"

namespace {

constexpr int kBlocks = 1000000;
constexpr std::size_t kBlockSize = 64;
constexpr std::size_t kAlignment = 8;

struct block {
  void* p;
};

template <class Alloc, class Free>
void alloc_free_case(const char* name, Alloc alloc, Free free_fn) {
  std::vector<void*> blocks(kBlocks);
  const auto start = bench::now_ns();
  for (int i = 0; i < kBlocks; ++i) blocks[static_cast<std::size_t>(i)] = alloc();
  for (int i = 0; i < kBlocks; ++i) free_fn(blocks[static_cast<std::size_t>(i)]);
  const auto elapsed = bench::now_ns() - start;
  bench::report(name, kBlocks, static_cast<double>(elapsed));
}

void new_delete_case() {
  alloc_free_case(
      "new/delete, 64 B",
      [] { return ::operator new(kBlockSize); },
      [](void* p) { ::operator delete(p); });
}

void unsynchronized_pool_case() {
  pmrt::unsynchronized_pool_resource pool;
  alloc_free_case(
      "unsynchronized_pool_resource, 64 B",
      [&] { return pool.allocate(kBlockSize, kAlignment); },
      [&](void* p) { pool.deallocate(p, kBlockSize, kAlignment); });
}

void monotonic_case() {
  pmrt::monotonic_buffer_resource resource;
  const auto start = bench::now_ns();
  for (int i = 0; i < kBlocks; ++i) {
    void* p = resource.allocate(kBlockSize, kAlignment);
    bench::keep_alive(p);
  }
  const auto elapsed = bench::now_ns() - start;
  bench::report("monotonic_buffer_resource, 64 B", kBlocks, static_cast<double>(elapsed));
}

void synchronized_pool_case() {
  pmrt::synchronized_pool_resource pool;
  alloc_free_case(
      "synchronized_pool_resource, 64 B",
      [&] { return pool.allocate(kBlockSize, kAlignment); },
      [&](void* p) { pool.deallocate(p, kBlockSize, kAlignment); });
}

template <class Vector>
void vector_case(const char* name, Vector& values) {
  const auto start = bench::now_ns();
  for (int i = 0; i < kBlocks; ++i) values.push_back(i);
  const auto elapsed = bench::now_ns() - start;
  bench::report(name, kBlocks, static_cast<double>(elapsed));
  bench::keep_alive(values.data());
}

}  // namespace

int main() {
  bench::header("PMR allocation throughput");
  new_delete_case();
  unsynchronized_pool_case();
  synchronized_pool_case();
  monotonic_case();

  bench::header("vector growth, 1M ints");
  {
    std::vector<int> values;
    vector_case("std::vector<int> (no reserve)", values);
  }
  {
    std::vector<int> values;
    values.reserve(kBlocks);
    vector_case("std::vector<int> (reserve)", values);
  }
  {
    pmrt::monotonic_buffer_resource resource;
    pmrt::vector<int> values(&resource);
    vector_case("pmrt::vector<int> on monotonic (no reserve)", values);
  }
  {
    pmrt::unsynchronized_pool_resource pool;
    pmrt::vector<int> values(&pool);
    values.reserve(kBlocks);
    vector_case("pmrt::vector<int> on pool (reserve)", values);
  }

  return 0;
}
