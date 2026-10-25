#include "voxelsieve/model.hpp"

#include <openvdb/openvdb.h>
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

#include "detail/convolution.hpp"
#include "detail/material_volume.hpp"
#include "detail/transform.hpp"
#include "voxelsieve/io.hpp"
#include "voxelsieve/sieve.hpp"
#include "voxelsieve/vdb.hpp"

namespace voxelsieve {
namespace {

using Index3 = std::array<std::int64_t, 3>;
using Json = nlohmann::json;

constexpr std::string_view kMagic = "VSMODEL1";
constexpr int kFormatVersion = 1;
constexpr int kMaxMaterials = 8;

static_assert(std::endian::native == std::endian::little, "Model files are little-endian");

std::int64_t ceilDiv(std::int64_t a, std::int64_t b) { return (a + b - 1) / b; }

std::size_t weightCount(const ModelLayer& layer) {
  return static_cast<std::size_t>(layer.out_channels) *
         static_cast<std::size_t>(layer.in_channels) *
         static_cast<std::size_t>(layer.kernel * layer.kernel * layer.kernel);
}

}  // namespace

namespace detail {

/// Feature maps that a forward pass no longer needs, kept for the next pass: the tiles of a
/// dataset have the same sizes, and fresh memory costs a page fault and the zeroing of every page
/// by the operating system, on one thread at a time.
class FeaturePool {
 public:
  /// Uninitialised values, in memory of an earlier pass when one at least that large and at most
  /// twice as large is free (tiles at the edges of a dataset are smaller).
  FeatureValues take(std::size_t size) {
    {
      const std::scoped_lock lock(mutex_);
      if (const auto it = free_.lower_bound(size); it != free_.end() && it->first <= 2 * size) {
        FeatureValues values = std::move(it->second);
        bytes_ -= it->first * sizeof(float);
        free_.erase(it);
        values.resize(size);  // within the capacity, without initialising
        return values;
      }
    }
    return FeatureValues(size);
  }

  /// Keeps `values` for a later pass, up to a bound.
  void give(FeatureValues&& values) {
    const std::size_t capacity = values.capacity();
    const std::size_t bytes = capacity * sizeof(float);
    const std::scoped_lock lock(mutex_);
    if (bytes == 0 || bytes_ + bytes > kMaxBytes) {
      return;
    }
    bytes_ += bytes;
    free_.emplace(capacity, std::move(values));
  }

