// Python bindings of VoxelSieve (module voxelsieve._core, ADR 0031). Kept thin: the Python
// package (python/voxelsieve) turns the JSON strings into dicts, the voxel boxes into numpy order
// and adds slicing. Arrays cross the boundary without copies where the layout allows: VoxelSieve's
// x-fastest volumes are C-ordered numpy arrays of shape (z, y, x).

#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/array.h>
#include <nanobind/stl/filesystem.h>
#include <nanobind/stl/function.h>
#include <nanobind/stl/map.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/pair.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/unique_ptr.h>
#include <nanobind/stl/vector.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "voxelsieve/dataset.hpp"
#include "voxelsieve/materials.hpp"
#include "voxelsieve/operation.hpp"
#include "voxelsieve/phantom.hpp"
#include "voxelsieve/project.hpp"
#include "voxelsieve/resources.hpp"
#include "voxelsieve/source.hpp"
#include "voxelsieve/vdb.hpp"
#include "voxelsieve/voxel_size.hpp"

namespace nb = nanobind;

namespace voxelsieve {
namespace {

using Json = nlohmann::json;
using Index3 = std::array<std::int64_t, 3>;

/// A C-ordered (z, y, x) array that owns `values`.
template <class T>
nb::ndarray<nb::numpy, T, nb::ndim<3>> toArray(std::vector<T> values, const Box& box) {
  auto* owned = new std::vector<T>(std::move(values));
  const nb::capsule owner(owned, [](void* pointer) noexcept {
    delete static_cast<std::vector<T>*>(pointer);  // NOLINT(cppcoreguidelines-owning-memory)
  });
  return nb::ndarray<nb::numpy, T, nb::ndim<3>>(
      owned->data(),
      {static_cast<std::size_t>(box.size(2)), static_cast<std::size_t>(box.size(1)),
       static_cast<std::size_t>(box.size(0))},
      owner);
}

Box makeBox(const Index3& min, const Index3& max) {
  Box box{min, max};
  for (std::size_t axis = 0; axis < 3; ++axis) {
    if (box.max[axis] < box.min[axis]) {
      throw std::invalid_argument("Box max is smaller than min");
    }
  }
  return box;
}

void checkInside(const Box& box, const Index3& dims) {
  for (std::size_t axis = 0; axis < 3; ++axis) {
    if (box.min[axis] < 0 || box.max[axis] > dims[axis]) {
      throw nb::index_error("Box lies outside the volume");
    }
  }
}

Json voxelSizeJson(const VoxelSize& size) {
  return {{"pitch_mm", size.pitch_mm}, {"slice_thickness_mm", size.slice_thickness_mm}};
}

VoxelSize voxelSizeFrom(const std::array<double, 3>& pitch_mm, double slice_thickness_mm) {
  VoxelSize size(pitch_mm[0], pitch_mm[1], pitch_mm[2]);
  size.slice_thickness_mm = slice_thickness_mm;
  size.validate();
  return size;
}

Json datasetInfoJson(const DatasetInfo& info) {
  Json levels = Json::array();
  for (const LevelInfo& level : info.levels) {
    levels.push_back({{"level", level.level},
                      {"dims", level.dims},
                      {"voxel_size", voxelSizeJson(level.voxel_size)},
                      {"brick_count", level.bricks.size()}});
  }
  return {{"dims", info.dims},
          {"voxel_size", voxelSizeJson(info.voxel_size)},
          {"brick_size", info.brick_size},
          {"value_type", std::string(valueTypeName(info.value_type))},
          {"threshold", info.threshold},
          {"air_level", info.air_level},
          {"margin_voxels", info.margin_voxels},
          {"min_material_voxels", info.min_material_voxels},
          {"active_voxel_count", info.active_voxel_count},
          {"value_mapping",
           {{"offset", info.value_mapping.offset}, {"scale", info.value_mapping.scale}}},
          {"levels", levels}};
}

/// A volume in a numpy array (or numpy.memmap, which keeps a large raw file out of memory) as a
/// source of `writeDataset`. The array is C-ordered (z, y, x) and must outlive the source.
template <class T>
class ArraySource final : public VolumeSource {
 public:
  ArraySource(const T* data, const Index3& dims, const VoxelSize& voxel_size,
              const ValueMapping& mapping)
      : data_(data), dims_(dims), voxel_size_(voxel_size), mapping_(mapping) {}

