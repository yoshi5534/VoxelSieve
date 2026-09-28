#pragma once

#include <array>
#include <cstddef>
#include <nlohmann/json.hpp>
#include <string>

namespace voxelsieve {

/// Edge lengths of a voxel in mm along x, y and z (ADR 0012). CT volumes are often sampled more
/// coarsely between slices than within them, and slices can be thinner than their spacing. The
/// grid stays regular and lossless in voxel indices; everything that measures scales each axis by
/// its own pitch.
struct VoxelSize {
  std::array<double, 3> pitch_mm{1.0, 1.0, 1.0};
  /// Thickness of a slice along z when it is thinner than the pitch, which leaves a gap between
  /// slices. 0: the slices touch (thickness = pitch along z). Informational: measurements use the
  /// pitch, since each voxel stands for the whole pitch.
  double slice_thickness_mm = 0.0;

  constexpr VoxelSize() = default;
  /// Cubic voxels.
  constexpr VoxelSize(
      double size)  // NOLINT(google-explicit-constructor,hicpp-explicit-conversions)
      : pitch_mm{size, size, size} {}
  constexpr VoxelSize(double x, double y, double z) : pitch_mm{x, y, z} {}

  [[nodiscard]] constexpr double operator[](std::size_t axis) const { return pitch_mm[axis]; }
  [[nodiscard]] constexpr bool isotropic() const {
    return pitch_mm[0] == pitch_mm[1] && pitch_mm[1] == pitch_mm[2];
  }
  [[nodiscard]] constexpr double volumeMm3() const {
    return pitch_mm[0] * pitch_mm[1] * pitch_mm[2];
  }
  [[nodiscard]] double minMm() const;
  [[nodiscard]] double maxMm() const;
  /// Edge of the cube with the same volume, for estimates that need one length.
  [[nodiscard]] double meanMm() const;
  [[nodiscard]] constexpr double sliceThicknessMm() const {
    return slice_thickness_mm > 0.0 ? slice_thickness_mm : pitch_mm[2];
  }
  [[nodiscard]] constexpr double sliceGapMm() const {
    return pitch_mm[2] > sliceThicknessMm() ? pitch_mm[2] - sliceThicknessMm() : 0.0;
  }
  /// Position in mm of a position in voxel units (voxel centres at integers).
  [[nodiscard]] constexpr std::array<double, 3> toMm(const std::array<double, 3>& voxels) const {
    return {voxels[0] * pitch_mm[0], voxels[1] * pitch_mm[1], voxels[2] * pitch_mm[2]};
  }
  [[nodiscard]] constexpr std::array<double, 3> toVoxels(const std::array<double, 3>& mm) const {
    return {mm[0] / pitch_mm[0], mm[1] / pitch_mm[1], mm[2] / pitch_mm[2]};
  }
  /// Voxels `factor` times as large along every axis (coarser resolution levels); the slice
  /// thickness belongs to the scan and is kept.
  [[nodiscard]] VoxelSize scaled(double factor) const;
  /// Throws std::invalid_argument unless all pitches are positive and finite and the slice
  /// thickness is not negative and not larger than the pitch along z.
  void validate() const;

  constexpr bool operator==(const VoxelSize&) const = default;
};

/// Writes "voxel_size_mm" into `object`, as a number for cubic voxels and as [x, y, z] otherwise,
/// and "slice_thickness_mm" when the slices are thinner than their pitch.
void writeVoxelSize(nlohmann::json& object, const VoxelSize& size);
/// Reads what writeVoxelSize wrote; "voxel_size_mm" may be a number or [x, y, z].
[[nodiscard]] VoxelSize readVoxelSize(const nlohmann::json& object);
/// A voxel size given as a number or [x, y, z] (parameters, command lines).
[[nodiscard]] VoxelSize voxelSizeFromJson(const nlohmann::json& value);
/// JSON form of the pitch alone: a number for cubic voxels, else [x, y, z].
// NOLINTBEGIN(readability-identifier-naming): names required by nlohmann::json
void to_json(nlohmann::json& json, const VoxelSize& size);
void from_json(const nlohmann::json& json, VoxelSize& size);
// NOLINTEND(readability-identifier-naming)
/// "0.1" for cubic voxels or "0.33,0.33,0.6" for x, y and z (command lines).
[[nodiscard]] VoxelSize parseVoxelSize(const std::string& text);
/// "0.1 mm" or "0.33 x 0.33 x 0.6 mm", with the slice thickness when given.
[[nodiscard]] std::string describe(const VoxelSize& size);

}  // namespace voxelsieve
