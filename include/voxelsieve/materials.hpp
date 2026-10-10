#pragma once

// Segmentation of a dataset into materials by grey value (ADR 0013). A voxel is material when it
// is above the air threshold and enough of its 26 neighbours are too, which removes isolated
// noise spikes but keeps walls one voxel thin; material then grows into touching voxels above a
// lower threshold (hysteresis). Each material voxel gets the class of the mean grey value of the
// material voxels around it, split by thresholds between the classes (multi-level Otsu by
// default). The result is a material volume: sparse integer bricks with the dataset's layout
// plus materials.json.

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "voxelsieve/dataset.hpp"
#include "voxelsieve/source.hpp"
#include "voxelsieve/voxel_size.hpp"

namespace voxelsieve {

struct Material {
  int id = 0;  // 1 and up; 0 is air
  std::string name;
  std::array<std::uint8_t, 3> color{};
  /// Grey value (mean of the material around a voxel) from which on it belongs to this class.
  float lower = 0.0F;
  std::int64_t voxel_count = 0;
  double volume_mm3 = 0.0;
};

struct SegmentationOptions {
  /// Number of material classes.
  int materials = 2;
  /// Smoothed grey value above which a voxel is material; default: the dataset's threshold.
  std::optional<float> air_threshold;
  /// Voxels of the 3^3 neighbourhood (itself included) that must be above the air threshold.
  /// A wall one voxel thin gives 9, an isolated noise spike 1.
  int min_neighbours = 6;
  /// Voxels between `grow_fraction` of the way from the air level to the air threshold and the
  /// threshold itself become material when they touch material, repeated `grow_steps` times
  /// (hysteresis), which closes gaps that noise tears into walls.
  float grow_fraction = 0.5F;
  int grow_steps = 1;
  /// Thresholds between the material classes (materials - 1, ascending); default: multi-level
  /// Otsu over the grey values above the air threshold.
  std::vector<float> material_thresholds;
  /// Names and colours of the classes in class order, for example as defined in the histogram
  /// of the studio; a class without one is "Material <id>" in the default colour.
  std::vector<std::string> names;
  std::vector<std::array<std::uint8_t, 3>> colors;
};

struct MaterialVolumeInfo {
  std::array<std::int64_t, 3> dims{};
  VoxelSize voxel_size;
  std::int64_t brick_size = 0;
  float air_level = 0.0F;
  float air_threshold = 0.0F;
  float grow_threshold = 0.0F;
  int grow_steps = 0;
  int min_neighbours = 0;
  std::vector<Material> materials;
  std::vector<std::array<std::int64_t, 3>> bricks;
  /// Name of the learned model that made the volume; empty for the threshold segmentation.
  std::string model;
};

/// Segments `dataset` and writes the material volume to `dir` (created, must be empty). Streams
/// brick by brick, each read with a halo of grow_steps + 1 voxels.
MaterialVolumeInfo segmentMaterials(const Dataset& dataset, const std::filesystem::path& dir,
                                    const SegmentationOptions& options = {});

[[nodiscard]] MaterialVolumeInfo readMaterialVolumeInfo(const std::filesystem::path& dir);

/// Read access to a material volume. Bricks are loaded on demand and a bounded number of them is
/// cached. All methods may be called from several threads.
class MaterialVolume {
 public:
  [[nodiscard]] static MaterialVolume open(const std::filesystem::path& dir);

  MaterialVolume(MaterialVolume&&) noexcept;
  MaterialVolume& operator=(MaterialVolume&&) noexcept;
  MaterialVolume(const MaterialVolume&) = delete;
  MaterialVolume& operator=(const MaterialVolume&) = delete;
  ~MaterialVolume();

  [[nodiscard]] const MaterialVolumeInfo& info() const;

  /// Material ids (0 = air) of every `stride`-th level-0 voxel of `box`, starting at box.min, x
  /// fastest; `out.size()` must be the product of ceil(box.size(a) / stride).
  void readRegion(const Box& box, std::span<std::uint8_t> out, std::int64_t stride = 1) const;

 private:
  struct Impl;
  explicit MaterialVolume(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

/// Agreement of a material volume with labelled components (ground truth).
struct MaterialScore {
  /// Rows: true class (0 = unlabelled, then the material ids); columns: segmented material.
  std::vector<std::vector<std::int64_t>> confusion;
  /// Dice coefficient per material id (index 0: labelled vs segmented material of any class).
  std::vector<double> dice;
  /// Number of labelled components and how many were assigned to each material.
  std::int64_t components = 0;
  std::vector<std::int64_t> components_per_material;
};

/// Compares `segmentation` with a label volume of components (0 = unlabelled, other values are
/// component ids, as in the target folder of a training dataset). A component's true material is
/// the class of its median grey value in `dataset` under the segmentation's thresholds. When
/// the labels were joined from parts numbered independently, `part_starts` (along `part_axis`)
/// keeps equal ids of different parts apart. With `region`, only its voxels count, for example
/// to score on data that a learned model did not see in training.
[[nodiscard]] MaterialScore scoreMaterials(const MaterialVolume& segmentation,
                                           const VolumeSource& labels, const Dataset& dataset,
                                           const std::vector<std::int64_t>& part_starts = {},
                                           int part_axis = 2,
                                           const std::optional<Box>& region = std::nullopt);

/// Thresholds that split `histogram` (one count per 16-bit grey value) into `classes` classes
/// with the largest between-class variance (multi-level Otsu), ascending.
[[nodiscard]] std::vector<float> multiOtsu(const std::vector<std::uint64_t>& histogram,
                                           int classes);

}  // namespace voxelsieve
