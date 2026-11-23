// Built-in operations: materials and processing of the volume.
#include "operations/common.hpp"

namespace voxelsieve::operations {
namespace {

//// Colour "#rrggbb" as RGB.
std::array<std::uint8_t, 3> parseColor(const std::string& text) {
  const bool hex = text.size() == 7 && text[0] == '#' &&
                   std::all_of(text.begin() + 1, text.end(),
                               [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); });
  if (!hex) {
    throw std::invalid_argument("Colour '" + text + "' is not of the form #rrggbb");
  }
  std::array<std::uint8_t, 3> color{};
  for (std::size_t c = 0; c < 3; ++c) {
    color[c] = static_cast<std::uint8_t>(std::stoi(text.substr(1 + 2 * c, 2), nullptr, 16));
  }
  return color;
}

/// Materials of a segmentation for the summary of its step: id, name, colour, volume and, for
/// the threshold segmentation, the grey value each class starts at.
Json materialSummary(const MaterialVolumeInfo& info, bool with_thresholds) {
  Json materials = Json::array();
  for (const Material& material : info.materials) {
    Json entry = {{"id", material.id},
                  {"name", material.name},
                  {"color", material.color},
                  {"voxels", material.voxel_count},
                  {"volume_mm3", material.volume_mm3}};
    if (with_thresholds) {
      entry["from_grey_value"] = material.lower;
    }
    materials.push_back(entry);
  }
  return materials;
}

Json cuppingJson(const CuppingProfile& profile) {
  return {{"level", profile.level},
          {"cupping", profile.cupping},
          {"depth_mm", profile.depth_mm},
          {"material_level", profile.level_grey}};
}

class SegmentMaterials final : public Operation {
 public:
  SegmentMaterials() {
    info_.id = "segment_materials";
    info_.title = "Material segmentation";
    info_.description =
        "Splits the part into materials by grey value: a voxel is material when it and enough "
        "of its neighbours are above the air threshold (noise spikes go, walls one voxel thin "
        "stay), material grows into touching voxels above a lower threshold, and the classes are "
        "separated by thresholds (unless given: the valleys between the peaks of the histogram "
        "when it has one per class, else multi-level Otsu). With materials 0 the classes "
        "are found in the histogram: one per peak above the air threshold, split in the valleys "
        "between them (the histogram tool shows the suggestion first). Writes a material volume "
        "with volumes per material; the slice view shows the materials in colour.";
    info_.inputs = {{"dataset", artifact::kDataset, "Dataset to segment"}};
    info_.outputs = {{"materials", artifact::kMaterials, "Material volume"}};
    info_.parameters = {
        {"type", "object"},
        {"properties",
         {{"materials",
           {{"type", "integer"},
            {"minimum", 0},
            {"maximum", 8},
            {"default", 2},
            {"description", "Number of classes; 0: one per peak of the histogram"}}},
          {"air_threshold",
           {{"type", "number"}, {"description", "Default: the dataset's threshold"}}},
          {"material_thresholds",
           {{"type", "array"},
            {"items", {{"type", "number"}}},
            {"description",
             "Thresholds between the classes (materials - 1, ascending); default: the valleys "
             "between the histogram's peaks when it has one per class, else Otsu"}}},
          {"min_neighbours",
           {{"type", "integer"},
            {"minimum", 1},
            {"maximum", 27},
            {"default", 6},
            {"description", "Voxels of the 3x3x3 neighbourhood above the air threshold"}}},
          {"grow_steps", {{"type", "integer"}, {"minimum", 0}, {"default", 1}}},
          {"grow_fraction",
           {{"type", "number"},
            {"minimum", 0},
            {"maximum", 1},
            {"default", 0.5},
            {"description", "Lower threshold between air level (0) and air threshold (1)"}}},
          {"material_names",
           {{"type", "array"},
            {"items", {{"type", "string"}}},
            {"description", "Names of the classes, in order; default: Material 1, 2, ..."}}},
          {"material_colors",
           {{"type", "array"},
            {"items", {{"type", "string"}}},
            {"description", "Colours of the classes as #rrggbb, in order"}}}}}};
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }
  [[nodiscard]] OperationEstimate estimate(const OperationContext& context) const override {
    return perActiveVoxel(context, "dataset", 0.1, 0.5);
  }

  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    const Json& p = context.params;
    SegmentationOptions options;
    options.progress = ProgressToken(context.progress, context.cancel);
    options.materials = p.at("materials").get<int>();
    if (p.contains("air_threshold")) {
      options.air_threshold = p.at("air_threshold").get<float>();
    }
    if (p.contains("material_thresholds")) {
      options.material_thresholds = p.at("material_thresholds").get<std::vector<float>>();
    }
    options.min_neighbours = p.at("min_neighbours").get<int>();
    options.grow_steps = p.at("grow_steps").get<int>();
    options.grow_fraction = p.at("grow_fraction").get<float>();
    if (p.contains("material_names")) {
      options.names = p.at("material_names").get<std::vector<std::string>>();
    }
    if (p.contains("material_colors")) {
      for (const Json& color : p.at("material_colors")) {
        options.colors.push_back(parseColor(color.get<std::string>()));
      }
    }
    const auto dataset = Dataset::open(context.inputs.at("dataset"));
    const MaterialVolumeInfo info =
        segmentMaterials(dataset, context.output_dir / "materials", options);
    OperationResult result;
    result.outputs["materials"] = "materials";
    result.summary = {{"air_threshold", info.air_threshold},
                      {"materials", materialSummary(info, true)}};
    if (options.materials == 0) {
      result.summary["materials_from_histogram"] = true;
    }
    return result;
  }

