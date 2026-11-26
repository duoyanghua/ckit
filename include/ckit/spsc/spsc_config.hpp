// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>

namespace spsc {
using size_t = std::size_t;

inline constexpr std::size_t kCacheLineSize = 64;

constexpr std::uint32_t bit_ceil(std::uint32_t v) noexcept {
  --v;
  v |= v >> 1;
  v |= v >> 2;
  v |= v >> 4;
  v |= v >> 8;
  v |= v >> 16;
  return v + 1;
}

}