  [[nodiscard]] Index3 dims() const override { return dims_; }
  [[nodiscard]] VoxelSize voxelSize() const override { return voxel_size_; }
  [[nodiscard]] ValueMapping valueMapping() const override { return mapping_; }
  [[nodiscard]] SampleType sampleType() const override {
    return sizeof(T) == 1 ? SampleType::kUInt8 : SampleType::kUInt16;
  }
  void readRegion(const Box& box, std::span<std::uint16_t> out) const override {
    std::size_t index = 0;
    for (std::int64_t z = box.min[2]; z < box.max[2]; ++z) {
      for (std::int64_t y = box.min[1]; y < box.max[1]; ++y) {
        const T* row = data_ + static_cast<std::size_t>((z * dims_[1] + y) * dims_[0]);
        for (std::int64_t x = box.min[0]; x < box.max[0]; ++x) {
          out[index++] = row[x];
        }
      }
    }
  }

 private:
  const T* data_;
  Index3 dims_;
  VoxelSize voxel_size_;
  ValueMapping mapping_;
};

template <class T>
using VolumeArray = nb::ndarray<const T, nb::ndim<3>, nb::c_contig, nb::device::cpu>;

struct WriteOptions {
  std::array<double, 3> pitch_mm{1.0, 1.0, 1.0};
  double slice_thickness_mm = 0.0;
  std::optional<float> threshold;
  int margin_voxels = 3;
  std::int64_t brick_size = 256;
  int min_material_voxels = 1;
  std::optional<std::string> value_type;
  double mapping_offset = 0.0;
  double mapping_scale = 1.0;
};

template <class T>
std::string writeArray(const VolumeArray<T>& array, const std::filesystem::path& dir,
                       const WriteOptions& write) {
  const Index3 dims{static_cast<std::int64_t>(array.shape(2)),
                    static_cast<std::int64_t>(array.shape(1)),
                    static_cast<std::int64_t>(array.shape(0))};
  const ArraySource<T> source(array.data(), dims,
                              voxelSizeFrom(write.pitch_mm, write.slice_thickness_mm),
                              ValueMapping{write.mapping_offset, write.mapping_scale});
  DatasetOptions options;
  options.threshold = write.threshold;
  options.margin_voxels = write.margin_voxels;
  options.brick_size = write.brick_size;
  options.min_material_voxels = write.min_material_voxels;
  // The array is in memory or memory-mapped: random access is cheap, no staging copy.
  options.stage_slow_sources = false;
  if (write.value_type) {
    options.value_type = parseValueType(*write.value_type);
  }
  DatasetInfo info;
  {
    const nb::gil_scoped_release release;
    info = writeDataset(source, dir, options);
  }
  return datasetInfoJson(info).dump();
}

std::vector<float> readBox(const Dataset& dataset, int level, const Box& box, float fill) {
  checkInside(box, dataset.level(level).dims);
  std::vector<float> values(static_cast<std::size_t>(box.voxelCount()));
  const nb::gil_scoped_release release;
  dataset.readRegion(level, box, values, fill);
  return values;
}

void checkLevel(const Dataset& dataset, int level) {
  if (level < 0 || static_cast<std::size_t>(level) >= dataset.info().levels.size()) {
    throw nb::index_error("No such level");
  }
}

/// Inputs of a step as {port: [step, output]}.
std::map<std::string, ArtifactRef> artifactRefs(
    const std::map<std::string, std::pair<int, std::string>>& inputs) {
  std::map<std::string, ArtifactRef> refs;
  for (const auto& [port, ref] : inputs) {
    refs[port] = ArtifactRef{ref.first, ref.second};
  }
  return refs;
}

Json stepJson(const Project& project, const Step& step) {
  Json outputs = Json::object();
  for (const auto& [port, path] : step.outputs) {
    outputs[port] = {{"path", project.resolve(ArtifactRef{step.id, port}).string()},
                     {"type", step.output_types.contains(port) ? step.output_types.at(port) : ""}};
  }
  Json inputs = Json::object();
  for (const auto& [port, ref] : step.inputs) {
    inputs[port] = {{"step", ref.step}, {"output", ref.output}};
  }
  return {{"id", step.id},
          {"operation", step.operation},
          {"title", step.title},
          {"params", step.params},
          {"inputs", inputs},
          {"outputs", outputs},
          {"status", step.status},
          {"started", step.started},
          {"finished", step.finished},
          {"messages", step.messages},
          {"summary", step.summary},
          {"object", step.object},
          {"telemetry", step.telemetry}};
}

/// A project and the operations it runs: the built-in ones and those of loaded plugins.
class PyProject {
 public:
  explicit PyProject(Project project) : project_(std::move(project)) {
    registerBuiltinOperations(registry_);
  }

  Project& project() { return project_; }
  OperationRegistry& registry() { return registry_; }