 private:
  OperationInfo info_;
};

class MaterialMeshes final : public Operation {
 public:
  MaterialMeshes() {
    info_.id = "material_meshes";
    info_.title = "Material meshes";
    info_.description =
        "Exports the surface of each material of a material segmentation as a triangle mesh: "
        "one binary STL in mm per material, in the coordinates of the dataset (like the surface "
        "step's surface.stl), for CAD, 3D printing or other viewers. A material's surface is the "
        "boundary between its voxels and all others, so inner materials get closed meshes too. "
        "Large volumes are meshed from every 2nd, 4th, ... voxel until max_triangles holds. "
        "With add_objects (the default, when run as run_material_meshes), each mesh then also "
        "becomes an object of the project in its material's colour and at the pose of the "
        "segmented volume, so view_objects, alignment and comparison can use the materials "
        "one by one.";
    info_.inputs = {{"materials", artifact::kMaterials, "Material segmentation"}};
    info_.outputs = {{"meshes", artifact::kMaterialMeshes, "One STL per material"}};
    info_.parameters = {{"type", "object"},
                        {"properties",
                         {{"material_ids",
                           {{"type", "array"},
                            {"items", {{"type", "integer"}, {"minimum", 1}}},
                            {"description", "Materials to export; default: all"}}},
                          {"max_triangles",
                           {{"type", "integer"},
                            {"minimum", 0},
                            {"default", 2000000},
                            {"description", "Triangles per material at most; 0: full resolution"}}},
                          {"add_objects",
                           {{"type", "boolean"},
                            {"default", true},
                            {"description",
                             "Add each mesh as an object of the project, named and coloured like "
                             "its material"}}}}}};
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }

  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    const Json& p = context.params;
    const MaterialVolume volume = MaterialVolume::open(context.inputs.at("materials"));
    const MaterialVolumeInfo& info = volume.info();
    std::vector<int> ids(info.materials.size());
    std::ranges::transform(info.materials, ids.begin(), &Material::id);
    if (p.contains("material_ids")) {
      ids = p.at("material_ids").get<std::vector<int>>();
      for (const int id : ids) {
        if (std::ranges::none_of(info.materials, [id](const Material& m) { return m.id == id; })) {
          throw std::invalid_argument("The segmentation has no material " + std::to_string(id));
        }
      }
    }
    const auto max_triangles = p.at("max_triangles").get<std::size_t>();
    const std::filesystem::path dir = context.output_dir / "meshes";
    std::filesystem::create_directories(dir);
    Json meshes = Json::array();
    for (std::size_t i = 0; i < ids.size(); ++i) {
      const Material& material =
          *std::ranges::find_if(info.materials, [&](const Material& m) { return m.id == ids[i]; });
      const IndexedMesh voxels = materialMesh(volume, material.id, max_triangles);
      Mesh mesh;
      mesh.triangles.reserve(voxels.triangles.size());
      const auto mm = [&](std::uint32_t index) {
        const auto& q = voxels.points[index];
        return std::array<float, 3>{static_cast<float>(q[0] * info.voxel_size[0]),
                                    static_cast<float>(q[1] * info.voxel_size[1]),
                                    static_cast<float>(q[2] * info.voxel_size[2])};
      };
      for (const auto& t : voxels.triangles) {
        mesh.triangles.push_back({mm(t[0]), mm(t[1]), mm(t[2])});
      }
      std::string name;
      for (const char c : material.name) {
        name += std::isalnum(static_cast<unsigned char>(c)) != 0 ? c : '_';
      }
      const std::string file = "material_" + std::to_string(material.id) + "_" + name + ".stl";
      writeStl(dir / file, mesh);
      std::string color = "#";
      for (const std::uint8_t channel : material.color) {
        constexpr std::string_view kHex = "0123456789abcdef";
        color += kHex[channel >> 4U];
        color += kHex[channel & 15U];
      }
      meshes.push_back({{"id", material.id},
                        {"name", material.name},
                        {"color", color},
                        {"file", file},
                        {"triangles", mesh.triangles.size()}});
      context.progress(static_cast<double>(i + 1) / static_cast<double>(ids.size()));
    }
    OperationResult result;
    result.outputs["meshes"] = "meshes";
    result.summary = {{"meshes", meshes}};
    return result;
  }

 private:
  OperationInfo info_;
};

