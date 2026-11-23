// Internals of the built-in operations (builtin_operations.cpp), which are spread over the files
// of this directory by what they work on: what several of them share.
#pragma once

#include <openvdb/io/File.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <memory>
#include <numeric>
#include <optional>
#include <sstream>
#include <stdexcept>

#include "voxelsieve/battery.hpp"
#include "voxelsieve/beam_hardening.hpp"
#include "voxelsieve/compare.hpp"
#include "voxelsieve/dataset.hpp"
#include "voxelsieve/denoise.hpp"
#include "voxelsieve/dicom.hpp"
#include "voxelsieve/diconde.hpp"
#include "voxelsieve/dimensional.hpp"
#include "voxelsieve/first_look.hpp"
#include "voxelsieve/gzipped_xml.hpp"
#include "voxelsieve/io.hpp"
#include "voxelsieve/isolate.hpp"
#include "voxelsieve/materials.hpp"
#include "voxelsieve/measure.hpp"
#include "voxelsieve/mesh.hpp"
#include "voxelsieve/model.hpp"
#include "voxelsieve/operation.hpp"
#include "voxelsieve/orientation.hpp"
#include "voxelsieve/porosity.hpp"
#include "voxelsieve/preview.hpp"
#include "voxelsieve/project.hpp"
#include "voxelsieve/report.hpp"
#include "voxelsieve/source.hpp"
#include "voxelsieve/surface.hpp"
#include "voxelsieve/telemetry.hpp"
#include "voxelsieve/thickness.hpp"
#include "voxelsieve/tiff.hpp"

namespace voxelsieve::operations {

using Json = nlohmann::json;

/// The `value_type` parameter of the imports and of the operations that write a dataset (ADR 0021).
[[nodiscard]] Json valueTypeParameter();

/// The value type chosen with it; nothing for "native".
[[nodiscard]] std::optional<ValueType> valueTypeOption(const Json& params);

[[nodiscard]] Json datasetSummary(const DatasetInfo& info);

/// What an operation on the dataset of input `port` needs, from its active voxels (ADR 0034):
/// `memory` and `output` bytes per active voxel. Rough figures from the 2048^3 scale benchmark;
/// nothing when the input is no dataset.
[[nodiscard]] OperationEstimate perActiveVoxel(const OperationContext& context,
                                               const std::string& port, double memory,
                                               double output);

[[nodiscard]] std::string readTextFile(const std::filesystem::path& path);

/// The inspection order of a report step: order_path, then order merged over it.
[[nodiscard]] Json readOrder(const Json& p);

/// An entry of OperationContext::objects.
[[nodiscard]] const Json& objectEntry(const OperationContext& context, const std::string& id);

[[nodiscard]] RigidTransform entryPose(const Json& object);

/// The surface of an object in its own coordinates (mm): a mesh object's triangles, or the
/// extracted surface of a volume (which needs a surface step). `resolution_mm` is set to half the
/// smallest voxel pitch of a volume and left alone for a mesh.
[[nodiscard]] IndexedMesh objectSurface(const Json& object, std::size_t max_triangles,
                                        bool outer_only, double& resolution_mm);

/// Each file of this directory registers its operations.
void registerImportOperations(OperationRegistry& registry);
void registerInspectionOperations(OperationRegistry& registry);
void registerSurfaceOperations(OperationRegistry& registry);
void registerMaterialsOperations(OperationRegistry& registry);
void registerObjectsOperations(OperationRegistry& registry);
void registerBatteryOperations(OperationRegistry& registry);

}  // namespace voxelsieve::operations