  std::string run(const std::string& operation, const std::string& params_json,
                  const std::map<std::string, std::pair<int, std::string>>& inputs,
                  const std::string& object, const std::string& name,
                  const std::function<void(double)>& progress) {
    const Json params = Json::parse(params_json);
    const auto refs = artifactRefs(inputs);
    StepTarget target;
    target.object = object;
    target.name = name;
    std::function<void(double)> report;
    if (progress) {
      report = [&progress](double fraction) {
        const nb::gil_scoped_acquire acquire;
        progress(fraction);
      };
    }
    int id = 0;
    {
      const nb::gil_scoped_release release;
      id = project_.run(registry_, operation, params, refs, report, nullptr, target).id;
    }
    return stepJson(project_, stepById(id)).dump();
  }

  [[nodiscard]] const Step& stepById(int id) const {
    for (const Step& step : project_.steps()) {
      if (step.id == id) {
        return step;
      }
    }
    throw nb::index_error("No such step");
  }

  [[nodiscard]] std::string steps() const {
    Json steps = Json::array();
    for (const Step& step : project_.steps()) {
      steps.push_back(stepJson(project_, step));
    }
    return steps.dump();
  }

  [[nodiscard]] std::string objects() const {
    Json objects = Json::array();
    for (const ProjectObject& object : project_.objects()) {
      objects.push_back(toJson(object));
    }
    return objects.dump();
  }

  [[nodiscard]] std::string results(const std::string& object) const {
    Json results = Json::array();
    for (const AnalysisResult& result : project_.results(object)) {
      Json entry = toJson(result);
      entry["path"] = project_.resolve(result.ref).string();
      results.push_back(entry);
    }
    return results.dump();
  }

 private:
  Project project_;
  OperationRegistry registry_;
};

std::string operationsJson(const OperationRegistry& registry) {
  Json operations = Json::array();
  for (const auto& operation : registry.all()) {
    const OperationInfo& info = operation->info();
    const auto ports = [](const std::vector<PortInfo>& list) {
      Json result = Json::array();
      for (const PortInfo& port : list) {
        result.push_back({{"name", port.name},
                          {"type", port.type},
                          {"description", port.description},
                          {"optional", port.optional}});
      }
      return result;
    };
    operations.push_back({{"id", info.id},
                          {"title", info.title},
                          {"description", info.description},
                          {"inputs", ports(info.inputs)},
                          {"outputs", ports(info.outputs)},
                          {"parameters", info.parameters}});
  }
  return operations.dump();
}

}  // namespace
}  // namespace voxelsieve