class MeasureObjects final : public Operation {
 public:
  MeasureObjects() {
    info_.id = "measure_objects";
    info_.title = "Measure objects";
    info_.description =
        "Measures every object of a material segmentation: each connected piece of a material "
        "(voxels sharing faces), such as a separated part, a bone or an organ, with its volume, "
        "surface area, centroid, bounding box and its lengths along its principal axes and along "
        "an axis you give. Material 0 measures enclosed air, the voids that do not reach the "
        "border, such as a swim bladder or a cavity. Writes a table (objects.csv) and "
        "objects.json; coordinates in mm in the volume's own coordinates.";
    info_.inputs = {{"materials", artifact::kMaterials, "Material segmentation"}};
    info_.outputs = {{"measurements", artifact::kMeasurements, "objects.csv and objects.json"}};
    info_.parameters = {
        {"type", "object"},
        {"properties",
         {{"material_ids",
           {{"type", "array"},
            {"items", {{"type", "integer"}, {"minimum", 0}}},
            {"description", "Materials to measure, 0 for enclosed air; default: every material"}}},
          {"min_voxels",
           {{"type", "integer"},
            {"minimum", 1},
            {"default", 27},
            {"description", "Objects smaller than this many voxels are left out"}}},
          {"max_objects", {{"type", "integer"}, {"minimum", 1}, {"default", 100}}},
          {"axis",
           {{"type", "array"},
            {"items", {{"type", "number"}}},
            {"minItems", 3},
            {"maxItems", 3},
            {"description",
             "Direction along the volume's x, y, z to measure each object's length along, such "
             "as [0, 0, 1]"}}}}}};
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }

  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    const Json& p = context.params;
    MeasureOptions options;
    if (p.contains("material_ids")) {
      options.material_ids = p.at("material_ids").get<std::vector<int>>();
    }
    options.min_voxels = p.at("min_voxels").get<std::int64_t>();
    options.max_objects = p.at("max_objects").get<int>();
    if (p.contains("axis")) {
      options.axis = p.at("axis").get<std::array<double, 3>>();
    }
    const MaterialVolume volume = MaterialVolume::open(context.inputs.at("materials"));
    const MeasureInfo info = measureObjects(volume, options);
    context.progress(0.9);
    const std::filesystem::path dir = context.output_dir / "measurements";
    std::filesystem::create_directories(dir);
    writeObjectsCsv(dir / "objects.csv", info);
    Json objects = Json::array();
    for (std::size_t k = 0; k < info.objects.size(); ++k) {
      const MeasuredObject& object = info.objects[k];
      Json json = {{"object", k + 1},
                   {"material", object.material},
                   {"material_name", object.material_name},
                   {"voxels", object.voxels},
                   {"volume_mm3", object.volume_mm3},
                   {"surface_mm2", object.surface_mm2},
                   {"centroid_mm", object.centroid_mm},
                   {"min_mm", object.min_mm},
                   {"max_mm", object.max_mm},
                   {"principal_axes", object.principal_axes},
                   {"principal_lengths_mm", object.principal_lengths_mm},
                   {"touches_border", object.touches_border}};
      if (object.axis_length_mm) {
        json["axis_length_mm"] = *object.axis_length_mm;
      }
      objects.push_back(std::move(json));
    }
    writeJson(
        dir / "objects.json",
        Json{{"stride", info.stride}, {"objects_found", info.objects_found}, {"objects", objects}});
    OperationResult result;
    result.outputs["measurements"] = "measurements";
    // The summary keeps the largest objects; the files hold all.
    constexpr std::size_t kSummaryObjects = 20;
    Json shown = Json::array();
    for (std::size_t k = 0; k < std::min(kSummaryObjects, objects.size()); ++k) {
      Json brief = objects[k];
      brief.erase("principal_axes");
      brief.erase("min_mm");
      brief.erase("max_mm");
      shown.push_back(std::move(brief));
    }
    result.summary = {{"objects_found", info.objects_found},
                      {"objects_measured", info.objects.size()},
                      {"stride", info.stride},
                      {"objects", shown},
                      {"table", "measurements/objects.csv"}};
    if (info.stride > 1) {
      context.log("Measured on every " + std::to_string(info.stride) +
                  "th voxel per axis: the volume is large");
    }
    return result;
  }

 private:
  OperationInfo info_;
};