 private:
  static constexpr std::size_t kMaxBytes = std::size_t{2} << 30U;
  std::mutex mutex_;
  std::multimap<std::size_t, FeatureValues> free_;  // by capacity
  std::size_t bytes_ = 0;
};

}  // namespace detail

namespace {

/// The channels of `parts`, one after the other, with a border of `r` zero voxels on every side,
/// so the taps need no bounds checks. Every value is written once, in parallel, so the memory is
/// first touched by many threads.
FeatureValues padTensor(const std::vector<const Tensor*>& parts, std::int64_t r,
                        detail::FeaturePool& pool) {
  const Index3 dims = parts.front()->dims;
  const Index3 padded{dims[0] + 2 * r, dims[1] + 2 * r, dims[2] + 2 * r};
  std::vector<std::pair<const Tensor*, std::int64_t>> channels;  // tensor and its channel
  for (const Tensor* part : parts) {
    for (std::int64_t c = 0; c < part->channels; ++c) {
      channels.emplace_back(part, c);
    }
  }
  const auto count = static_cast<std::int64_t>(channels.size());
  // Uninitialised: every value is written below.
  FeatureValues out =
      pool.take(static_cast<std::size_t>(count * padded[0] * padded[1] * padded[2]));
  tbb::parallel_for(std::int64_t{0}, count * padded[2], [&](std::int64_t task) {
    const std::int64_t i = task / padded[2];
    const std::int64_t pz = task % padded[2];
    const auto [tensor, c] = channels[static_cast<std::size_t>(i)];
    const std::int64_t z = pz - r;
    for (std::int64_t py = 0; py < padded[1]; ++py) {
      float* dst = out.data() + ((i * padded[2] + pz) * padded[1] + py) * padded[0];
      const std::int64_t y = py - r;
      if (z < 0 || z >= dims[2] || y < 0 || y >= dims[1]) {
        std::fill_n(dst, padded[0], 0.0F);
        continue;
      }
      std::fill_n(dst, r, 0.0F);
      std::copy_n(tensor->data.data() + ((c * dims[2] + z) * dims[1] + y) * dims[0], dims[0],
                  dst + r);
      std::fill_n(dst + r + dims[0], r, 0.0F);
    }
  });
  return out;
}

/// "Same" 3D convolution. Each task computes one z plane of up to four output channels, row by
/// row (detail/convolution), so every input value that is loaded feeds four accumulations.
Tensor convolve(const std::vector<const Tensor*>& in, const ModelLayer& layer,
                detail::FeaturePool& pool) {
  constexpr int kBlock = detail::kConvolutionBlock;
  const Index3 dims = in.front()->dims;
  const std::int64_t nx = dims[0];
  const std::int64_t ny = dims[1];
  const std::int64_t nz = dims[2];
  const std::int64_t plane = nx * ny;
  Tensor out{layer.out_channels, dims,
             pool.take(static_cast<std::size_t>(layer.out_channels * nx * ny * nz))};
  const int k = layer.kernel;
  const std::int64_t r = k / 2;
  const std::int64_t k3 = static_cast<std::int64_t>(k) * k * k;
  FeatureValues padded = padTensor(in, r, pool);
  const std::int64_t row = nx + 2 * r;
  const std::int64_t padded_plane = row * (ny + 2 * r);
  const std::int64_t channel_stride = padded_plane * (nz + 2 * r);
  const std::int64_t blocks = ceilDiv(layer.out_channels, kBlock);
  // Weights of each block of four output channels, interleaved per tap.
  const std::int64_t taps = layer.in_channels * k3;
  std::vector<float> packed(static_cast<std::size_t>(blocks * taps * kBlock), 0.0F);
  for (std::int64_t o = 0; o < layer.out_channels; ++o) {
    for (std::int64_t t = 0; t < taps; ++t) {
      packed[static_cast<std::size_t>(((o / kBlock) * taps + t) * kBlock + o % kBlock)] =
          layer.weights[static_cast<std::size_t>(o * taps + t)];
    }
  }
  tbb::parallel_for(
      tbb::blocked_range<std::int64_t>(0, blocks * nz),
      [&](const tbb::blocked_range<std::int64_t>& range) {
        for (std::int64_t task = range.begin(); task != range.end(); ++task) {
          const std::int64_t block = task / nz;
          const std::int64_t o0 = block * kBlock;
          const std::int64_t z = task % nz;
          detail::ConvolutionRow conv;
          conv.in_channels = layer.in_channels;
          conv.channel_stride = channel_stride;
          conv.plane = padded_plane;
          conv.row = row;
          conv.kernel = k;
          conv.weights = packed.data() + block * taps * kBlock;
          conv.count = static_cast<int>(std::min<std::int64_t>(kBlock, layer.out_channels - o0));
          conv.relu = layer.relu;
          conv.width = nx;
          for (int b = 0; b < conv.count; ++b) {
            conv.bias[static_cast<std::size_t>(b)] = layer.bias[static_cast<std::size_t>(o0 + b)];
          }
          for (std::int64_t y = 0; y < ny; ++y) {
            conv.src = padded.data() + z * padded_plane + y * row;
            for (int b = 0; b < conv.count; ++b) {
              conv.dst[static_cast<std::size_t>(b)] =
                  out.data.data() + ((o0 + b) * nz + z) * plane + y * nx;
            }
            detail::convolveRow(conv);
          }
        }
      });
  pool.give(std::move(padded));
  return out;
}

Tensor maxPool(const Tensor& in, detail::FeaturePool& pool) {
  const Index3 dims{in.dims[0] / 2, in.dims[1] / 2, in.dims[2] / 2};
  Tensor out{in.channels, dims,
             pool.take(static_cast<std::size_t>(in.channels * dims[0] * dims[1] * dims[2]))};
  tbb::parallel_for(std::int64_t{0}, in.channels * dims[2], [&](std::int64_t task) {
    const std::int64_t c = task / dims[2];
    const std::int64_t z = task % dims[2];
    for (std::int64_t y = 0; y < dims[1]; ++y) {
      for (std::int64_t x = 0; x < dims[0]; ++x) {
        float best = -std::numeric_limits<float>::infinity();
        for (std::int64_t dz = 0; dz < 2; ++dz) {
          for (std::int64_t dy = 0; dy < 2; ++dy) {
            const std::int64_t base =
                ((c * in.dims[2] + 2 * z + dz) * in.dims[1] + 2 * y + dy) * in.dims[0] + 2 * x;
            best = std::max({best, in.data[static_cast<std::size_t>(base)],
                             in.data[static_cast<std::size_t>(base + 1)]});
          }
        }
        out.data[static_cast<std::size_t>(((c * dims[2] + z) * dims[1] + y) * dims[0] + x)] = best;
      }
    }
  });
  return out;
}

Tensor upsample(const Tensor& in, detail::FeaturePool& pool) {
  const Index3 dims{in.dims[0] * 2, in.dims[1] * 2, in.dims[2] * 2};
  Tensor out{in.channels, dims,
             pool.take(static_cast<std::size_t>(in.channels * dims[0] * dims[1] * dims[2]))};
  tbb::parallel_for(std::int64_t{0}, in.channels * dims[2], [&](std::int64_t task) {
    const std::int64_t c = task / dims[2];
    const std::int64_t z = task % dims[2];
    for (std::int64_t y = 0; y < dims[1]; ++y) {
      const float* src =
          in.data.data() + ((c * in.dims[2] + z / 2) * in.dims[1] + y / 2) * in.dims[0];
      float* dst = out.data.data() + ((c * dims[2] + z) * dims[1] + y) * dims[0];
      for (std::int64_t x = 0; x < dims[0]; ++x) {
        dst[x] = src[x / 2];
      }
    }
  });
  return out;
}

Tensor concat(const std::vector<const Tensor*>& inputs) {
  Tensor out{0, inputs.front()->dims, {}};
  for (const Tensor* input : inputs) {
    if (input->dims != out.dims) {
      throw std::invalid_argument("concat inputs differ in size");
    }
    out.channels += input->channels;
    out.data.insert(out.data.end(), input->data.begin(), input->data.end());
  }
  return out;
}

constexpr const char* kSegmentationTask = "segmentation";
constexpr const char* kDenoisingTask = "denoising";

ModelTask parseTask(const std::string& name) {
  if (name == kSegmentationTask) {
    return ModelTask::kSegmentation;
  }
  if (name == kDenoisingTask) {
    return ModelTask::kDenoising;
  }
  throw std::runtime_error("Unknown model task: " + name);
}

/// `grey` (dims, x fastest) copied into a volume of `padded` dims, filled up with `fill`.
std::vector<float> padInput(std::span<const float> grey, const Index3& dims, const Index3& padded,
                            float fill) {
  std::vector<float> input(static_cast<std::size_t>(padded[0] * padded[1] * padded[2]), fill);
  for (std::int64_t z = 0; z < dims[2]; ++z) {
    for (std::int64_t y = 0; y < dims[1]; ++y) {
      std::copy_n(grey.begin() + (z * dims[1] + y) * dims[0], dims[0],
                  input.begin() + (z * padded[1] + y) * padded[0]);
    }
  }
  return input;
}

Material materialFromJson(const Json& json) {
  Material material;
  material.id = json.at("id").get<int>();
  material.name = json.at("name").get<std::string>();
  material.color = json.contains("color") ? json.at("color").get<std::array<std::uint8_t, 3>>()
                                          : detail::materialColor(material.id);
  material.lower = json.at("lower").get<float>();
  return material;
}

}  // namespace

Model::Model(ModelSpec spec)
    : spec_(std::move(spec)), pool_(std::make_shared<detail::FeaturePool>()) {
  const bool segmentation = spec_.task == ModelTask::kSegmentation;
  if (segmentation && (spec_.materials.empty() || spec_.materials.size() > kMaxMaterials)) {
    throw std::invalid_argument("A segmentation model needs 1 to 8 materials");
  }
  if (!segmentation && !spec_.materials.empty()) {
    throw std::invalid_argument("A denoising model has no materials");
  }
  for (std::size_t m = 0; m < spec_.materials.size(); ++m) {
    if (spec_.materials[m].id != static_cast<int>(m) + 1) {
      throw std::invalid_argument("Model materials must have the ids 1, 2, ... in order");
    }
  }
  if (spec_.divisor < 1 || spec_.halo < 0 || spec_.input_scale == 0.0F) {
    throw std::invalid_argument("Invalid model divisor, halo or input scale");
  }
  if (spec_.layers.empty()) {
    throw std::invalid_argument("A model needs layers");
  }
  // Channels and resolution (pooling steps) of every named output.
  std::map<std::string, std::pair<int, int>> shapes{{"input", {1, 0}}};
  int deepest = 0;
  for (const ModelLayer& layer : spec_.layers) {
    if (layer.name.empty() || shapes.contains(layer.name)) {
      throw std::invalid_argument("Model layer names must be unique and not empty");
    }
    std::vector<std::pair<int, int>> in;
    for (const std::string& input : layer.inputs) {
      const auto it = shapes.find(input);
      if (it == shapes.end()) {
        throw std::invalid_argument("Layer " + layer.name + " reads unknown input " + input);
      }
      in.push_back(it->second);
    }
    const bool single = in.size() == 1;
    std::pair<int, int> shape;
    if (layer.op == "conv") {
      if (!single || layer.in_channels != in[0].first || layer.out_channels < 1 ||
          layer.kernel < 1 || layer.kernel % 2 == 0 || layer.weights.size() != weightCount(layer) ||
          layer.bias.size() != static_cast<std::size_t>(layer.out_channels)) {
        throw std::invalid_argument("Invalid conv layer " + layer.name);
      }
      shape = {layer.out_channels, in[0].second};
    } else if (layer.op == "maxpool" || layer.op == "upsample") {
      const int step = layer.op == "maxpool" ? 1 : -1;
      if (!single || in[0].second + step < 0) {
        throw std::invalid_argument("Invalid " + layer.op + " layer " + layer.name);
      }
      shape = {in[0].first, in[0].second + step};
    } else if (layer.op == "concat") {
      if (in.empty()) {
        throw std::invalid_argument("concat layer " + layer.name + " has no inputs");
      }
      shape = {0, in[0].second};
      for (const auto& [channels, level] : in) {
        if (level != shape.second) {
          throw std::invalid_argument("concat layer " + layer.name + " mixes resolutions");
        }
        shape.first += channels;
      }
    } else {
      throw std::invalid_argument("Unknown model layer type: " + layer.op);
    }
    deepest = std::max(deepest, shape.second);
    shapes[layer.name] = shape;
  }
  const auto& [channels, level] = shapes.at(spec_.layers.back().name);
  if (segmentation && (level != 0 || channels != static_cast<int>(spec_.materials.size()) + 1)) {
    throw std::invalid_argument(
        "The last model layer must give one score per class at full resolution");
  }
  if (!segmentation && (level != 0 || channels != 1)) {
    throw std::invalid_argument(
        "The last layer of a denoising model must give one channel at full resolution");
  }
  if (spec_.divisor % (1 << deepest) != 0) {
    throw std::invalid_argument("The model divisor must be a multiple of 2^pooling steps");
  }
}

Model Model::load(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("Cannot open model " + path.string());
  }
  std::string magic(kMagic.size(), '\0');
  std::uint64_t header_size = 0;
  in.read(magic.data(), static_cast<std::streamsize>(magic.size()));
  in.read(reinterpret_cast<char*>(&header_size), sizeof(header_size));
  if (!in || magic != kMagic || header_size > (std::uint64_t{1} << 26U)) {
    throw std::runtime_error("Not a VoxelSieve model: " + path.string());
  }
  std::string header(header_size, '\0');
  in.read(header.data(), static_cast<std::streamsize>(header_size));
  const Json json = Json::parse(header);
  if (json.at("format") != "voxelsieve-model" || json.at("version") != kFormatVersion) {
    throw std::runtime_error("Unsupported model format in " + path.string());
  }
  ModelSpec spec;
  spec.task = parseTask(json.value("task", std::string(kSegmentationTask)));
  spec.name = json.at("name").get<std::string>();
  spec.description = json.value("description", std::string());
  spec.input_offset = json.at("input").at("offset").get<float>();
  spec.input_scale = json.at("input").at("scale").get<float>();
  spec.divisor = json.at("divisor").get<int>();
  spec.halo = json.at("halo").get<int>();
  for (const Json& material : json.value("materials", Json::array())) {
    spec.materials.push_back(materialFromJson(material));
  }
  const auto read_floats = [&](std::vector<float>& values, std::size_t count) {
    values.resize(count);
    in.read(reinterpret_cast<char*>(values.data()),
            static_cast<std::streamsize>(count * sizeof(float)));
    if (!in) {
      throw std::runtime_error("Model weights are truncated in " + path.string());
    }
  };
  for (const Json& entry : json.at("layers")) {
    ModelLayer layer;
    layer.op = entry.at("op").get<std::string>();
    layer.name = entry.at("name").get<std::string>();
    layer.inputs = entry.at("inputs").get<std::vector<std::string>>();
    if (layer.op == "conv") {
      layer.in_channels = entry.at("in").get<int>();
      layer.out_channels = entry.at("out").get<int>();
      layer.kernel = entry.at("kernel").get<int>();
      layer.relu = entry.value("relu", false);
      if (layer.in_channels < 1 || layer.out_channels < 1 || layer.kernel < 1 ||
          layer.kernel > 15) {
        throw std::runtime_error("Invalid conv layer " + layer.name + " in " + path.string());
      }
      read_floats(layer.weights, weightCount(layer));
      read_floats(layer.bias, static_cast<std::size_t>(layer.out_channels));
    }
    spec.layers.push_back(std::move(layer));
  }
  if (in.peek() != std::char_traits<char>::eof()) {
    throw std::runtime_error("Model has more weights than its layers use: " + path.string());
  }
  return Model(std::move(spec));
}

