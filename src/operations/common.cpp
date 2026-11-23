#include "operations/common.hpp"

namespace voxelsieve::operations {

Json valueTypeParameter() {
  return {{"type", "string"},
          {"enum", {"native", "uint16", "float"}},
          {"default", "native"},
          {"description",
           "Stored grey values: native keeps the input's 8 or 16 bits; float for viewers such as "
           "Blender and Houdini, which cannot read VoxelSieve's integer grids"}};
}

std::optional<ValueType> valueTypeOption(const Json& params) {
  const std::string name = params.value("value_type", std::string("native"));
  if (name == "native") {
    return std::nullopt;
  }
  return parseValueType(name);
}

Json datasetSummary(const DatasetInfo& info) {
  Json summary = {{"dims", info.dims}};
  writeVoxelSize(summary, info.voxel_size);
  summary.update({{"levels", info.levels.size()},
                  {"bricks", info.levels.empty() ? 0 : info.levels.front().bricks.size()},
                  {"active_voxels", info.active_voxel_count},
                  {"value_type", std::string(valueTypeName(info.value_type))},
                  {"threshold", info.threshold}});
  if (!info.value_mapping.isIdentity()) {
    summary["value_mapping"] = {{"offset", info.value_mapping.offset},
                                {"scale", info.value_mapping.scale}};
  }
  return summary;
}

OperationEstimate perActiveVoxel(const OperationContext& context, const std::string& port,
                                 double memory, double output) {
  const auto it = context.inputs.find(port);
  if (it == context.inputs.end()) {
    return {};
  }
  std::error_code error;
  if (!std::filesystem::exists(it->second / "index.json", error)) {
    return {};
  }
  const auto voxels = static_cast<double>(readDatasetInfo(it->second).active_voxel_count);
  return {static_cast<std::uint64_t>(voxels * memory), static_cast<std::uint64_t>(voxels * output)};
}

std::string readTextFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("Cannot read " + path.string());
  }
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

Json readOrder(const Json& p) {
  Json order = Json::object();
  if (p.contains("order_path")) {
    order = Json::parse(readTextFile(p.at("order_path").get<std::string>()));
  }
  if (p.contains("order")) {
    order.merge_patch(p.at("order"));
  }
  return order;
}

const Json& objectEntry(const OperationContext& context, const std::string& id) {
  for (const Json& object : context.objects) {
    if (object.at("id") == id) {
      return object;
    }
  }
  throw std::invalid_argument("No object '" + id + "'");
}

RigidTransform entryPose(const Json& object) {
  return RigidTransform::fromMatrix(object.at("pose").get<std::vector<double>>());
}

IndexedMesh objectSurface(const Json& object, std::size_t max_triangles, bool outer_only,
                          double& resolution_mm) {
  const Json& outputs = object.at("outputs");
  const std::string name = object.at("name").get<std::string>();
  if (object.at("kind") == kMeshObject) {
    return indexedMesh(readStl(outputs.at(artifact::kMesh).get<std::string>()));
  }
  if (!outputs.contains(artifact::kSurface)) {
    throw std::invalid_argument("Object '" + name +
                                "' has no surface yet: run the surface extraction on it first");
  }
  const SurfaceMask mask = SurfaceMask::open(
      std::filesystem::path(outputs.at(artifact::kSurface).get<std::string>()) / "surface.vss");
  const double half_voxel = 0.5 * mask.info().voxel_size.minMm();
  resolution_mm = resolution_mm > 0.0 ? std::min(resolution_mm, half_voxel) : half_voxel;
  return scanSurface(mask, max_triangles, outer_only).mesh;
}

}  // namespace voxelsieve::operations