class SegmentWithModel final : public Operation {
 public:
  SegmentWithModel() {
    info_.id = "segment_model";
    info_.title = "Learned segmentation";
    info_.description =
        "Splits the part into materials with a learned model (.vsm, a small 3D network trained "
        "for example with tools/models). VoxelSieve runs it on the CPU tile by tile, so it works "
        "on datasets larger than RAM. Writes a material volume like the threshold segmentation; "
        "the slice view shows the materials in colour.";
    info_.inputs = {{"dataset", artifact::kDataset, "Dataset to segment"}};
    info_.outputs = {{"materials", artifact::kMaterials, "Material volume"}};
    info_.parameters = {
        {"type", "object"},
        {"properties",
         {{"model_path",
           {{"type", "string"}, {"format", "path"}, {"description", "Learned model (.vsm)"}}},
          {"tile",
           {{"type", "integer"},
            {"minimum", 16},
            {"default", 128},
            {"description", "Edge of the tiles the model runs on, in voxels"}}}}},
        {"required", {"model_path"}}};
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }
  [[nodiscard]] OperationEstimate estimate(const OperationContext& context) const override {
    return perActiveVoxel(context, "dataset", 0.1, 0.5);
  }

  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    const Json& p = context.params;
    const Model model =
        Model::load(std::filesystem::absolute(p.at("model_path").get<std::string>()));
    ModelSegmentationOptions options;
    options.progress = ProgressToken(context.progress, context.cancel);
    options.tile = p.at("tile").get<std::int64_t>();
    const auto dataset = Dataset::open(context.inputs.at("dataset"));
    const MaterialVolumeInfo info =
        segmentMaterialsWithModel(dataset, model, context.output_dir / "materials", options);
    OperationResult result;
    result.outputs["materials"] = "materials";
    result.summary = {{"model", info.model}, {"materials", materialSummary(info, false)}};
    return result;
  }

 private:
  OperationInfo info_;
};

