#pragma once

// Internals shared by the surface thickness and the thickness map (thickness.hpp).

#include <cmath>
#include <cstddef>

namespace voxelsieve::detail {

/// The smallest of 1, 2, 2.5, 5 times a power of ten that is at least `value`: the end of a colour
/// scale or histogram. 1 for values up to 0.
[[nodiscard]] inline double niceCeil(double value) {
  if (value <= 0.0) {
    return 1.0;
  }
  const double power = std::pow(10.0, std::floor(std::log10(value)));
  for (const double m : {1.0, 2.0, 2.5, 5.0, 10.0}) {
    if (m * power >= value * (1.0 - 1e-9)) {
      return m * power;
    }
  }
  return 10.0 * power;
}

/// Regions listed in the JSON files; the counts include all of them.
inline constexpr std::size_t kListedRegions = 100;

}  // namespace voxelsieve::detail
