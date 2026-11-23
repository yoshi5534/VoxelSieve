// Built-in operations: meshes and the poses of objects.
#include "operations/common.hpp"

namespace voxelsieve::operations {
namespace {

/// Writes pose.json and fills the summary of a step that moves objects.
OperationResult movedResult(const OperationContext& context, const std::vector<std::string>& ids,
                            const RigidTransform& motion, Json summary) {
  Json poses = Json::object();
  for (const std::string& id : ids) {
    poses[id] = motion.after(entryPose(objectEntry(context, id))).matrix();
  }
  summary[kMovedObjectsKey] = ids;
  summary[kMotionKey] = motion.matrix();
  summary["rotation_deg"] = motion.angleDegrees();
  summary["translation_mm"] = motion.translation;
  Json record = summary;
  record["poses"] = poses;
  writeJson(context.output_dir / "pose.json", record);
  OperationResult result;
  result.outputs["pose"] = "pose.json";
  result.summary = std::move(summary);
  return result;
}

/// The ids in the `objects` parameter, each known and named once.
std::vector<std::string> movedObjects(const OperationContext& context) {
  const auto ids = context.params.at("objects").get<std::vector<std::string>>();
  for (const std::string& id : ids) {
    if (std::ranges::count(ids, id) > 1) {
      throw std::invalid_argument("Object '" + id + "' is named twice");
    }
    (void)objectEntry(context, id);
  }
  return ids;
}

const Json& movedObjectsSchema() {
  static const Json schema = {
      {"type", "array"},
      {"items", {{"type", "string"}}},
      {"minItems", 1},
      {"description", "Ids of the objects to move together; the first one is fitted"}};
  return schema;
}

class AddMesh final : public Operation {
 public:
  AddMesh() {
    info_.id = "add_mesh";
    info_.title = "Add mesh";
    info_.description =
        "Adds a triangle mesh in mm, such as a CAD model, as an object of the project. It starts "
        "in the coordinates of the file; move or align it afterwards. The file is not copied.";
    info_.outputs = {{"mesh", artifact::kMesh, "The mesh (STL)"}};
    info_.parameters = {
        {"type", "object"},
        {"properties",
         {{"path", {{"type", "string"}, {"format", "path"}, {"description", "STL file in mm"}}},
          {"color",
           {{"type", "string"},
            {"pattern", "^#[0-9a-fA-F]{6}$"},
            {"description",
             "Colour of the object in views as #rrggbb; default: by its place in the list"}}},
          {"pose",
           {{"type", "array"},
            {"items", {{"type", "number"}}},
            {"minItems", 12},
            {"maxItems", 16},
            {"description",
             "Pose of the new object, object to global coordinates, as a row-major 3x4 or 4x4 "
             "matrix in mm; default: the coordinates of the file"}}}}},
        {"required", {"path"}}};
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }

  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    const std::filesystem::path path =
        std::filesystem::absolute(context.params.at("path").get<std::string>());
    const Mesh mesh = readStl(path);
    if (mesh.triangles.empty()) {
      throw std::invalid_argument("The mesh has no triangles: " + path.string());
    }
    const Bounds bounds = meshBounds(mesh);
    OperationResult result;
    result.outputs["mesh"] = path;
    result.summary = {{"triangles", mesh.triangles.size()},
                      {"bounds_min_mm", bounds.min},
                      {"bounds_max_mm", bounds.max},
                      {"volume_mm3", meshVolumeMm3(mesh)}};
    if (context.params.contains("pose")) {
      // The object starts at this pose instead of the identity.
      const RigidTransform pose =
          RigidTransform::fromMatrix(context.params.at("pose").get<std::vector<double>>());
      result.summary[kMovedObjectsKey] = Json::array({context.object});
      result.summary[kMotionKey] = pose.matrix();
    }
    return result;
  }