class Denoise final : public Operation {
 public:
  Denoise() {
    info_.id = "denoise";
    info_.title = "Denoise";
    info_.description =
        "Reduces the noise of a dataset and writes a new dataset with the same part; the input "
        "stays in the project. Non-local means averages voxels whose neighbourhoods look alike, "
        "measured against the noise of the scan including the correlation of neighbouring "
        "voxels, and leaves out neighbourhoods that differ more than noise explains, so edges, "
        "thin walls and pores of a few voxels keep their contrast. A learned denoising model "
        "(.vsm, trained on the noisy scan itself with tools/models/train_denoiser.py) can run "
        "instead. Later steps of the object use the denoised dataset.";
    info_.inputs = {{"dataset", artifact::kDataset, "Dataset to denoise"}};
    info_.outputs = {{"dataset", artifact::kDataset, "Denoised dataset"}};
    info_.parameters = {
        {"type", "object"},
        {"properties",
         {{"method",
           {{"type", "string"},
            {"enum", {"non_local_means", "model"}},
            {"default", "non_local_means"}}},
          {"strength",
           {{"type", "number"},
            {"exclusiveMinimum", 0},
            {"default", 1.0},
            {"description", "Non-local means: larger values smooth more"}}},
          {"similarity_limit",
           {{"type", "number"},
            {"exclusiveMinimum", 0},
            {"default", 1.3},
            {"description",
             "Non-local means: neighbourhoods that differ by more than this multiple of the "
             "noise get no weight; larger values smooth small details more"}}},
          {"search_radius",
           {{"type", "integer"},
            {"minimum", 1},
            {"maximum", 7},
            {"default", 3},
            {"description", "Non-local means: half edge of the search window in voxels"}}},
          {"patch_radius",
           {{"type", "integer"},
            {"minimum", 0},
            {"maximum", 3},
            {"default", 1},
            {"description", "Non-local means: half edge of the compared patches in voxels"}}},
          {"noise_sigma",
           {{"type", "number"},
            {"minimum", 0},
            {"description",
             "Non-local means: noise in grey values, taken as uncorrelated; default: measured "
             "from the dataset, correlation included"}}},
          {"model_path",
           {{"type", "string"},
            {"format", "path"},
            {"description", "Method model: the learned denoising model (.vsm)"}}},
          {"tile",
           {{"type", "integer"},
            {"minimum", 0},
            {"default", 0},
            {"description", "Tile edge in voxels; 0: 64 for non-local means, 128 for a model"}}},
          {"value_type", valueTypeParameter()}}}};
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }
  [[nodiscard]] OperationEstimate estimate(const OperationContext& context) const override {
    return perActiveVoxel(context, "dataset", 0.1, 2.5);
  }

  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    const Json& p = context.params;
    DenoiseOptions options;
    std::optional<Model> model;
    if (p.at("method").get<std::string>() == "model") {
      if (!p.contains("model_path")) {
        throw std::invalid_argument("Denoising with a model needs model_path");
      }
      model.emplace(Model::load(std::filesystem::absolute(p.at("model_path").get<std::string>())));
      options.method = DenoiseMethod::kModel;
      options.model = &*model;
    }
    options.non_local_means.strength = p.at("strength").get<double>();
    options.non_local_means.similarity_limit = p.at("similarity_limit").get<double>();
    options.non_local_means.search_radius = p.at("search_radius").get<int>();
    options.non_local_means.patch_radius = p.at("patch_radius").get<int>();
    if (p.contains("noise_sigma")) {
      options.noise_sigma = p.at("noise_sigma").get<double>();
    }
    options.tile = p.at("tile").get<std::int64_t>();
    options.output.value_type = valueTypeOption(p);
    options.output.progress = [&context](std::string_view stage, double fraction) {
      context.progress(stage == "bricks" ? 0.95 * fraction : 0.95 + 0.05 * fraction);
    };
    const auto dataset = Dataset::open(context.inputs.at("dataset"));
    const DenoiseInfo info = denoiseDataset(dataset, context.output_dir / "dataset", options);
    const NoiseModel& noise = info.noise;
    const double after = estimateNoise(Dataset::open(context.output_dir / "dataset"), 1).sigma;
    std::ostringstream text;
    text << std::fixed << std::setprecision(0) << "Noise " << noise.sigma << " -> " << after
         << " grey values (correlation of neighbours " << std::setprecision(2)
         << noise.correlation(1, 0, 0) << ", " << noise.correlation(0, 1, 0) << ", "
         << noise.correlation(0, 0, 1) << ")";
    context.log(text.str());
    OperationResult result;
    result.outputs["dataset"] = "dataset";
    result.summary = datasetSummary(info.dataset);
    result.summary.update(
        {{"method", std::string(denoiseMethodName(info.method))},
         {"noise_sigma", noise.sigma},
         {"noise_sigma_after", after},
         {"noise_correlation",
          {noise.correlation(1, 0, 0), noise.correlation(0, 1, 0), noise.correlation(0, 0, 1)}}});
    if (!info.model.empty()) {
      result.summary["model"] = info.model;
    }
    return result;
  }

 private:
  OperationInfo info_;
};

