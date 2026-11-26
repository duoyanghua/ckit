// SPDX-License-Identifier: GPL-3.0-or-later WITH GCC-exception-3.1 -- adapted from libstdc++ <memory_resource>.
#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <shared_mutex>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace pmrt {

using std::size_t;

class memory_resource {

  virtual void* do_allocate(size_t bytes, size_t alignment) = 0;
  virtual void do_deallocate(void* p, size_t bytes, size_t alignment) = 0;
  virtual bool do_is_equal(const memory_resource& other) const noexcept = 0;
public:
  virtual ~memory_resource() = default;

  [[nodiscard]] void*
  allocate(size_t bytes, size_t alignment = alignof(std::max_align_t)) {
    return do_allocate(bytes, alignment);
  }
  void
  deallocate(void* p, size_t bytes, size_t alignment = alignof(std::max_align_t)) {
    do_deallocate(p, bytes, alignment);
  }
  [[nodiscard]] bool
  is_equal(const memory_resource& other) const noexcept {
    return do_is_equal(other);
  }
};

inline bool
operator==(const memory_resource& a, const memory_resource& b) noexcept {
  return &a == &b || a.is_equal(b);
}
inline bool
operator!=(const memory_resource& a, const memory_resource& b) noexcept {
  return !(a == b);
}

namespace detail {
class new_delete_resource_impl final : public memory_resource {
  void*
  do_allocate(size_t bytes, size_t alignment) override {
    return ::operator new(bytes, std::align_val_t(alignment));
  }
  void
  do_deallocate(void* p, size_t bytes, size_t alignment) noexcept override {
    ::operator delete(p, std::align_val_t(alignment));
  }
  bool
  do_is_equal(const memory_resource& other) const noexcept override {
    return this == &other;
  }
};

class null_resource_impl final : public memory_resource {
  void*
  do_allocate(size_t, size_t) override {
    throw std::bad_alloc();
  }
  void
  do_deallocate(void*, size_t, size_t) noexcept override {}
  bool
  do_is_equal(const memory_resource& other) const noexcept override {
    return this == &other;
  }
};
}

inline memory_resource*
new_delete_resource() noexcept {
  static detail::new_delete_resource_impl resource;
  return &resource;
}
inline memory_resource*
null_memory_resource() noexcept {
  static detail::null_resource_impl resource;
  return &resource;
}

inline std::atomic<memory_resource*>&
default_resource_slot() noexcept {
  static std::atomic<memory_resource*> slot{new_delete_resource()};
  return slot;
}

inline memory_resource*
get_default_resource() noexcept {
  return default_resource_slot().load(std::memory_order_acquire);
}
inline memory_resource*
set_default_resource(memory_resource* resource) noexcept {
  if (resource == nullptr)
    resource = new_delete_resource();
  return default_resource_slot().exchange(resource, std::memory_order_acq_rel);
}

constexpr size_t
round_up(size_t n, size_t alignment) noexcept {
  return (n + alignment - 1) & ~(alignment - 1);

}

constexpr size_t
next_buffer_size(size_t n) noexcept {
  if (n == 0)  n = 1;
  return n + n / 2;
}

inline constexpr size_t default_buffer_size = 128 * sizeof(void*);

inline constexpr size_t pool_sizes[] = {
    8,   16,  24,  32,
    48,  64,  80,  96, 112, 128,
    192, 256, 320, 384, 448, 512,
    768, 1024,
    1536, 2048,
    3072,
    1u << 12, 1u << 13, 1u << 14, 1u << 15, 1u << 16,
    1u << 17, 1u << 20, 1u << 21, 1u << 22,
};

struct pool_options {
  size_t max_blocks_per_chunk = 0;
  size_t largest_required_pool_block = 0;

};

inline constexpr size_t max_blocks_per_chunk_limit = (1u << 19) - 1;

