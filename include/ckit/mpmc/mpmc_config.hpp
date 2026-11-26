// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>

namespace mpmc {

inline constexpr std::size_t kCacheLineSize = 64;

constexpr bool is_power_of_two(std::size_t n) noexcept {
  return n != 0 && (n & (n - 1)) == 0;
}

}
