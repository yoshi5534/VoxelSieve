#pragma once

// Deterministic, order-independent random numbers for synthetic volumes.

#include <cmath>
#include <cstdint>
#include <numbers>

namespace voxelsieve::detail {

inline std::uint64_t splitMix64(std::uint64_t value) {
  value += 0x9E3779B97F4A7C15ULL;
  value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
  return value ^ (value >> 31U);
}

/// Uniform sample in [0, 1) that depends only on (seed, index).
inline double uniformNoise(std::uint64_t seed, std::uint64_t index) {
  constexpr double kScale = 1.0 / 9007199254740992.0;  // 2^-53
  return static_cast<double>(splitMix64(seed ^ splitMix64(index)) >> 11U) * kScale;
}

/// Standard normal sample that depends only on (seed, index), so any voxel can be generated
/// independently and in any order (Box-Muller on two hashed uniforms).
inline double gaussianNoise(std::uint64_t seed, std::uint64_t index) {
  const std::uint64_t h1 = splitMix64(seed ^ splitMix64(2 * index));
  const std::uint64_t h2 = splitMix64(seed ^ splitMix64(2 * index + 1));
  constexpr double kScale = 1.0 / 9007199254740992.0;                 // 2^-53
  const double u1 = (static_cast<double>(h1 >> 11U) + 1.0) * kScale;  // (0, 1]
  const double u2 = static_cast<double>(h2 >> 11U) * kScale;          // [0, 1)
  return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * std::numbers::pi * u2);
}

}  // namespace voxelsieve::detail