inline pool_options
munge_options(pool_options opts) {
  if (opts.max_blocks_per_chunk == 0)
    opts.max_blocks_per_chunk = sizeof(size_t) * 8u << 8;
  else if (opts.max_blocks_per_chunk < size_t(-4))
    opts.max_blocks_per_chunk = round_up(opts.max_blocks_per_chunk, 4);

  else
    opts.max_blocks_per_chunk &= ~size_t(3);

  opts.max_blocks_per_chunk = std::min(opts.max_blocks_per_chunk, max_blocks_per_chunk_limit);

  if (opts.largest_required_pool_block == 0)
    opts.largest_required_pool_block = sizeof(size_t) * 8u << 6;
  else
    opts.largest_required_pool_block = round_up(opts.largest_required_pool_block, pool_sizes[0]);

  opts.largest_required_pool_block = std::max<size_t>(opts.largest_required_pool_block, 64);
  opts.largest_required_pool_block = std::min<size_t>(opts.largest_required_pool_block, pool_sizes[std::size(pool_sizes) - 1]);

  return opts;
}

inline int
select_num_pools(const pool_options& opts) {
  auto* p = std::lower_bound(std::begin(pool_sizes), std::end(pool_sizes), opts.largest_required_pool_block);

  const int n = static_cast<int>(p - std::begin(pool_sizes));
  return p == std::end(pool_sizes) ? n : n + 1;

}
inline int
pool_index(size_t block_size, int npools) {
  auto* p = std::lower_bound(pool_sizes, pool_sizes + npools, block_size);
  const int n = static_cast<int>(p - pool_sizes);
  return n == npools ? -1 : n;
}

inline size_t
choose_block_size(size_t bytes, size_t alignment) {
  if (bytes == 0)
    return alignment;
  const size_t mask = std::bit_ceil(alignment) - 1;

  const size_t block_size = (bytes + mask) & ~mask;
  return block_size >= bytes ? block_size : size_t(-1);
}

inline size_t
max_blocks_for(size_t block_size) {
  constexpr size_t max_bytes = std::numeric_limits<uint32_t>::max();
  return (max_bytes - 64) * 8 / (block_size * 8 + 1);

}

template <class T>
class ResourceAllocator {
  memory_resource* resource_;

public:
  using value_type = T;
  using size_type = std::size_t;
  using is_always_equal = std::false_type;

  explicit ResourceAllocator(memory_resource* resource) noexcept
      : resource_(resource) {}
  template <class U>
  ResourceAllocator(const ResourceAllocator<U>& other) noexcept
      : resource_(other.resource()) {}
  memory_resource* resource() const noexcept { return resource_; }

  [[nodiscard]] T*
  allocate(size_t n) {
    if (n > static_cast<size_type>(std::numeric_limits<std::ptrdiff_t>::max()) / sizeof(T))
      throw std::bad_array_new_length();

    return static_cast<T*>(resource_->allocate(n * sizeof(T), alignof(T)));
  }

  void
  deallocate(T* p, size_t n) noexcept {
    resource_->deallocate(p, n * sizeof(T), alignof(T));
  }

  template <class U>
  bool operator==(const ResourceAllocator<U>& other) const noexcept {
    return resource_ == other.resource();
  }
  template <class U>
  bool operator!=(const ResourceAllocator<U>& other) const noexcept {
    return !(*this == other);
  }
};

class bitset {

  uint64_t* words_ = nullptr;
  size_t nblocks_ = 0;
  size_t next_word_ = 0;

  size_t nwords() const noexcept { return (nblocks_ + 63) / 64; }
public:
  static constexpr size_t npos = size_t(-1);
  size_t size() const noexcept { return nblocks_; }
  uint64_t* data() const noexcept { return words_; }

  bitset() = default;
  bitset(void* words, size_t nblocks) noexcept
    : words_(static_cast<uint64_t*>(words)), nblocks_(nblocks), next_word_(0) {
    const size_t full_words = nblocks_ / 64;
    if (full_words != 0)
      std::memset(words_, 0, full_words * sizeof(uint64_t));
    if (const size_t extra = nblocks_ % 64)
      words_[full_words] = ~uint64_t{0} << extra;

  }

