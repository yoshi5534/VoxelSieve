#pragma once

// Learned segmentation models (ADR 0014): small 3D convolutional networks, trained outside
// VoxelSieve (for example with PyTorch) and stored as a .vsm file, a JSON description of the
// layers followed by the float32 weights. VoxelSieve runs them itself on the CPU, tile by tile
// with a halo, so a model segments datasets larger than RAM like the threshold segmentation
// does and writes the same material volume (materials.hpp).

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "voxelsieve/dataset.hpp"
#include "voxelsieve/materials.hpp"

namespace voxelsieve {

/// Dense feature maps: `channels` volumes of `dims` (x, y, z) voxels each, x fastest.
struct Tensor {
  int channels = 0;
  std::array<std::int64_t, 3> dims{};
  std::vector<float> data;

  [[nodiscard]] std::int64_t voxelCount() const { return dims[0] * dims[1] * dims[2]; }
};

/// One layer. Its output is named `name`; `inputs` name earlier layers or "input".
///   conv:     3D convolution, stride 1, zero padding kernel / 2 ("same" size), optional ReLU;
///             weights out x in x kz x ky x kx, then one bias per output channel
///   maxpool:  2x2x2 maximum, halves the size
///   upsample: nearest neighbour, doubles the size
///   concat:   channels of all inputs, in order
struct ModelLayer {
  std::string op;
  std::string name;
  std::vector<std::string> inputs;
  int in_channels = 0;
  int out_channels = 0;
  int kernel = 1;
  bool relu = false;
  std::vector<float> weights;
  std::vector<float> bias;
};

struct ModelSpec {
  std::string name;
  std::string description;
  /// The network sees (grey - input_offset) * input_scale.
  float input_offset = 0.0F;
  float input_scale = 1.0F;
  /// Tile edges must be multiples of this (2^pooling steps).
  int divisor = 1;
  /// Voxels of context each tile gets on every side; covers the receptive field.
  int halo = 0;
  /// Output channel 0 is air, channel k the material materials[k - 1]. `lower` is the grey
  /// value from which a labelled component counted as that material in training; scoring
  /// (scoreMaterials) uses it to assign true classes.
  std::vector<Material> materials;
  /// In order of evaluation; the last layer gives the class scores.
  std::vector<ModelLayer> layers;
};

/// A loaded model. All methods may be called from several threads.
class Model {
 public:
  /// Validates the layer graph and weight sizes.
  explicit Model(ModelSpec spec);

  [[nodiscard]] static Model load(const std::filesystem::path& path);

  [[nodiscard]] const ModelSpec& spec() const { return spec_; }

  /// Class scores (one channel per class) of a grey-value volume whose edges are multiples of
  /// the divisor.
  [[nodiscard]] Tensor forward(std::span<const float> grey,
                               const std::array<std::int64_t, 3>& dims) const;

  /// Class of every voxel (0 = air, else the material id) of a grey-value volume of any size;
  /// it is padded with `fill` up to multiples of the divisor.
  [[nodiscard]] std::vector<std::uint8_t> classify(std::span<const float> grey,
                                                   const std::array<std::int64_t, 3>& dims,
                                                   float fill) const;

 private:
  ModelSpec spec_;
};

/// Writes `spec` as a .vsm file: the bytes "VSMODEL1", the length of the JSON header as
/// little-endian uint64, the JSON header, then the weights and biases of the conv layers in
/// layer order as little-endian float32.
void writeModel(const std::filesystem::path& path, const ModelSpec& spec);

struct ModelSegmentationOptions {
  /// Edge of the tile cores the model runs on, in voxels (each gets the model's halo).
  std::int64_t tile = 128;
};

/// Segments `dataset` with `model` and writes the material volume to `dir` (created, must be
/// empty). Tiles in which no voxel reaches the first material's `lower` are air.
MaterialVolumeInfo segmentMaterialsWithModel(const Dataset& dataset, const Model& model,
                                             const std::filesystem::path& dir,
                                             const ModelSegmentationOptions& options = {});

}  // namespace voxelsieve