void writeModel(const std::filesystem::path& path, const ModelSpec& spec) {
  const Model check(spec);  // refuse to write what cannot be loaded
  Json materials = Json::array();
  for (const Material& material : spec.materials) {
    materials.push_back({{"id", material.id},
                         {"name", material.name},
                         {"color", material.color},
                         {"lower", material.lower}});
  }
  Json layers = Json::array();
  for (const ModelLayer& layer : spec.layers) {
    Json entry = {{"op", layer.op}, {"name", layer.name}, {"inputs", layer.inputs}};
    if (layer.op == "conv") {
      entry.update({{"in", layer.in_channels},
                    {"out", layer.out_channels},
                    {"kernel", layer.kernel},
                    {"relu", layer.relu}});
    }
    layers.push_back(entry);
  }
  const std::string header = Json{
      {"format", "voxelsieve-model"},
      {"version", kFormatVersion},
      {"task", spec.task == ModelTask::kSegmentation ? kSegmentationTask : kDenoisingTask},
      {"name", spec.name},
      {"description", spec.description},
      {"input", {{"offset", spec.input_offset}, {"scale", spec.input_scale}}},
      {"divisor", spec.divisor},
      {"halo", spec.halo},
      {"materials", materials},
      {"layers", layers}}.dump();
  std::ofstream out(path, std::ios::binary);
  const std::uint64_t header_size = header.size();
  out.write(kMagic.data(), static_cast<std::streamsize>(kMagic.size()));
  out.write(reinterpret_cast<const char*>(&header_size), sizeof(header_size));
  out.write(header.data(), static_cast<std::streamsize>(header.size()));
  for (const ModelLayer& layer : spec.layers) {
    if (layer.op == "conv") {
      out.write(reinterpret_cast<const char*>(layer.weights.data()),
                static_cast<std::streamsize>(layer.weights.size() * sizeof(float)));
      out.write(reinterpret_cast<const char*>(layer.bias.data()),
                static_cast<std::streamsize>(layer.bias.size() * sizeof(float)));
    }
  }
  if (!out) {
    throw std::runtime_error("Cannot write model " + path.string());
  }
}