  bool test(size_t i) const noexcept {
    assert(i < nblocks_);
    return (words_[i / 64] >> (i % 64)) & 1u;
  }

  size_t first_unset() noexcept {
    while (next_word_ < nwords() && words_[next_word_] == ~uint64_t{0})
      ++next_word_;
    if (next_word_ >= nwords())
      return npos;
    const uint64_t word = words_[next_word_];
    const size_t bit = static_cast<size_t>(std::countr_one(word));
    words_[next_word_] |= uint64_t{1} << bit;
    const size_t index = next_word_ * 64 + bit;
    while (next_word_ < nwords() && words_[next_word_] == ~uint64_t{0})
      ++next_word_;
    return index;
  }
  void clear(size_t i) noexcept {
    assert(i < nblocks_);
    const size_t word = i / 64;
    words_[word] &= ~(uint64_t{1} << (i % 64));
    if (word < next_word_)
      next_word_ = word;
  }

  bool empty() const noexcept {
    if (nblocks_ == 0)
      return true;
    const size_t words = nwords();
    for (size_t i = 0; i + 1 < words; ++i)
      if (words_[i] != 0)
        return false;
    uint64_t last = words_[words - 1];
    if (const size_t extra = nblocks_ % 64)
      last &= (uint64_t{1} << extra) - 1;
    return last == 0;
  }

  bool full() const noexcept {
    for (size_t i = next_word_; i < nwords(); ++i)
      if (words_[i] != ~uint64_t{0})
        return false;
    return true;
  }
  void reset() noexcept {

    words_ = nullptr;
    nblocks_ = 0;
    next_word_ = 0;
  }
};

class chunk {
  bitset free_;
  std::byte* base_ = nullptr;
  uint32_t bytes_ = 0;

public:
  std::byte* base() const noexcept { return base_; }
  uint32_t bytes() const noexcept { return bytes_; }
  bool empty() const noexcept { return free_.empty(); }
  void reset() noexcept {
    free_.reset();
    base_ = nullptr;
    bytes_ = 0;
  }

  chunk() = default;
  chunk(void* base, uint32_t bytes, void* words, size_t blocks) noexcept
      : free_(words, blocks), base_(static_cast<std::byte*>(base)), bytes_(bytes) {}

  chunk(const chunk&) = delete;
  chunk& operator=(const chunk&) = delete;
  chunk(chunk&& other) noexcept
      : free_(other.free_), base_(other.base_), bytes_(other.bytes_) {
    other.reset();
  }
  chunk&
  operator=(chunk&& other) noexcept {
    if (this != &other) {
      free_ = other.free_;
      base_ = other.base_;
      bytes_ = other.bytes_;
      other.reset();
    }
    return *this;
  }

  bool owns(void* p, size_t block_size) const noexcept {
    const auto base = reinterpret_cast<uintptr_t>(base_);
    const auto address = reinterpret_cast<uintptr_t>(p);
    const auto end = reinterpret_cast<uintptr_t>(free_.data());
    return base <= address && address + block_size <= end;
  }
  void*
  reserve(size_t block_size) noexcept {
    const size_t index = free_.first_unset();
    if (index == bitset::npos)
      return nullptr;
    return base_ + index * block_size;
  }
  void
  release(void* p, size_t block_size) {
    assert(owns(p, block_size));
    const size_t offset = static_cast<std::byte*>(p) - base_;
    assert(offset % block_size == 0);
    assert(free_.test(offset / block_size));
    free_.clear(offset / block_size);
  }
  bool
  try_release(void* p, size_t block_size) {
    if (!owns(p, block_size))
      return false;
    release(p, block_size);
    return true;
  }
};

class pool {
  size_t block_size_;
  size_t blocks_per_chunk_;
  memory_resource* upstream_;
  std::vector<chunk, ResourceAllocator<chunk>> chunks_;

public:
  size_t block_size() const noexcept { return block_size_; }
  pool(const pool&) = delete;
  pool& operator=(const pool&) = delete;

