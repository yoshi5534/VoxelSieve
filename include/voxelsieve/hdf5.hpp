#pragma once

// Volumes in HDF5 files (ADR 0045), as synchrotron beamlines, tomography pipelines and NeXus
// files store them, read with the HDF5 library.

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "voxelsieve/source.hpp"
#include "voxelsieve/voxel_size.hpp"

namespace voxelsieve {

/// A dataset of an HDF5 file that can be read as a volume: three dimensions (or more, of which
/// all but three have size 1) of 8-, 16- or 32-bit integers or floats.
struct Hdf5Volume {
  /// Path inside the file, such as "/exchange/data" or "/entry/data/data".
  std::string path;
  /// x, y, z: the file's last dimension is x (C order, as written by numpy and h5py).
  std::array<std::int64_t, 3> dims{};
  RawSampleType sample_type = RawSampleType::kUInt16;
};

/// Whether `path` is an HDF5 file (the signature at offset 0, 512, 1024, ...).
[[nodiscard]] bool isHdf5File(const std::filesystem::path& path);

/// The datasets of an HDF5 file that can be read as volumes, the one to read first: the largest
/// whose name does not look like labels or masks, then the others by size.
[[nodiscard]] std::vector<Hdf5Volume> listHdf5Volumes(const std::filesystem::path& path);

struct Hdf5Options {
  /// Path of the dataset inside the file; empty: the first of listHdf5Volumes.
  std::string dataset;
  /// Overrides the voxel size of the file.
  std::optional<VoxelSize> voxel_size;
  /// 32-bit and float samples: the values mapped to grey 0 and 65535; unset: estimated as for
  /// raw files (ADR 0044).
  std::optional<std::array<double, 2>> value_range;
};

/// One dataset of an HDF5 file as volume source. Signed, 32-bit and float samples become grey
/// values as in raw files (ADR 0044). The voxel size comes from the options, else an attribute
/// of the dataset (`element_size_um` z, y, x in µm; `voxel_size`, `pixel_size` or `spacing` in
/// mm, one or three values x, y, z), else the file name, else 1 mm. Reads go through one lock,
/// since the HDF5 library is not built thread-safe; chunks are decompressed by the library.
class Hdf5Source final : public VolumeSource {
 public:
  explicit Hdf5Source(const std::filesystem::path& path, const Hdf5Options& options = {});
  ~Hdf5Source() override;

  [[nodiscard]] std::array<std::int64_t, 3> dims() const override;
  [[nodiscard]] VoxelSize voxelSize() const override;
  [[nodiscard]] ValueMapping valueMapping() const override;
  [[nodiscard]] SampleType sampleType() const override;
  /// Always: chunks are decompressed by the library, one reader at a time.
  [[nodiscard]] bool slowRandomAccess() const override { return true; }
  [[nodiscard]] std::string identity() const override;
  void readRegion(const Box& box, std::span<std::uint16_t> out) const override;

  [[nodiscard]] const Hdf5Volume& volume() const;
  /// Other datasets of the file that could be read (choose with Hdf5Options::dataset).
  [[nodiscard]] const std::vector<Hdf5Volume>& otherVolumes() const;
  /// Where the voxel size comes from: "parameters", "file", "file name" or "assumed".
  [[nodiscard]] const std::string& voxelSizeFrom() const;
  /// 32-bit and float samples: the value range used; nothing when the samples are kept exactly.
  [[nodiscard]] std::optional<std::array<double, 2>> valueRange() const;
  [[nodiscard]] std::uint64_t clippedValues() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace voxelsieve
