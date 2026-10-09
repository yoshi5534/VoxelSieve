#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "voxelsieve/transform.hpp"
#include "voxelsieve/voxel_size.hpp"

namespace voxelsieve {

/// Gzipped XML projects (.gzxml) as input (ADR 0019). A project does not hold the volumes; it refers
/// to the files they were imported from, with the import settings, the grid and where the volume
/// lies in the scene. `readGzippedXmlProject` reads those references so that the volumes can be imported
/// from their original files. VoxelSieve does not write .gzxml files.

/// A file a project refers to, as written in it, and where it was found.
struct GzippedXmlFile {
  /// The path as the project stores it: usually absolute, on the machine that saved it.
  std::string reference;
  /// The file on this machine: the reference itself, else the same path relative to the
  /// project's folder, else a file of that name next to the project. Nothing if none exists.
  std::optional<std::filesystem::path> path;
};

/// A volume of a project.
struct GzippedXmlVolume {
  std::string name;
  /// The class of the import settings, such as DicomImportSettings.
  std::string import_class;
  /// The input format VoxelSieve reads the volume from: "dicom"; empty when it cannot.
  std::string format;
  /// The files the volume was imported from, in the order the project lists them.
  std::vector<GzippedXmlFile> files;
  std::array<std::int64_t, 3> dims{0, 0, 0};
  VoxelSize voxel_size;
  /// Sample type as the application names it (Int16, UInt16, Float32, ...).
  std::string sample_type;
  /// From voxel coordinates (index times pitch, voxel centres at integers) to the coordinates of
  /// the project's scene in mm.
  RigidTransform pose;
  /// Import settings VoxelSieve does not apply, such as axis swaps or resampling.
  std::vector<std::string> notes;
};

/// Another file the project refers to, which is not imported, such as a mask.
struct GzippedXmlReference {
  /// The class of the object that refers to the file.
  std::string owner_class;
  /// What the file is, in words.
  std::string description;
  GzippedXmlFile file;
};

struct GzippedXmlProject {
  std::string app_name;
  std::string app_version;
  std::vector<GzippedXmlVolume> volumes;
  std::vector<GzippedXmlReference> other_files;
};

/// Reads a gzipped XML project: gzip-compressed or plain XML. Throws if it is neither.
[[nodiscard]] GzippedXmlProject readGzippedXmlProject(const std::filesystem::path& file);

/// Whether `path` names a gzipped XML project (.gzxml).
[[nodiscard]] bool isGzippedXmlFile(const std::filesystem::path& path);

}  // namespace voxelsieve