Tensor Model::forward(std::span<const float> grey, const std::array<std::int64_t, 3>& dims) const {
  for (const std::int64_t d : dims) {
    if (d < 1 || d % spec_.divisor != 0) {
      throw std::invalid_argument("Model input edges must be positive multiples of the divisor");
    }
  }
  if (static_cast<std::int64_t>(grey.size()) != dims[0] * dims[1] * dims[2]) {
    throw std::invalid_argument("Model input size does not match its dimensions");
  }
  // Concatenations read only by convolutions are not copied: the convolution reads their parts.
  std::set<std::string> joined;
  for (std::size_t l = 0; l + 1 < spec_.layers.size(); ++l) {
    if (spec_.layers[l].op == "concat") {
      joined.insert(spec_.layers[l].name);
    }
  }
  for (const ModelLayer& layer : spec_.layers) {
    if (layer.op != "conv") {
      for (const std::string& input : layer.inputs) {
        joined.erase(input);
      }
    }
  }
  // The layer after which each output is no longer needed, so feature maps are freed early. The
  // parts of a concatenation that is not copied live as long as it is read.
  std::map<std::string, std::size_t> last_use;
  for (std::size_t l = 0; l < spec_.layers.size(); ++l) {
    for (const std::string& input : spec_.layers[l].inputs) {
      last_use[input] = l;
    }
  }
  for (const ModelLayer& layer : spec_.layers) {
    if (joined.contains(layer.name)) {
      for (const std::string& part : layer.inputs) {
        last_use[part] = std::max(last_use[part], last_use.at(layer.name));
      }
    }
  }
  std::map<std::string, Tensor> tensors;
  std::map<std::string, std::vector<const Tensor*>> parts;  // of the joined concatenations
  Tensor& input = tensors["input"];
  input = Tensor{1, dims, pool_->take(grey.size())};
  std::transform(grey.begin(), grey.end(), input.data.begin(),
                 [&](float value) { return (value - spec_.input_offset) * spec_.input_scale; });
  const auto inputs_of = [&](const ModelLayer& layer) {
    std::vector<const Tensor*> in;
    for (const std::string& name : layer.inputs) {
      if (const auto it = parts.find(name); it != parts.end()) {
        in.insert(in.end(), it->second.begin(), it->second.end());
      } else {
        in.push_back(&tensors.at(name));
      }
    }
    return in;
  };
  for (std::size_t l = 0; l < spec_.layers.size(); ++l) {
    const ModelLayer& layer = spec_.layers[l];
    const std::vector<const Tensor*> in = inputs_of(layer);
    if (joined.contains(layer.name)) {
      for (const Tensor* part : in) {
        if (part->dims != in.front()->dims) {
          throw std::invalid_argument("concat inputs differ in size");
        }
      }
      parts[layer.name] = in;
    } else {
      Tensor out;
      if (layer.op == "conv") {
        out = convolve(in, layer, *pool_);
      } else if (layer.op == "maxpool") {
        out = maxPool(*in[0], *pool_);
      } else if (layer.op == "upsample") {
        out = upsample(*in[0], *pool_);
      } else {
        out = concat(in);
      }
      tensors[layer.name] = std::move(out);
    }
    for (const auto& [name, last] : last_use) {
      if (last == l) {
        if (const auto it = tensors.find(name); it != tensors.end()) {
          pool_->give(std::move(it->second.data));
          tensors.erase(it);
        }
        parts.erase(name);
      }
    }
  }
  return std::move(tensors.at(spec_.layers.back().name));
}