  pool(size_t block_size, size_t blocks_per_chunk, memory_resource* upstream)
      : block_size_(block_size), blocks_per_chunk_(blocks_per_chunk),
        upstream_(upstream), chunks_(ResourceAllocator<chunk>(upstream)) {}
  pool(pool&&) noexcept = default;
  pool&
  operator=(pool&& other) noexcept {
    if (this != &other) {
      release();
      block_size_ = other.block_size_;
      blocks_per_chunk_ = other.blocks_per_chunk_;
      upstream_ = other.upstream_;
      chunks_ = std::move(other.chunks_);
    }
    return *this;
  }
  ~pool() { release(); }

  void*
  try_allocate() noexcept {
    for (auto it = chunks_.rbegin(); it != chunks_.rend(); ++it)
      if (void* p = it->reserve(block_size_))
        return p;
    return nullptr;
  }
  void*
  allocate(const pool_options& opts) {
    if (void* p = try_allocate())
      return p;
    return replenish(opts);
  }
  bool
  deallocate(void* p) {
    auto it = std::upper_bound(chunks_.begin(), chunks_.end(), p,
        [](void* address, const chunk& c) {
          return std::less<const void*>{}(address, c.base());
        });

    if (it == chunks_.begin())
      return false;
    --it;
    return it->try_release(p, block_size_);
  }

  void
  move_nonempty_to(pool& destination) {
    for (auto& c : chunks_) {
      if (!c.empty()) {
        destination.chunks_.push_back(std::move(c));
        c.reset();
      }
    }
    std::sort(destination.chunks_.begin(), destination.chunks_.end(),
              [](const chunk& a, const chunk& b) {
                return std::less<const void*>{}(a.base(), b.base());
              });
  }
  void
  release() noexcept {
    const size_t alignment = std::bit_ceil(block_size_);

    for (auto& c : chunks_)
      if (c.base() != nullptr)
        upstream_->deallocate(c.base(), c.bytes(), alignment);
    chunks_.clear();
  }

private:
  void*
  replenish(const pool_options& opts) {
    const size_t blocks_limit = std::min(max_blocks_for(block_size_), opts.max_blocks_per_chunk);
    size_t blocks = std::min(blocks_per_chunk_, blocks_limit);
    blocks = std::max<size_t>(blocks, 1);

    const size_t words = (blocks + 63) / 64;
    const size_t bytes = blocks * block_size_ + words * sizeof(uint64_t);
    const size_t alignment = std::bit_ceil(block_size_);
    assert(bytes <= std::numeric_limits<uint32_t>::max());

    void* p = upstream_->allocate(bytes, alignment);

    void* result = nullptr;
    auto* words_ptr = static_cast<char*>(p) + blocks * block_size_;
    try {
      chunks_.emplace_back(p, static_cast<uint32_t>(bytes), words_ptr, blocks);
    } catch (...) {
      upstream_->deallocate(p, bytes, alignment);
      throw;
    }

    result = chunks_.back().reserve(block_size_);
    std::sort(chunks_.begin(), chunks_.end(),
              [](const chunk& a, const chunk& b) {
                return std::less<const void*>{}(a.base(), b.base());
              });
    if (blocks_per_chunk_ < opts.max_blocks_per_chunk) {
      blocks_per_chunk_ = std::min(blocks_limit, blocks_per_chunk_ * 2);

    }
    return result;
  }

};

using PoolArray = std::vector<pool, ResourceAllocator<pool>>;

struct big_block {
  void* pointer = nullptr;
  size_t size = 0;
  size_t alignment = 0;
};

class pool_base {
  pool_options options_;
  memory_resource* upstream_;
  int npools_;
  std::vector<big_block, ResourceAllocator<big_block>> unpooled_;

public:
  pool_base(const pool_options& opts, memory_resource* upstream)
      : options_(munge_options(opts)),
        upstream_(upstream),
        npools_(select_num_pools(options_)),
        unpooled_(ResourceAllocator<big_block>(upstream)) {}

