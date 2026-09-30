// Example plugin: a grey value histogram of a dataset, written as histogram.json.
//
// Build it against libvoxelsieve as a shared library and put the .so into the directory given to
// vs-studio --plugins (or VOXELSIEVE_PLUGIN_PATH); see examples/plugins/histogram/CMakeLists.txt.

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>
#include <voxelsieve/dataset.hpp>
#include <voxelsieve/io.hpp>
#include <voxelsieve/operation.hpp>

namespace {

class Histogram final : public voxelsieve::Operation {
 public:
  Histogram() {
    info_.id = "histogram";
    info_.title = "Grey value histogram";
    info_.description = "Counts the grey values of the kept voxels of a dataset level in bins.";
    info_.inputs = {{"dataset", voxelsieve::artifact::kDataset, "Dataset"}};
    info_.outputs = {{"histogram", "table", "histogram.json"}};
    info_.parameters = {
        {"type", "object"},
        {"properties",
         {{"bins", {{"type", "integer"}, {"minimum", 2}, {"maximum", 65536}, {"default", 256}}},
          {"level", {{"type", "integer"}, {"minimum", 0}, {"default", 1}}}}}};
  }
  [[nodiscard]] const voxelsieve::OperationInfo& info() const override { return info_; }

  [[nodiscard]] voxelsieve::OperationResult run(
      const voxelsieve::OperationContext& context) const override {
    const auto bins = context.params.at("bins").get<std::size_t>();
    const auto dataset = voxelsieve::Dataset::open(context.inputs.at("dataset"));
    const int level = std::min(context.params.at("level").get<int>(),
                               static_cast<int>(dataset.info().levels.size()) - 1);
    std::vector<std::uint64_t> counts(bins, 0);
    std::uint64_t total = 0;
    const double scale = static_cast<double>(bins) / 65536.0;
    for (const auto& brick : dataset.level(level).bricks) {
      const auto grid = dataset.brick(level, brick);
      for (auto it = grid->cbeginValueOn(); it; ++it) {
        const double value = std::clamp(static_cast<double>(*it), 0.0, 65535.0);
        ++counts[static_cast<std::size_t>(value * scale)];
        ++total;
      }
    }
    voxelsieve::writeJson(context.output_dir / "histogram.json",
                          {{"level", level}, {"bins", bins}, {"counts", counts}});
    voxelsieve::OperationResult result;
    result.outputs["histogram"] = "histogram.json";
    result.summary = {{"voxels", total}, {"level", level}};
    return result;
  }

 private:
  voxelsieve::OperationInfo info_;
};

}  // namespace

VOXELSIEVE_PLUGIN_EXPORT int voxelsieve_plugin_api_version() {
  return voxelsieve::kPluginApiVersion;
}

VOXELSIEVE_PLUGIN_EXPORT void voxelsieve_register_operations(
    voxelsieve::OperationRegistry& registry) {
  registry.add(std::make_shared<Histogram>());
}