std::vector<std::uint8_t> Model::classify(std::span<const float> grey,
                                          const std::array<std::int64_t, 3>& dims,
                                          float fill) const {
  if (spec_.task != ModelTask::kSegmentation) {
    throw std::invalid_argument("Model " + spec_.name + " is not a segmentation model");
  }
  if (static_cast<std::int64_t>(grey.size()) != dims[0] * dims[1] * dims[2]) {
    throw std::invalid_argument("Model input size does not match its dimensions");
  }
  const std::int64_t d = spec_.divisor;
  const Index3 padded{ceilDiv(dims[0], d) * d, ceilDiv(dims[1], d) * d, ceilDiv(dims[2], d) * d};
  Tensor scores = forward(padInput(grey, dims, padded, fill), padded);
  const std::int64_t n = padded[0] * padded[1] * padded[2];
  std::vector<std::uint8_t> classes(grey.size());
  tbb::parallel_for(std::int64_t{0}, dims[2], [&](std::int64_t z) {
    for (std::int64_t y = 0; y < dims[1]; ++y) {
      for (std::int64_t x = 0; x < dims[0]; ++x) {
        const std::int64_t v = (z * padded[1] + y) * padded[0] + x;
        int best = 0;
        for (int c = 1; c < scores.channels; ++c) {
          if (scores.data[static_cast<std::size_t>(c * n + v)] >
              scores.data[static_cast<std::size_t>(best * n + v)]) {
            best = c;
          }
        }
        classes[static_cast<std::size_t>((z * dims[1] + y) * dims[0] + x)] =
            static_cast<std::uint8_t>(best);
      }
    }
  });
  pool_->give(std::move(scores.data));
  return classes;
}