  pool_base(const pool_base&) = delete;
  pool_base& operator=(const pool_base&) = delete;
  pool_base(pool_base&&) = delete;
  pool_base& operator=(pool_base&&) = delete;

  ~pool_base() { release(); }

  const pool_options& options() const noexcept { return options_; }
  int npools() const noexcept { return npools_; }
  memory_resource* upstream() const noexcept { return upstream_; }

  PoolArray
  make_pools() const {
    PoolArray pools{ResourceAllocator<pool>(upstream_)};
    pools.reserve(static_cast<size_t>(npools_));

    for (int i = 0; i < npools_; ++i) {
      const size_t block_size = (i + 1 == npools_)?
        options_.largest_required_pool_block: pool_sizes[i];
      size_t blocks = std::max<size_t>(16, 1024 / block_size);
      blocks = std::min(blocks, options_.max_blocks_per_chunk);
      blocks = static_cast<size_t>(blocks * (1.0 - 1.0 / (8.0 * static_cast<double>(block_size))));

      blocks = std::clamp<size_t>(blocks, 1, max_blocks_for(block_size));
      pools.emplace_back(block_size, blocks, upstream_);
    }
    return pools;
  }

  void*
  allocate_big(size_t bytes, size_t alignment) {
    size_t size = round_up(bytes, 64);

    if (size < bytes) [[unlikely]]
      size = size_t(-1);
    void* p = upstream_->allocate(size, alignment);
    try {
      const big_block block{p, size, alignment};
      auto pos = std::lower_bound(unpooled_.begin(), unpooled_.end(), p,
          [](const big_block& b, void* address) {
            return std::less<const void*>{}(b.pointer, address);
          });
      unpooled_.insert(pos, block);
    } catch (...) {
      upstream_->deallocate(p, size, alignment);
      throw;
    }
    return p;
  }
  void
  deallocate_big(void* p, size_t bytes, size_t alignment) {
    auto it = std::lower_bound(unpooled_.begin(), unpooled_.end(), p,
        [](const big_block& b, void* address) {
          return std::less<const void*>{}(b.pointer, address);
        });
    if (it == unpooled_.end() || it->pointer != p) {
      assert(false && "deallocate: pointer is not an unpooled block");
      return;
    }
    const big_block block = *it;
    assert(block.size == round_up(bytes, 64));
    assert(block.alignment == alignment);
    unpooled_.erase(it);
    upstream_->deallocate(block.pointer, block.size, block.alignment);
  }
  void
  release() noexcept {
    for (auto& block : unpooled_)
      upstream_->deallocate(block.pointer, block.size, block.alignment);

    std::vector<big_block, ResourceAllocator<big_block>> empty{ResourceAllocator<big_block>(upstream_)};
    unpooled_.swap(empty);

  }

};

class monotonic_buffer_resource : public memory_resource {
  struct buffer_header {
    buffer_header* previous;
    size_t size;
    size_t alignment;
  };
  buffer_header* head_ = nullptr;
  void* current_ = nullptr;
  size_t available_ = 0;
  size_t next_size_ = default_buffer_size;

  memory_resource* const upstream_;
  void* const original_buffer_ = nullptr;
  size_t const original_size_ = default_buffer_size;

public:
  explicit monotonic_buffer_resource(memory_resource* upstream = nullptr)
      : upstream_(upstream != nullptr ? upstream : get_default_resource()) {}
  explicit monotonic_buffer_resource(size_t initial_size, memory_resource* upstream = nullptr)
      : next_size_(initial_size),
        upstream_(upstream != nullptr ? upstream : get_default_resource()),
        original_size_(initial_size) {
    assert(initial_size > 0);
  }
  monotonic_buffer_resource(void* buffer, size_t buffer_size, memory_resource* upstream = nullptr)
      : current_(buffer),
        available_(buffer_size),
        next_size_(next_buffer_size(buffer_size)),
        upstream_(upstream != nullptr ? upstream : get_default_resource()),
        original_buffer_(buffer),
        original_size_(buffer_size) {
    assert(buffer != nullptr || buffer_size == 0);
  }
  monotonic_buffer_resource(const monotonic_buffer_resource&) = delete;
  monotonic_buffer_resource&
  operator=(const monotonic_buffer_resource&) = delete;

