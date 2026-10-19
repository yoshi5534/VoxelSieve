#pragma once

// Segmentation of a dataset into materials by grey value (ADR 0013). A voxel is material when it
// is above the air threshold and enough of its 26 neighbours are too, which removes isolated
// noise spikes but keeps walls one voxel thin; material then grows into touching voxels above a
// lower threshold (hysteresis). Each material voxel gets the class of the mean grey value of the
// material voxels around it, split by thresholds between the classes (by default the valleys
// between the histogram's peaks, else multi-level Otsu). The result is a material volume: sparse
// integer bricks with the dataset's layout plus materials.json.

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
#include "voxelsieve/surface.hpp"
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
  /// Number of material classes; 0: one per peak of the histogram above the air threshold, with
  /// the thresholds in the valleys between them (suggestMaterials).
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
  /// Thresholds between the material classes (materials - 1, ascending); default: the valleys
  /// between the peaks when the histogram has one clear peak per class (suggestMaterials), else
  /// multi-level Otsu over the grey values above the air threshold.
  std::vector<float> material_thresholds;
  /// Names and colours of the classes in class order, for example as defined in the histogram
  /// of the GUI; a class without one is "Material <id>" in the default colour.
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

/// Surface of material `id` of `volume` as a triangle mesh in level-0 voxel coordinates (voxel
/// index, not mm), facing outwards: the boundary between its voxels and all others, so a sheet one
/// voxel thin keeps both faces. Meshed from every stride-th voxel, the stride the smallest power
/// of two that reads at most `max_samples` voxels and, when `max_triangles` is not 0, gives at most
/// that many triangles. Empty when the material has no voxels.
[[nodiscard]] IndexedMesh materialMesh(const MaterialVolume& volume, int id,
                                       std::size_t max_triangles = 0,
                                       std::int64_t max_samples = std::int64_t{1} << 26);

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

/// Grey-value histogram of the active voxels of one level of `dataset`: one count per 16-bit grey
/// value (65536 bins; float grey values are clamped to 0 to 65535). Coarser levels are much
/// faster to count, and averaging narrows the peaks of the materials.
[[nodiscard]] std::vector<std::uint64_t> datasetHistogram(const Dataset& dataset, int level = 0);

/// One Gaussian class of a mixture fitted to a histogram: grey value, spread and share of voxels.
struct MixtureComponent {
  double mean = 0.0;
  double sigma = 0.0;
  double weight = 0.0;
};

/// Material classes found in a histogram (suggestMaterials).
struct MaterialSuggestion {
  /// Grey values of the class centres above the air threshold, ascending: one material each.
  std::vector<float> peaks;
  /// Thresholds between the classes (peaks - 1, ascending): the lowest point of the smoothed
  /// histogram between two clear peaks, or where two mixture components are equally likely.
  std::vector<float> thresholds;
  /// Clear peaks of the histogram; fewer than the classes when shoulders or tails add some.
  int clear_peaks = 0;
  /// Lowest points of the smoothed histogram between neighbouring clear peaks (clear_peaks - 1,
  /// ascending). A threshold between two classes with a valley between them lies there.
  std::vector<float> valleys;
  /// The Gaussian mixture with one component per class (grey values), empty without voxels.
  std::vector<MixtureComponent> components;
  /// Share of the voxels above the air threshold whose class is uncertain: the most likely
  /// component of the mixture has a probability below 90 %. A high share means classes that
  /// overlap and a segmentation whose boundaries depend on noise.
  double ambiguous_fraction = 0.0;
};

/// Suggests the number of materials and the thresholds between them from `histogram` (one count
/// per 16-bit grey value) above `air_threshold`. Every clear peak is a material: a peak counts
/// when the histogram falls by at least a fifth of its height on both sides before it reaches a
/// higher peak and when it is not tiny next to the highest one; when more than `max_materials`
/// peaks count, the most prominent ones are kept. A Gaussian mixture fitted to the histogram
/// (the number of components chosen by the Bayesian information criterion) also finds classes
/// that show only as a shoulder or a tail; when it finds more classes than there are peaks, its
/// classes are suggested. No class at all gives one material.
[[nodiscard]] MaterialSuggestion suggestMaterials(const std::vector<std::uint64_t>& histogram,
                                                  float air_threshold, int max_materials = 8);

/// Thresholds that split `histogram` (one count per 16-bit grey value) into `classes` classes
/// with the largest between-class variance (multi-level Otsu), ascending.
[[nodiscard]] std::vector<float> multiOtsu(const std::vector<std::uint64_t>& histogram,
                                           int classes);

}  // namespace voxelsieve