std::vector<float> Model::denoise(std::span<const float> grey,
                                  const std::array<std::int64_t, 3>& dims, float fill) const {
  if (spec_.task != ModelTask::kDenoising) {
    throw std::invalid_argument("Model " + spec_.name + " is not a denoising model");
  }
  if (static_cast<std::int64_t>(grey.size()) != dims[0] * dims[1] * dims[2]) {
    throw std::invalid_argument("Model input size does not match its dimensions");
  }
  const std::int64_t d = spec_.divisor;
  const Index3 padded{ceilDiv(dims[0], d) * d, ceilDiv(dims[1], d) * d, ceilDiv(dims[2], d) * d};
  Tensor out = forward(padInput(grey, dims, padded, fill), padded);
  std::vector<float> denoised(grey.size());
  const float scale = 1.0F / spec_.input_scale;
  for (std::int64_t z = 0; z < dims[2]; ++z) {
    for (std::int64_t y = 0; y < dims[1]; ++y) {
      const float* row = out.data.data() + (z * padded[1] + y) * padded[0];
      float* target = denoised.data() + (z * dims[1] + y) * dims[0];
      for (std::int64_t x = 0; x < dims[0]; ++x) {
        target[x] = row[x] * scale + spec_.input_offset;
      }
    }
  }
  pool_->give(std::move(out.data));
  return denoised;
}

