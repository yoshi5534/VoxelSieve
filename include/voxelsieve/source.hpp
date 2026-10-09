#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>

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

/// Linear map from the stored 16-bit grey values to the values of the scan, for scans whose values
/// are not 16-bit integers, such as float reconstructions (ADR 0015): value = offset + scale *
/// grey. The identity for integer scans.
struct ValueMapping {
  double offset = 0.0;
  double scale = 1.0;

  [[nodiscard]] bool isIdentity() const { return offset == 0.0 && scale == 1.0; }
  [[nodiscard]] double toValue(double grey) const { return offset + scale * grey; }
  [[nodiscard]] double toGrey(double value) const { return (value - offset) / scale; }
  bool operator==(const ValueMapping&) const = default;
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
  /// How the grey values of `readRegion` relate to the values of the scan.
  [[nodiscard]] virtual ValueMapping valueMapping() const { return {}; }
  /// Whether reading small regions in any order costs much more than reading the volume once,
  /// slice by slice: slices that are stored compressed or decoded as a whole, such as TIFF stacks
  /// in a ZIP archive, or files on a network share, where every small read is a request of its
  /// own. `writeDataset` first copies such a volume to a temporary raw file.
  [[nodiscard]] virtual bool slowRandomAccess() const { return false; }
  /// Lets go of memory held for faster reading, such as caches of decoded data or pages of a
  /// memory-mapped input; later reads work as before. Called once a source has been copied, and
  /// for a raw copy now and then while it is read.
  virtual void releaseMemory() const {}

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
  /// True when the file lies on a network share (SMB, NFS, a mapped network drive): the import
  /// then reads it once, slice by slice, instead of twice in small pieces (ADR 0020).
  [[nodiscard]] bool slowRandomAccess() const override { return on_network_share_; }
  /// Lets the pages read so far go from the process's memory; they are read again when needed.
  void releaseMemory() const override;
  void readRegion(const Box& box, std::span<std::uint16_t> out) const override;

 private:
  struct Mapping;
  std::unique_ptr<Mapping> mapping_;
  RawLayout layout_;
  std::uint64_t header_bytes_ = 0;
  bool on_network_share_ = false;
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

/// Several volumes placed one after another along `axis`, read as one volume. This joins scans
/// that were reconstructed in parts, such as the sub-volumes of a long object. The other two
/// dimensions, the voxel size and the value mapping of all parts must be equal.
class ConcatSource final : public VolumeSource {
 public:
  ConcatSource(std::vector<std::unique_ptr<VolumeSource>> parts, int axis);

  [[nodiscard]] std::array<std::int64_t, 3> dims() const override { return dims_; }
  [[nodiscard]] VoxelSize voxelSize() const override { return parts_.front()->voxelSize(); }
  [[nodiscard]] ValueMapping valueMapping() const override {
    return parts_.front()->valueMapping();
  }
  [[nodiscard]] bool slowRandomAccess() const override;
  void releaseMemory() const override;
  void readRegion(const Box& box, std::span<std::uint16_t> out) const override;

  [[nodiscard]] int axis() const { return axis_; }
  [[nodiscard]] std::size_t partCount() const { return parts_.size(); }
  /// First voxel of part `i` along `axis`.
  [[nodiscard]] std::int64_t partStart(std::size_t i) const { return starts_[i]; }

 private:
  std::vector<std::unique_ptr<VolumeSource>> parts_;
  int axis_ = 2;
  std::vector<std::int64_t> starts_;  // one more than parts: the last is dims_[axis_]
  std::array<std::int64_t, 3> dims_{};
};

}  // namespace voxelsieve