// NOLINTBEGIN(misc-use-anonymous-namespace,readability-identifier-naming): nanobind's macro
NB_MODULE(_core, module) {
  // NOLINTEND(misc-use-anonymous-namespace,readability-identifier-naming)
  using namespace voxelsieve;  // NOLINT(google-build-using-namespace)
  initializeVdb();
  returnFreedMemoryEagerly();
  module.doc() = "Native part of the VoxelSieve Python package; use the voxelsieve package.";

  nb::class_<Dataset>(module, "Dataset")
      .def_static(
          "open",
          [](const std::filesystem::path& dir, std::size_t cache_bytes, bool on_access) {
            return Dataset::open(dir, cache_bytes,
                                 on_access ? BrickLoading::kOnAccess : BrickLoading::kFull);
          },
          nb::arg("dir"), nb::arg("cache_bytes"), nb::arg("on_access"))
      .def("info", [](const Dataset& dataset) { return datasetInfoJson(dataset.info()).dump(); })
      .def("dir", [](const Dataset& dataset) { return dataset.dir(); })
      .def(
          "read_region",
          [](const Dataset& dataset, int level, const Index3& min, const Index3& max, float fill) {
            checkLevel(dataset, level);
            const Box box = makeBox(min, max);
            return toArray(readBox(dataset, level, box, fill), box);
          },
          nb::arg("level"), nb::arg("min"), nb::arg("max"), nb::arg("fill"))
      .def(
          "bricks",
          [](const Dataset& dataset, int level) {
            checkLevel(dataset, level);
            return dataset.level(level).bricks;
          },
          nb::arg("level"))
      .def(
          "brick_box",
          [](const Dataset& dataset, int level, const Index3& brick) {
            checkLevel(dataset, level);
            const Box box = dataset.brickBox(level, brick);
            return std::pair{box.min, box.max};
          },
          nb::arg("level"), nb::arg("brick"))
      .def(
          "has_brick",
          [](const Dataset& dataset, int level, const Index3& brick) {
            checkLevel(dataset, level);
            return dataset.hasBrick(level, brick);
          },
          nb::arg("level"), nb::arg("brick"))
      .def("cache_stats", [](const Dataset& dataset) {
        const CacheStats stats = dataset.cacheStats();
        return Json{{"hits", stats.hits},
                    {"misses", stats.misses},
                    {"bricks", stats.bricks},
                    {"bytes", stats.bytes}}
            .dump();
      });

  module.def(
      "write_dataset_u16",
      [](const VolumeArray<std::uint16_t>& array, const std::filesystem::path& dir,
         const WriteOptions& options) { return writeArray(array, dir, options); },
      nb::arg("array"), nb::arg("dir"), nb::arg("options"));
  module.def(
      "write_dataset_u8",
      [](const VolumeArray<std::uint8_t>& array, const std::filesystem::path& dir,
         const WriteOptions& options) { return writeArray(array, dir, options); },
      nb::arg("array"), nb::arg("dir"), nb::arg("options"));

  nb::class_<WriteOptions>(module, "WriteOptions")
      .def(nb::init<>())
      .def_rw("pitch_mm", &WriteOptions::pitch_mm)
      .def_rw("slice_thickness_mm", &WriteOptions::slice_thickness_mm)
      .def_rw("threshold", &WriteOptions::threshold)
      .def_rw("margin_voxels", &WriteOptions::margin_voxels)
      .def_rw("brick_size", &WriteOptions::brick_size)
      .def_rw("min_material_voxels", &WriteOptions::min_material_voxels)
      .def_rw("value_type", &WriteOptions::value_type)
      .def_rw("mapping_offset", &WriteOptions::mapping_offset)
      .def_rw("mapping_scale", &WriteOptions::mapping_scale);

  // Per-brick processing in Python (writeDerivedDataset): `function(brick, min, max, values)`
  // gets the input values of the brick's box grown by `halo` voxels (clipped to the volume, NaN
  // where no voxel is active) and returns the new values of that grown box.
  module.def(
      "write_derived_dataset",
      [](const Dataset& input, const std::filesystem::path& dir, const nb::callable& function,
         std::int64_t halo, const std::optional<std::string>& value_type) {
        if (halo < 0) {
          throw std::invalid_argument("halo must not be negative");
        }
        DerivedDatasetOptions options;
        if (value_type) {
          options.value_type = parseValueType(*value_type);
        }
        const Index3 dims = input.info().dims;
        const BrickValues values = [&](const Index3& brick, const Box& box,
                                       std::vector<float>& out) {
          Box grown = box;
          for (std::size_t axis = 0; axis < 3; ++axis) {
            grown.min[axis] = std::max<std::int64_t>(0, box.min[axis] - halo);
            grown.max[axis] = std::min(dims[axis], box.max[axis] + halo);
          }
          std::vector<float> region(static_cast<std::size_t>(grown.voxelCount()));
          input.readRegion(0, grown, region, std::numeric_limits<float>::quiet_NaN());
          const nb::gil_scoped_acquire acquire;
          const nb::object result =
              function(brick, grown.min, grown.max, toArray(std::move(region), grown));
          const auto computed =
              nb::cast<nb::ndarray<const float, nb::ndim<3>, nb::c_contig, nb::device::cpu>>(
                  result);
          if (computed.shape(0) != static_cast<std::size_t>(grown.size(2)) ||
              computed.shape(1) != static_cast<std::size_t>(grown.size(1)) ||
              computed.shape(2) != static_cast<std::size_t>(grown.size(0))) {
            throw std::invalid_argument(
                "The function must return an array of the shape of the values it got");
          }
          std::size_t index = 0;
          for (std::int64_t z = box.min[2]; z < box.max[2]; ++z) {
            for (std::int64_t y = box.min[1]; y < box.max[1]; ++y) {
              for (std::int64_t x = box.min[0]; x < box.max[0]; ++x) {
                out[index++] = computed(static_cast<std::size_t>(z - grown.min[2]),
                                        static_cast<std::size_t>(y - grown.min[1]),
                                        static_cast<std::size_t>(x - grown.min[0]));
              }
            }
          }
        };
        DatasetInfo info;
        {
          const nb::gil_scoped_release release;
          info = writeDerivedDataset(input, dir, values, options);
        }
        return datasetInfoJson(info).dump();
      },
      nb::arg("input"), nb::arg("dir"), nb::arg("function"), nb::arg("halo"),
      nb::arg("value_type"));

  nb::class_<MaterialVolume>(module, "MaterialVolume")
      .def_static("open", &MaterialVolume::open, nb::arg("dir"))
      .def("info",
           [](const MaterialVolume& volume) {
             const MaterialVolumeInfo& info = volume.info();
             Json materials = Json::array();
             for (const Material& material : info.materials) {
               materials.push_back({{"id", material.id},
                                    {"name", material.name},
                                    {"color", material.color},
                                    {"lower", material.lower},
                                    {"voxel_count", material.voxel_count},
                                    {"volume_mm3", material.volume_mm3}});
             }
             return Json{{"dims", info.dims},
                         {"voxel_size", voxelSizeJson(info.voxel_size)},
                         {"brick_size", info.brick_size},
                         {"air_level", info.air_level},
                         {"air_threshold", info.air_threshold},
                         {"materials", materials},
                         {"model", info.model}}
                 .dump();
           })
      .def(
          "read_region",
          [](const MaterialVolume& volume, const Index3& min, const Index3& max) {
            const Box box = makeBox(min, max);
            checkInside(box, volume.info().dims);
            std::vector<std::uint8_t> labels(static_cast<std::size_t>(box.voxelCount()));
            {
              const nb::gil_scoped_release release;
              volume.readRegion(box, labels);
            }
            return toArray(std::move(labels), box);
          },
          nb::arg("min"), nb::arg("max"));

  nb::class_<PyProject>(module, "Project")
      .def_static(
          "create",
          [](const std::filesystem::path& dir, const std::string& name) {
            return std::make_unique<PyProject>(Project::create(dir, name));
          },
          nb::arg("dir"), nb::arg("name"))
      .def_static(
          "open",
          [](const std::filesystem::path& dir) {
            return std::make_unique<PyProject>(Project::open(dir));
          },
          nb::arg("dir"))
      .def("dir", [](PyProject& self) { return self.project().dir(); })
      .def("name", [](PyProject& self) { return self.project().name(); })
      .def("run", &PyProject::run, nb::arg("operation"), nb::arg("params"), nb::arg("inputs"),
           nb::arg("object"), nb::arg("name"), nb::arg("progress").none())
      .def("undo", [](PyProject& self) { return self.project().undo(); })
      .def("redo", [](PyProject& self) { return self.project().redo(); })
      .def("cursor", [](PyProject& self) { return self.project().cursor(); })
      .def("steps", &PyProject::steps)
      .def("objects", &PyProject::objects)
      .def("results", &PyProject::results, nb::arg("object"))
      .def("select_object",
           [](PyProject& self, const std::string& id) { self.project().selectObject(id); })
      .def(
          "latest",
          [](PyProject& self, const std::string& type,
             const std::string& object) -> std::optional<std::filesystem::path> {
            const auto ref = self.project().latest(type, object);
            if (!ref) {
              return std::nullopt;
            }
            return self.project().resolve(*ref);
          },
          nb::arg("type"), nb::arg("object"))
      .def("operations", [](PyProject& self) { return operationsJson(self.registry()); })
      .def(
          "load_plugins",
          [](PyProject& self, const std::filesystem::path& dir) {
            return self.registry().loadPlugins(dir);
          },
          nb::arg("dir"));

  module.def("operations", [] {
    OperationRegistry registry;
    registerBuiltinOperations(registry);
    return operationsJson(registry);
  });

  module.def(
      "phantom",
      [](const Index3& dims, double voxel_size_mm, std::uint16_t air_value,
         std::uint16_t material_value, double noise_sigma, std::uint64_t seed,
         const std::array<double, 3>& outer_size_mm, double wall_thickness_mm,
         const std::vector<std::array<double, 4>>& pores) {
        PhantomSpec spec;
        spec.dims = dims;
        spec.voxel_size = VoxelSize(voxel_size_mm);
        spec.air_value = air_value;
        spec.material_value = material_value;
        spec.noise_sigma = noise_sigma;
        spec.seed = seed;
        spec.outer_size_mm = outer_size_mm;
        spec.wall_thickness_mm = wall_thickness_mm;
        for (const auto& pore : pores) {
          spec.pores.push_back(Pore{{pore[0], pore[1], pore[2]}, pore[3]});
        }
        Volume16 volume;
        {
          const nb::gil_scoped_release release;
          volume = generatePhantom(spec);
        }
        const double material_mm3 = phantomMaterialVolumeMm3(spec);
        return std::pair{toArray(std::move(volume.data), Box{{0, 0, 0}, dims}), material_mm3};
      },
      nb::arg("dims"), nb::arg("voxel_size_mm"), nb::arg("air_value"), nb::arg("material_value"),
      nb::arg("noise_sigma"), nb::arg("seed"), nb::arg("outer_size_mm"),
      nb::arg("wall_thickness_mm"), nb::arg("pores"));

  module.def("version", [] { return std::string(VOXELSIEVE_PYTHON_VERSION); });
}