 private:
  OperationInfo info_;
};

class MoveObjects final : public Operation {
 public:
  MoveObjects() {
    const auto vector3 = [](const Json& extra) {
      Json schema = {
          {"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3}};
      schema.update(extra);
      return schema;
    };
    info_.id = "move";
    info_.title = "Move objects";
    info_.description =
        "Moves objects together in the global coordinate system of the project by a rigid "
        "motion: a rotation about an axis through a centre, then a translation. Alternatively "
        "`pose` places objects at a given pose. Moves apply on top of the earlier ones, in "
        "order; undo takes them back. The data is not touched.";
    info_.outputs = {{"pose", artifact::kPose, "The motion and the new poses (pose.json)"}};
    info_.parameters = {
        {"type", "object"},
        {"properties",
         {{"objects",
           {{"type", "array"},
            {"items", {{"type", "string"}}},
            {"minItems", 1},
            {"description", "Ids of the objects to move together"}}},
          {"translation_mm", vector3({{"default", {0.0, 0.0, 0.0}}})},
          {"rotation_axis", vector3({{"default", {0.0, 0.0, 1.0}}})},
          {"rotation_deg", {{"type", "number"}, {"default", 0.0}}},
          {"center_mm", vector3({{"default", {0.0, 0.0, 0.0}},
                                 {"description", "Point the rotation axis passes through"}})},
          {"pose",
           {{"type", "array"},
            {"items", {{"type", "number"}}},
            {"minItems", 12},
            {"maxItems", 16},
            {"description",
             "New pose of the objects (which must share one pose, such as a single object), "
             "object to global coordinates, as a row-major 3x4 or 4x4 matrix in mm; replaces "
             "the rotation and translation"}}}}},
        {"required", {"objects"}}};
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }

  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    const Json& p = context.params;
    const auto ids = movedObjects(context);
    RigidTransform motion;
    if (p.contains("pose")) {
      const auto current = entryPose(objectEntry(context, ids.front())).matrix();
      for (const std::string& id : ids) {
        if (entryPose(objectEntry(context, id)).matrix() != current) {
          throw std::invalid_argument("'pose' places objects that share one pose");
        }
      }
      motion = RigidTransform::fromMatrix(p.at("pose").get<std::vector<double>>())
                   .after(entryPose(objectEntry(context, ids.front())).inverse());
    } else {
      const auto center = p.at("center_mm").get<std::array<double, 3>>();
      const auto translation = p.at("translation_mm").get<std::array<double, 3>>();
      const RigidTransform rotation = RigidTransform::fromAxisAngle(
          p.at("rotation_axis").get<std::array<double, 3>>(), p.at("rotation_deg").get<double>());
      // x' = R (x - c) + c + t
      const auto rotated = rotation.rotate(center);
      motion = rotation;
      for (std::size_t i = 0; i < 3; ++i) {
        motion.translation[i] = center[i] - rotated[i] + translation[i];
      }
    }
    return movedResult(context, ids, motion, Json::object());
  }

 private:
  OperationInfo info_;
};

class AlignPoints final : public Operation {
 public:
  AlignPoints() {
    info_.id = "align_points";
    info_.title = "Align by point pairs";
    info_.description =
        "Moves objects so that points picked on them meet their partners on the target: the "
        "rigid motion with the least squared distances (at least three pairs, not on a line). "
        "Points are in global coordinates (mm) where they lie now. Applies on top of the earlier "
        "moves; the distance left per pair is in the result.";
    info_.outputs = {{"pose", artifact::kPose, "The motion, the new poses and the residuals"}};
    info_.parameters = {
        {"type", "object"},
        {"properties",
         {{"objects", movedObjectsSchema()},
          {"target", {{"type", "string"}, {"description", "Id of the object aligned to"}}},
          {"pairs",
           {{"type", "array"},
            {"items", {{"type", "object"}}},
            {"minItems", 3},
            {"description",
             "Point pairs {moving: [x, y, z], target: [x, y, z]}, global coordinates in mm"}}}}},
        {"required", {"objects", "pairs"}}};
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }

  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    const Json& p = context.params;
    const auto ids = movedObjects(context);
    if (p.contains("target")) {
      const std::string target = p.at("target").get<std::string>();
      (void)objectEntry(context, target);
      if (std::ranges::count(ids, target) > 0) {
        throw std::invalid_argument("The target must not move with the objects");
      }
    }
    std::vector<std::array<double, 3>> moving;
    std::vector<std::array<double, 3>> target;
    for (const Json& pair : p.at("pairs")) {
      if (!pair.contains("moving") || !pair.contains("target")) {
        throw std::invalid_argument("A point pair needs 'moving' and 'target'");
      }
      moving.push_back(pair.at("moving").get<std::array<double, 3>>());
      target.push_back(pair.at("target").get<std::array<double, 3>>());
    }
    const RigidFit fit = fitRigid(moving, target);
    Json summary = {{"pairs", moving.size()},
                    {"residuals_mm", fit.residuals_mm},
                    {"rms_mm", fit.rms_mm},
                    {"max_residual_mm", std::ranges::max(fit.residuals_mm)}};
    if (p.contains("target")) {
      summary["target"] = p.at("target");
    }
    return movedResult(context, ids, fit.transform, std::move(summary));
  }

