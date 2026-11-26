// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>

namespace chase {

inline constexpr std::size_t kCacheLineSize = 64;

constexpr bool is_power_of_two(std::size_t n) noexcept {
  return n != 0 && (n & (n - 1)) == 0;
}

constexpr std::size_t next_pow_two(std::size_t v) noexcept {
  if (v <= 1) return 1;
  return std::size_t{1} << (64 - __builtin_clzll(v - 1));
}

constexpr std::size_t bit_ceil(std::size_t v) noexcept {
  if (v <= 1) return 1;
  --v;
  v |= v >> 1;
  v |= v >> 2;
  v |= v >> 4;
  v |= v >> 8;
  v |= v >> 16;
  v |= v >> 32;
  return v + 1;
}

}