MaterialVolumeInfo segmentMaterialsWithModel(const Dataset& dataset, const Model& model,
                                             const std::filesystem::path& dir,
                                             const ModelSegmentationOptions& options) {
  if (options.tile < 1) {
    throw std::invalid_argument("The tile edge must be positive");
  }
  if (std::filesystem::exists(dir) && !std::filesystem::is_empty(dir)) {
    throw std::invalid_argument("Output directory is not empty: " + dir.string());
  }
  if (model.spec().task != ModelTask::kSegmentation) {
    throw std::invalid_argument("Model " + model.spec().name + " is not a segmentation model");
  }
  initializeVdb();
  const DatasetInfo& data = dataset.info();
  const ModelSpec& spec = model.spec();
  MaterialVolumeInfo info;
  info.dims = data.dims;
  info.voxel_size = data.voxel_size;
  info.brick_size = data.brick_size;
  info.air_level = data.air_level;
  info.air_threshold = spec.materials.front().lower;
  info.materials = spec.materials;
  info.model = spec.name;
  std::vector<std::int64_t> counts(info.materials.size() + 1, 0);

  std::filesystem::create_directories(dir / "level0");
  const Index3 brick_dims{ceilDiv(info.dims[0], info.brick_size),
                          ceilDiv(info.dims[1], info.brick_size),
                          ceilDiv(info.dims[2], info.brick_size)};
  const std::int64_t halo = spec.halo;
  // Bricks and their tiles one after another; the layers run in parallel inside a tile.
  for (std::int64_t bz = 0; bz < brick_dims[2]; ++bz) {
    for (std::int64_t by = 0; by < brick_dims[1]; ++by) {
      for (std::int64_t bx = 0; bx < brick_dims[0]; ++bx) {
        const Index3 brick{bx, by, bz};
        if (!dataset.hasBrick(0, brick)) {
          continue;
        }
        const Box brick_box = dataset.brickBox(0, brick);
        auto grid = openvdb::Int32Grid::create(0);
        auto accessor = grid->getAccessor();
        for (std::int64_t tz = brick_box.min[2]; tz < brick_box.max[2]; tz += options.tile) {
          for (std::int64_t ty = brick_box.min[1]; ty < brick_box.max[1]; ty += options.tile) {
            for (std::int64_t tx = brick_box.min[0]; tx < brick_box.max[0]; tx += options.tile) {
              const Box core{{tx, ty, tz},
                             {std::min(tx + options.tile, brick_box.max[0]),
                              std::min(ty + options.tile, brick_box.max[1]),
                              std::min(tz + options.tile, brick_box.max[2])}};
              Box outer;
              for (std::size_t a = 0; a < 3; ++a) {
                outer.min[a] = std::max<std::int64_t>(core.min[a] - halo, 0);
                outer.max[a] = std::min<std::int64_t>(core.max[a] + halo, info.dims[a]);
              }
              std::vector<float> grey(static_cast<std::size_t>(outer.voxelCount()));
              dataset.readRegion(0, outer, grey, info.air_level);
              if (*std::max_element(grey.begin(), grey.end()) < info.air_threshold) {
                continue;
              }
              const Index3 size{outer.size(0), outer.size(1), outer.size(2)};
              const std::vector<std::uint8_t> classes = model.classify(grey, size, info.air_level);
              for (std::int64_t z = core.min[2]; z < core.max[2]; ++z) {
                for (std::int64_t y = core.min[1]; y < core.max[1]; ++y) {
                  for (std::int64_t x = core.min[0]; x < core.max[0]; ++x) {
                    const std::uint8_t id = classes[static_cast<std::size_t>(
                        (x - outer.min[0]) +
                        size[0] * ((y - outer.min[1]) + size[1] * (z - outer.min[2])))];
                    if (id == 0) {
                      continue;
                    }
                    ++counts[id];
                    accessor.setValue(openvdb::Coord(static_cast<int>(x), static_cast<int>(y),
                                                     static_cast<int>(z)),
                                      id);
                  }
                }
              }
            }
          }
        }
        if (grid->activeVoxelCount() == 0) {
          continue;
        }
        grid->setName("material");
        grid->setTransform(detail::voxelTransform(info.voxel_size, 0));
        writeVdb(detail::materialBrickFile(dir, brick), {grid});
        info.bricks.push_back(brick);
      }
    }
  }
  std::sort(info.bricks.begin(), info.bricks.end());
  const double voxel_mm3 = info.voxel_size.volumeMm3();
  for (Material& material : info.materials) {
    material.voxel_count = counts[static_cast<std::size_t>(material.id)];
    material.volume_mm3 = static_cast<double>(material.voxel_count) * voxel_mm3;
  }
  detail::writeMaterialVolumeInfo(dir, info);
  return info;
}

}  // namespace voxelsieve