class BeamHardeningCorrection final : public Operation {
 public:
  BeamHardeningCorrection() {
    info_.id = "beam_hardening";
    info_.title = "Correct beam hardening";
    info_.description =
        "Corrects beam hardening of a reconstructed dataset and writes a new dataset with the same "
        "part; the input stays in the project. Beam hardening darkens the material far from the "
        "surface (cupping) and between dense or thick regions (streaks). The reprojection method "
        "projects every slice across the rotation axis again, reconstructs the path lengths to "
        "the powers 2 to order and adds the combination that makes the material most uniform; it "
        "needs no spectrum and corrects streaks too. The depth method scales the grey values by "
        "the material level at their depth below the surface and corrects only cupping of one "
        "material. The correction is estimated on a coarse level and interpolated. The summary "
        "gives the cupping before and after. Later steps of the object use the corrected "
        "dataset.";
    info_.inputs = {{"dataset", artifact::kDataset, "Dataset to correct"}};
    info_.outputs = {{"dataset", artifact::kDataset, "Corrected dataset"}};
    info_.parameters = {
        {"type", "object"},
        {"properties",
         {{"method",
           {{"type", "string"},
            {"enum", {"reprojection", "depth"}},
            {"default", "reprojection"},
            {"description",
             "reprojection: cupping and streaks, needs the rotation axis; depth: cupping of one "
             "material only, no geometry needed"}}},
          {"rotation_axis",
           {{"type", "string"},
            {"enum", {"x", "y", "z"}},
            {"default", "z"},
            {"description",
             "Reprojection: rotation axis of the scan; slices across it are "
             "projected"}}},
          {"order",
           {{"type", "integer"},
            {"minimum", 2},
            {"maximum", 4},
            {"default", 3},
            {"description",
             "Reprojection: highest power of the path length; 2 suffices for mild hardening, 3 "
             "also follows hardening that saturates on long paths through dense material"}}},
          {"resolution",
           {{"type", "integer"},
            {"minimum", 8},
            {"maximum", 2048},
            {"default", 256},
            {"description",
             "Voxels across the largest edge of the level the correction is estimated on"}}},
          {"value_type", valueTypeParameter()}}}};
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }

  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    const Json& p = context.params;
    BeamHardeningOptions options;
    options.method = parseBeamHardeningMethod(p.at("method").get<std::string>());
    const std::string axis = p.at("rotation_axis").get<std::string>();
    options.rotation_axis = axis == "x" ? 0 : (axis == "y" ? 1 : 2);
    options.order = p.at("order").get<int>();
    options.resolution = p.at("resolution").get<std::int64_t>();
    options.output.value_type = valueTypeOption(p);
    options.output.progress = [&context](std::string_view stage, double fraction) {
      context.progress(stage == "bricks" ? 0.1 + 0.8 * fraction : 0.9 + 0.1 * fraction);
    };
    const auto dataset = Dataset::open(context.inputs.at("dataset"));
    const BeamHardeningInfo info =
        correctBeamHardening(dataset, context.output_dir / "dataset", options);
    std::ostringstream text;
    text << std::fixed << std::setprecision(1) << "Cupping " << 100.0 * info.before.cupping
         << " % -> " << 100.0 * info.after.cupping << " %, material spread " << std::setprecision(0)
         << info.material_spread_before << " -> " << info.material_spread_after
         << " grey values (level " << info.level << ")";
    context.log(text.str());
    OperationResult result;
    result.outputs["dataset"] = "dataset";
    result.summary = datasetSummary(info.dataset);
    result.summary.update({{"method", std::string(beamHardeningMethodName(info.method))},
                           {"level", info.level},
                           {"cupping_before", info.before.cupping},
                           {"cupping_after", info.after.cupping},
                           {"material_spread_before", info.material_spread_before},
                           {"material_spread_after", info.material_spread_after},
                           {"fit_voxels", info.fit_voxels},
                           {"profile_before", cuppingJson(info.before)},
                           {"profile_after", cuppingJson(info.after)}});
    if (!info.coefficients.empty()) {
      result.summary["coefficients"] = info.coefficients;
    }
    return result;
  }

 private:
  OperationInfo info_;
};