  monotonic_buffer_resource(monotonic_buffer_resource&&) = delete;
  monotonic_buffer_resource& operator=(monotonic_buffer_resource&&) = delete;

  memory_resource* upstream_resource() const noexcept { return upstream_; }

  ~monotonic_buffer_resource() override { release(); }

  void
  release() noexcept {
    release_buffers();
    if ((current_ = original_buffer_) != nullptr) {
      available_ = original_size_;
      next_size_ = next_buffer_size(original_size_);
    } else {
      available_ = 0;
      next_size_ = original_size_;
    }
  }
private:
  void*
  do_allocate(size_t bytes, size_t alignment) override {
    if (bytes == 0)
      bytes = 1;
    void* p = std::align(alignment, bytes, current_, available_);

    if (p == nullptr) {
      new_buffer(bytes, alignment);
      p = current_;
    }
    current_ = static_cast<std::byte*>(current_) + bytes;
    available_ -= bytes;
    return p;
  }
  void
  do_deallocate(void*, size_t, size_t) noexcept override {}
  bool
  do_is_equal(const memory_resource& other) const noexcept override {
    return this == &other;
  }

  struct buffer_allocation {
    void* data;
    size_t capacity;
  };
  void
  new_buffer(size_t bytes, size_t alignment) {
    const size_t minimum = std::max(bytes, next_size_);
    const size_t requested_alignment = round_up(alignment, alignof(std::max_align_t));
    const auto [p, capacity] = allocate_buffer(minimum, requested_alignment);

    current_ = p;
    available_ = capacity;
    next_size_ = next_buffer_size(next_size_);
  }
  buffer_allocation
  allocate_buffer(size_t payload_size, size_t alignment) {
    const size_t original = payload_size;
    payload_size = round_up(payload_size + sizeof(buffer_header), 64);
    if (payload_size < original) [[unlikely]] {
      payload_size = size_t(-1);
      alignment = size_t(1) << (sizeof(size_t) * 8 - 2);
    }
    void* p = upstream_->allocate(payload_size, alignment);
    void* const back = static_cast<char*>(p) + payload_size - sizeof(buffer_header);
    head_ = ::new (back) buffer_header{head_, payload_size, alignment};
    return {p, payload_size - sizeof(buffer_header)};
  }
  void
  release_buffers() noexcept {
    buffer_header* current = head_;
    head_ = nullptr;
    while (current != nullptr) {
      buffer_header* previous = current->previous;
      void* const start = reinterpret_cast<char*>(current + 1) - current->size;

      upstream_->deallocate(start, current->size, current->alignment);
      current = previous;
    }
  }
};

class unsynchronized_pool_resource : public memory_resource {
  pool_base base_;
  PoolArray pools_;
public:
  explicit unsynchronized_pool_resource(const pool_options& opts = {}, memory_resource* upstream = nullptr)
      : base_(opts, upstream != nullptr ? upstream : get_default_resource()),
        pools_(base_.make_pools()) {}

  unsynchronized_pool_resource(const unsynchronized_pool_resource&) = delete;
  unsynchronized_pool_resource&
  operator=(const unsynchronized_pool_resource&) = delete;