 private:
  OperationInfo info_;
};

class AlignSurfaces final : public Operation {
 public:
  AlignSurfaces() {
    info_.id = "align_surfaces";
    info_.title = "Align surfaces (best fit)";
    info_.description =
        "Moves objects so that the surface of the first one fits the surface of the target best: "
        "robust point-to-plane ICP as in the nominal-actual comparison, from where the objects "
        "lie now or, with start=principal_axes, from the best match of their principal axes. "
        "Volumes need a surface step first; only their outer skin is fitted. Applies on top of "
        "the earlier moves.";
    info_.outputs = {{"pose", artifact::kPose, "The motion, the new poses and the fit"}};
    info_.parameters = {
        {"type", "object"},
        {"properties",
         {{"objects", movedObjectsSchema()},
          {"target", {{"type", "string"}, {"description", "Id of the object aligned to"}}},
          {"start",
           {{"type", "string"},
            {"enum", {"current", "principal_axes"}},
            {"default", "current"},
            {"description",
             "current: refine where the objects lie (after a coarse step such as point pairs); "
             "principal_axes: find the orientation first, for objects far apart"}}},
          {"fit_points", {{"type", "integer"}, {"minimum", 100}, {"default", 20000}}},
          {"max_triangles", {{"type", "integer"}, {"minimum", 1000}, {"default", 2000000}}}}},
        {"required", {"objects", "target"}}};
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }

  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    const Json& p = context.params;
    const auto ids = movedObjects(context);
    const std::string target_id = p.at("target").get<std::string>();
    if (std::ranges::count(ids, target_id) > 0) {
      throw std::invalid_argument("The target must not move with the objects");
    }
    const Json& moving_object = objectEntry(context, ids.front());
    const Json& target_object = objectEntry(context, target_id);
    const auto max_triangles = p.at("max_triangles").get<std::size_t>();
    SurfaceAlignOptions options;
    options.coarse = p.at("start") == "principal_axes";
    options.fit_points = p.at("fit_points").get<std::size_t>();
    IndexedMesh moving;
    IndexedMesh target;
    {
      const TelemetryPhase phase("surfaces");
      moving = transformed(objectSurface(moving_object, max_triangles, true, options.resolution_mm),
                           entryPose(moving_object));
      context.progress(0.2);
      target = transformed(objectSurface(target_object, max_triangles, true, options.resolution_mm),
                           entryPose(target_object));
      context.progress(0.4);
    }
    const SurfaceAlignment aligned = alignSurfaces(moving, target, options);
    context.progress(0.95);
    return movedResult(context, ids, aligned.motion,
                       {{"target", target_id},
                        {"start", p.at("start")},
                        {"fit_rms_mm", aligned.rms_mm},
                        {"fit_inliers", aligned.inliers},
                        {"fit_iterations", aligned.iterations},
                        {"resolution_mm", aligned.resolution_mm}});
  }

 private:
  OperationInfo info_;
};

}  // namespace

void registerObjectsOperations(OperationRegistry& registry) {
  registry.add(std::make_shared<AddMesh>());
  registry.add(std::make_shared<MoveObjects>());
  registry.add(std::make_shared<AlignPoints>());
  registry.add(std::make_shared<AlignSurfaces>());
}

}  // namespace voxelsieve::operations