class IsolatePart final : public Operation {
 public:
  IsolatePart() {
    info_.id = "isolate_part";
    info_.title = "Isolate part";
    info_.description =
        "Removes what is not the part from a dataset and writes a new dataset; the input stays "
        "in the project. Voxels outside a region (a box in voxels, a crop) and objects other "
        "than the largest ones, such as a holder, a support or loose debris that the scan also "
        "shows, become outside air, so material segmentation, porosity, surfaces and volumes "
        "see the part alone. An object is material connected across faces, with its voids. "
        "Objects that touch are one object unless separate_voxels splits them at a thin neck; "
        "cut planes and a cylinder cut them apart where they touch broadly. Later steps of the "
        "object use the new dataset.";
    info_.inputs = {{"dataset", artifact::kDataset, "Dataset to isolate the part in"}};
    info_.outputs = {{"dataset", artifact::kDataset, "Dataset with the part alone"}};
    info_.parameters = {
        {"type", "object"},
        {"properties",
         {{"region_min",
           {{"type", "array"},
            {"items", {{"type", "integer"}, {"minimum", 0}}},
            {"minItems", 3},
            {"maxItems", 3},
            {"description", "First voxel (x, y, z) of the region to keep; default: 0, 0, 0"}}},
          {"region_max",
           {{"type", "array"},
            {"items", {{"type", "integer"}, {"minimum", 1}}},
            {"minItems", 3},
            {"maxItems", 3},
            {"description",
             "End of the region (x, y, z), exclusive; default: the dimensions of the dataset"}}},
          {"keep_objects",
           {{"type", "integer"},
            {"minimum", 0},
            {"default", 1},
            {"description", "Keep the largest objects, 1: only the largest; 0: all objects"}}},
          {"separate_voxels",
           {{"type", "integer"},
            {"minimum", 0},
            {"default", 0},
            {"description",
             "Separates touching objects, such as a holder glued to the part, where they meet "
             "through a neck thinner than about twice this many voxels (erosion, then growing "
             "back within the material); 0: only air separates objects"}}},
          {"cut_planes",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"properties",
               {{"point",
                 {{"type", "array"},
                  {"items", {{"type", "number"}}},
                  {"minItems", 3},
                  {"maxItems", 3},
                  {"description", "A point of the plane, in voxels (x, y, z)"}}},
                {"normal",
                 {{"type", "array"},
                  {"items", {{"type", "number"}}},
                  {"minItems", 3},
                  {"maxItems", 3},
                  {"description", "Points to the side that is removed"}}}}},
              {"required", {"point", "normal"}}}},
            {"description",
             "Planes that remove what lies on their normal's side, such as everything below a "
             "holder: point [0, 0, 120] with normal [0, 0, -1] removes all below z = 120. "
             "Several planes approximate a curved cut"}}},
          {"cylinder",
           {{"type", "object"},
            {"properties",
             {{"axis", {{"type", "string"}, {"enum", {"x", "y", "z"}}, {"default", "z"}}},
              {"center",
               {{"type", "array"},
                {"items", {{"type", "number"}}},
                {"minItems", 2},
                {"maxItems", 2},
                {"description", "Centre in the two other axes, in voxels (x, y for axis z)"}}},
              {"radius", {{"type", "number"}, {"exclusiveMinimum", 0}}}}},
            {"required", {"center", "radius"}},
            {"description", "Keep only the inside of this cylinder, such as a part in a tube"}}},
          {"threshold",
           {{"type", "number"},
            {"description",
             "Grey value above which a voxel is material; default: the dataset's"}}}}}};
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }
  [[nodiscard]] OperationEstimate estimate(const OperationContext& context) const override {
    return perActiveVoxel(context, "dataset", 0.3, 2.5);
  }

  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    const Json& p = context.params;
    const auto dataset = Dataset::open(context.inputs.at("dataset"));
    IsolateOptions options;
    if (p.contains("region_min") || p.contains("region_max")) {
      Box region{{0, 0, 0}, dataset.info().dims};
      if (p.contains("region_min")) {
        region.min = p.at("region_min").get<std::array<std::int64_t, 3>>();
      }
      if (p.contains("region_max")) {
        region.max = p.at("region_max").get<std::array<std::int64_t, 3>>();
      }
      options.region = region;
    }
    options.keep_objects = p.at("keep_objects").get<int>();
    options.separate_voxels = p.at("separate_voxels").get<int>();
    for (const Json& plane : p.value("cut_planes", Json::array())) {
      options.cut_planes.push_back({plane.at("point").get<std::array<double, 3>>(),
                                    plane.at("normal").get<std::array<double, 3>>()});
    }
    if (p.contains("cylinder")) {
      const Json& cylinder = p.at("cylinder");
      options.cylinder = RegionCylinder{cylinder.value("axis", std::string("z"))[0] - 'x',
                                        cylinder.at("center").get<std::array<double, 2>>(),
                                        cylinder.at("radius").get<double>()};
    }
    if (p.contains("threshold")) {
      options.threshold = p.at("threshold").get<float>();
    }
    options.progress = [&context](std::string_view stage, double fraction) {
      context.progress(stage == "objects"  ? 0.3 * fraction
                       : stage == "bricks" ? 0.3 + 0.65 * fraction
                                           : 0.95 + 0.05 * fraction);
    };
    const IsolateInfo info = isolatePart(dataset, context.output_dir / "dataset", options);
    std::ostringstream text;
    text << info.objects << " objects found; kept " << info.kept_material_voxels.size()
         << ", removed " << info.removed_voxels << " voxels";
    context.log(text.str());
    OperationResult result;
    result.outputs["dataset"] = "dataset";
    result.summary = datasetSummary(info.dataset);
    result.summary.update({{"objects_found", info.objects},
                           {"kept_material_voxels", info.kept_material_voxels},
                           {"removed_voxels", info.removed_voxels},
                           {"cell_size", info.cell_size}});
    return result;
  }

 private:
  OperationInfo info_;
};

}  // namespace

void registerMaterialsOperations(OperationRegistry& registry) {
  registry.add(std::make_shared<SegmentMaterials>());
  registry.add(std::make_shared<MaterialMeshes>());
  registry.add(std::make_shared<MeasureObjects>());
  registry.add(std::make_shared<SegmentWithModel>());
  registry.add(std::make_shared<Denoise>());
  registry.add(std::make_shared<BeamHardeningCorrection>());
  registry.add(std::make_shared<IsolatePart>());
}

}  // namespace voxelsieve::operations