  ~unsynchronized_pool_resource() override { release(); }
  void
  release() {
    for (auto& p : pools_)
      p.release();
    PoolArray empty{ResourceAllocator<pool>(base_.upstream())};
    pools_.swap(empty);
    base_.release();
  }
  pool_options options() const noexcept { return base_.options(); }
  memory_resource* upstream_resource() const noexcept { return base_.upstream(); }

private:
  void*
  do_allocate(size_t bytes, size_t alignment) override {
    const size_t block_size = choose_block_size(bytes, alignment);
    if (block_size <= base_.options().largest_required_pool_block) {
      if (pools_.empty())
        pools_ = base_.make_pools();

      const int index = pool_index(block_size, base_.npools());
      assert(index >= 0);
      return pools_[static_cast<size_t>(index)].allocate(base_.options());
    }
    return base_.allocate_big(bytes, alignment);
  }
  void
  do_deallocate(void* p, size_t bytes, size_t alignment) override {
    const size_t block_size = choose_block_size(bytes, alignment);
    if (block_size <= base_.options().largest_required_pool_block && !pools_.empty()) {
      const int index = pool_index(block_size, base_.npools());
      const bool released = pools_[static_cast<size_t>(index)].deallocate(p);
      assert(released && "deallocate: pointer does not belong to this pool");

      return;
    }

    base_.deallocate_big(p, bytes, alignment);
  }
  bool
  do_is_equal(const memory_resource& other) const noexcept override {
    return this == &other;
  }
};

class synchronized_pool_resource;

namespace detail {
struct thread_pools {
  thread_pools* thread_next = nullptr;
  thread_pools* resource_prev = nullptr;
  thread_pools* resource_next = nullptr;
  bool is_shared = false;
  synchronized_pool_resource* owner;
  PoolArray pools;
  thread_pools(synchronized_pool_resource* resource, PoolArray&& pool_array, bool shared)
      : owner(resource), pools(std::move(pool_array)), is_shared(shared) {}
};
inline thread_local thread_pools* tls_head = nullptr;

struct tls_guard {
  ~tls_guard();
};
inline thread_local tls_guard tls_guard_instance;

inline void
ensure_tls_guard() {
  (void)tls_guard_instance;

}
}

class synchronized_pool_resource : public memory_resource {
  pool_base base_;
  mutable std::shared_mutex mutex_;
  detail::thread_pools* shared_ = nullptr;
  detail::thread_pools* tp_head_ = nullptr;

public:
  explicit synchronized_pool_resource(const pool_options& opts = {}, memory_resource* upstream = nullptr)
      : base_(opts, upstream != nullptr ? upstream : get_default_resource()) {
    std::unique_lock lock(mutex_);
    shared_ = create_thread_pools_locked(true);
  }
  synchronized_pool_resource(const synchronized_pool_resource&) = delete;
  synchronized_pool_resource&
  operator=(const synchronized_pool_resource&) = delete;

  ~synchronized_pool_resource() override { release(); }

  pool_options options() const noexcept { return base_.options(); }
  memory_resource* upstream_resource() const noexcept { return base_.upstream(); }

private:
  detail::thread_pools*
  find_current_thread_pools() noexcept {
    for (detail::thread_pools* tp = detail::tls_head; tp != nullptr; tp = tp->thread_next)
      if (tp->owner == this)
        return tp;
    return nullptr;
  }

  void
  ensure_shared_locked() {
    if (shared_ == nullptr)
      shared_ = create_thread_pools_locked(true);
  }

  detail::thread_pools*
  create_thread_pools_locked(bool shared) {
    PoolArray pools = base_.make_pools();
    void* memory = base_.upstream()->allocate(sizeof(detail::thread_pools), alignof(detail::thread_pools));
    detail::thread_pools* tp = nullptr;
    try {
      tp = ::new (memory) detail::thread_pools(this, std::move(pools), shared);
    } catch (...) {
      base_.upstream()->deallocate(memory, sizeof(detail::thread_pools), alignof(detail::thread_pools));
      throw;
    }
    tp->resource_next = tp_head_;
    if (tp_head_ != nullptr)  tp_head_->resource_prev = tp;
    tp_head_ = tp;
    if (!shared) {
      detail::ensure_tls_guard();
      tp->thread_next = detail::tls_head;
      detail::tls_head = tp;
    }
    return tp;
  }

