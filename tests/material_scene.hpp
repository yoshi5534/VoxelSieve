#pragma once

// A scene of two materials with known labels, shared by the material and the model tests.

#include <algorithm>
#include <cstdint>
#include <random>

#include "voxelsieve/volume.hpp"

namespace voxelsieve {

// Two materials with a known layout: a hollow box of a light material (label 1), a dense cube
// floating in its cavity (label 2) and a sheet of the light material one voxel thin that spans
// the cavity (label 3), in Gaussian noise strong enough to put isolated spikes above the
// threshold in the air.
inline constexpr std::int64_t kSize = 96;
inline constexpr std::uint16_t kAir = 1000;
inline constexpr std::uint16_t kLight = 8000;
inline constexpr std::uint16_t kDense = 20000;
inline constexpr float kThreshold = 4500.0F;

struct Scene {
  Volume16 grey{{kSize, kSize, kSize}, VoxelSize{0.1}};
  Volume16 labels{{kSize, kSize, kSize}, VoxelSize{0.1}};
};

inline std::uint16_t labelAt(std::int64_t x, std::int64_t y, std::int64_t z) {
  const auto inside = [](std::int64_t v, std::int64_t lo, std::int64_t hi) {
    return v >= lo && v < hi;
  };
  const bool box = inside(x, 16, 80) && inside(y, 16, 80) && inside(z, 16, 80);
  const bool cavity = inside(x, 22, 74) && inside(y, 22, 74) && inside(z, 22, 74);
  if (box && !cavity) {
    return 1;
  }
  if (inside(x, 44, 60) && inside(y, 40, 56) && inside(z, 40, 56)) {
    return 2;
  }
  if (x == 32 && cavity) {
    return 3;
  }
  return 0;
}

inline Scene makeScene() {
  Scene scene;
  std::mt19937 rng(7);
  std::normal_distribution<double> noise(0.0, 1200.0);
  for (std::int64_t z = 0; z < kSize; ++z) {
    for (std::int64_t y = 0; y < kSize; ++y) {
      for (std::int64_t x = 0; x < kSize; ++x) {
        const std::uint16_t label = labelAt(x, y, z);
        const double value = (label == 0 ? kAir : label == 2 ? kDense : kLight) + noise(rng);
        scene.grey.data[scene.grey.index(x, y, z)] =
            static_cast<std::uint16_t>(std::clamp(value, 0.0, 65535.0));
        scene.labels.data[scene.labels.index(x, y, z)] = label;
      }
    }
  }
  return scene;
}

}  // namespace voxelsieve
