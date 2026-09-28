#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>

#include "voxelsieve/phantom.hpp"
#include "voxelsieve/volume.hpp"
#include "voxelsieve/voxel_size.hpp"

namespace voxelsieve {

/// Axis-aligned voxel box, `min` inclusive and `max` exclusive.
struct Box {
  std::array<std::int64_t, 3> min{0, 0, 0};
  std::array<std::int64_t, 3> max{0, 0, 0};

  [[nodiscard]] std::int64_t size(std::size_t axis) const { return max[axis] - min[axis]; }
  [[nodiscard]] std::int64_t voxelCount() const { return size(0) * size(1) * size(2); }
};

/// Read access to a volume that may be much larger than memory. Implementations must allow
/// concurrent calls to `readRegion` from several threads.
class VolumeSource {
 public:
  VolumeSource() = default;
  VolumeSource(const VolumeSource&) = delete;
  VolumeSource& operator=(const VolumeSource&) = delete;
  VolumeSource(VolumeSource&&) = delete;
  VolumeSource& operator=(VolumeSource&&) = delete;
  virtual ~VolumeSource() = default;

  [[nodiscard]] virtual std::array<std::int64_t, 3> dims() const = 0;
  [[nodiscard]] virtual VoxelSize voxelSize() const = 0;

  /// Copies the voxels of `box` into `out`, x fastest. `box` must lie inside the volume and
  /// `out.size()` must equal `box.voxelCount()`.
  virtual void readRegion(const Box& box, std::span<std::uint16_t> out) const = 0;
};

/// Source backed by a volume in memory (tests, small data).
class MemorySource final : public VolumeSource {
 public:
  explicit MemorySource(const Volume16& volume) : volume_(volume) {}
  [[nodiscard]] std::array<std::int64_t, 3> dims() const override { return volume_.dims; }
  [[nodiscard]] VoxelSize voxelSize() const override { return volume_.voxel_size; }
  void readRegion(const Box& box, std::span<std::uint16_t> out) const override;

 private:
  const Volume16& volume_;
};

enum class SampleType : std::uint8_t { kUInt8, kUInt16 };

/// Layout of a raw volume file: voxels x fastest, optionally preceded by a vendor header and
/// followed by a footer, both of which are skipped.
struct RawLayout {
  std::array<std::int64_t, 3> dims{0, 0, 0};
  VoxelSize voxel_size;
  SampleType sample_type = SampleType::kUInt16;
  std::endian byte_order = std::endian::little;
  /// Bytes before the voxel data. When unset, everything in the file beyond the voxel data is
  /// taken to be a header at the start (the common case for proprietary CT raw files).
  std::optional<std::uint64_t> header_bytes;
};

/// Raw volume file, memory-mapped. Only the pages that are read are loaded, and the operating
/// system evicts them under memory pressure. 8-bit samples are returned unchanged as 16-bit values.
class MappedRawSource final : public VolumeSource {
 public:
  MappedRawSource(const std::filesystem::path& path, const RawLayout& layout);
  /// Headerless little-endian uint16 file.
  MappedRawSource(const std::filesystem::path& path, const std::array<std::int64_t, 3>& dims,
                  const VoxelSize& voxel_size);
  ~MappedRawSource() override;

  [[nodiscard]] std::array<std::int64_t, 3> dims() const override { return layout_.dims; }
  [[nodiscard]] VoxelSize voxelSize() const override { return layout_.voxel_size; }
  /// Header size in bytes, as given or detected.
  [[nodiscard]] std::uint64_t headerBytes() const { return header_bytes_; }
  void readRegion(const Box& box, std::span<std::uint16_t> out) const override;

 private:
  struct Mapping;
  std::unique_ptr<Mapping> mapping_;
  RawLayout layout_;
  std::uint64_t header_bytes_ = 0;
};

/// Computes the synthetic phantom on the fly, so tests and benchmarks can use volumes of any size
/// without memory or disk.
class PhantomSource final : public VolumeSource {
 public:
  explicit PhantomSource(PhantomSpec spec) : spec_(std::move(spec)) {}
  [[nodiscard]] std::array<std::int64_t, 3> dims() const override { return spec_.dims; }
  [[nodiscard]] VoxelSize voxelSize() const override { return spec_.voxel_size; }
  void readRegion(const Box& box, std::span<std::uint16_t> out) const override;

 private:
  PhantomSpec spec_;
};

}  // namespace voxelsieve
