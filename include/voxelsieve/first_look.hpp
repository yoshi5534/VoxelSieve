#pragma once

// The first look at a scan (ADR 0039): what a file is and how to open it without questions, and
// which analyses suit the volume once it is open.

#include <filesystem>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

#include "voxelsieve/dataset.hpp"
#include "voxelsieve/source.hpp"

namespace voxelsieve {

/// The layout of a raw volume as import_raw reads it, and where each part comes from.
struct RawInput {
  RawLayout layout;
  /// False when nothing gives the voxel size: the import assumes 1 mm and warns.
  bool voxel_size_known = false;
  /// Where the dimensions, sample type and voxel size come from: "parameters", "sidecar",
  /// "file name", "grey values" or "assumed", by "dims", "sample_type", "voxel_size".
  std::map<std::string, std::string> from;
  /// Of the detection when the dimensions come from the grey values (RawLayoutGuess), else 1.
  double confidence = 1.0;
};

/// Resolves the layout of the raw volume `path` from the import parameters `params` (dims,
/// sample_type, big_endian, header_bytes, voxel_size_mm; each optional), then its sidecar, then
/// detectRawLayout. Throws std::invalid_argument when the dimensions cannot be found.
[[nodiscard]] RawInput resolveRawInput(const std::filesystem::path& path,
                                       const nlohmann::json& params);

/// What a file or directory is and how to open it (the inspect_scan method): kind (dataset, raw,
/// tiff, dicom, gzipped_xml, mesh, project or unknown), the operation that opens it and its
/// parameters, what was found (dimensions, sample type, voxel size, size) with where each comes
/// from, notes for the user, whether anything was guessed (`check`: the user should look at the
/// values before importing) and a project directory next to the scan for it.
[[nodiscard]] nlohmann::json inspectScan(const std::filesystem::path& path);

/// A directory for a new project of the scan `path`: `<name>.vsproj` next to it, or
/// `<name>-2.vsproj` and so on when that exists and is no project of the scan.
[[nodiscard]] std::filesystem::path projectDirFor(const std::filesystem::path& path);

/// STL files in the directory of the scan `path` (at most a few), the one named like the scan
/// first: CAD models it may be compared with.
[[nodiscard]] std::vector<std::filesystem::path> cadModelsNextTo(const std::filesystem::path& path);

/// What the project knows about a dataset, for suggestAnalyses.
struct AnalysisContext {
  /// Name of the object or file, searched for words such as "battery" or "cfrp".
  std::string name;
  /// Operations already run on the dataset (active steps).
  std::vector<std::string> done;
  /// Mesh objects of the project (CAD models): id and name.
  std::vector<std::pair<std::string, std::string>> meshes;
  /// STL files next to the scan, which could be the CAD model.
  std::vector<std::filesystem::path> nearby_meshes;
  /// Operations the registry has (plugins may add some; suggestions name only these).
  std::vector<std::string> available;
};

/// Analyses that suit the dataset (the suggest_analyses method), most useful first: each with
/// operation, title, why (from what was measured), params to run it with, and done when it has
/// run. Measures on a coarse level only, so it is quick for scans of any size: the materials in
/// the histogram (suggestMaterials), the cupping (measureCupping), how round the cross-section
/// is, and looks for words in the name. Also gives these facts.
[[nodiscard]] nlohmann::json suggestAnalyses(const Dataset& dataset,
                                             const AnalysisContext& context);

}  // namespace voxelsieve