  void
  destroy_thread_pools_locked(detail::thread_pools* tp, bool move_to_shared) {
    if (tp == nullptr)
      return;
    if (move_to_shared && tp != shared_ && shared_ != nullptr) {
      for (int i = 0; i < base_.npools(); ++i)
        tp->pools[static_cast<size_t>(i)].move_nonempty_to(shared_->pools[static_cast<size_t>(i)]);
    }

    if (tp == shared_)
      shared_ = nullptr;
    for (auto& p : tp->pools)
      p.release();
    if (tp->resource_prev != nullptr)
      tp->resource_prev->resource_next = tp->resource_next;
    else
      tp_head_ = tp->resource_next;
    if (tp->resource_next != nullptr)
      tp->resource_next->resource_prev = tp->resource_prev;

    memory_resource* upstream = base_.upstream();
    tp->~thread_pools();
    upstream->deallocate(tp, sizeof(detail::thread_pools), alignof(detail::thread_pools));
  }

public:

  void
  thread_exit(detail::thread_pools* tp) {
    if (tp == nullptr || tp == shared_)
      return;
    for (detail::thread_pools** link = &detail::tls_head; *link != nullptr; link = &(*link)->thread_next) {
      if ((*link) == tp) {
        *link = (*link)->thread_next;
        std::unique_lock lock(mutex_);
        destroy_thread_pools_locked(tp, true);
        return;
      }
    }

    assert(false && "thread_exit: tp not found in this thread's tls_head");
  }

  void
  release() {

    for (detail::thread_pools** link = &detail::tls_head; *link != nullptr; link = &(*link)->thread_next) {
      if ((*link)->owner == this) {
        *link = (*link)->thread_next;
        break;
      }
    }

    std::unique_lock lock(mutex_);
    for (detail::thread_pools* tp = tp_head_; tp != nullptr;) {
      detail::thread_pools* next = tp->resource_next;
      destroy_thread_pools_locked(tp, false);
      tp = next;
    }
    base_.release();
  }

private:
  void*
  do_allocate(size_t bytes, size_t alignment) override {
    const size_t block_size = choose_block_size(bytes, alignment);
    const pool_options opts = base_.options();
    if (block_size <= opts.largest_required_pool_block) {
      const int index = pool_index(block_size, base_.npools());
      assert(index >= 0);
      {
        std::shared_lock lock(mutex_);
        if (detail::thread_pools* tp = find_current_thread_pools())
          if (void* p = tp->pools[static_cast<size_t>(index)].try_allocate())
            return p;
      }

      std::unique_lock lock(mutex_);
      ensure_shared_locked();

      detail::thread_pools* tp = find_current_thread_pools();
      if (tp == nullptr)
        tp = create_thread_pools_locked(false);
      return tp->pools[static_cast<size_t>(index)].allocate(opts);

    }

    std::unique_lock lock(mutex_);
    return base_.allocate_big(bytes, alignment);
  }

  void
  do_deallocate(void* p, size_t bytes, size_t alignment) override {
    const size_t block_size = choose_block_size(bytes, alignment);
    const pool_options opts = base_.options();
    if (block_size <= opts.largest_required_pool_block) {
      const int index = pool_index(block_size, base_.npools());
      assert(index >= 0 && "invariant");
      {
        std::shared_lock lock(mutex_);
        if (detail::thread_pools* tp = find_current_thread_pools())
          if (tp->pools[static_cast<size_t>(index)].deallocate(p))
            return;
      }

      std::unique_lock lock(mutex_);
      detail::thread_pools* mine = find_current_thread_pools();
      for (detail::thread_pools* tp = tp_head_; tp != nullptr; tp = tp->resource_next) {
        if (tp != mine && tp->pools[static_cast<size_t>(index)].deallocate(p))
          return;
      }
      return;

    }

    std::unique_lock lock(mutex_);
    base_.deallocate_big(p, bytes, alignment);
  }

  bool
  do_is_equal(const memory_resource& other) const noexcept override {
    return this == &other;
  }
};

inline detail::tls_guard::~tls_guard() {
  detail::thread_pools* tp = detail::tls_head;
  while (tp != nullptr) {
    tp->owner->thread_exit(tp);
    tp = detail::tls_head;
  }
}

}
