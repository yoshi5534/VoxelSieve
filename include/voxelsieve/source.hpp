#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "voxelsieve/phantom.hpp"
#include "voxelsieve/volume.hpp"
#include "voxelsieve/voxel_size.hpp"

namespace voxelsieve {

namespace detail {
struct RawSamples;
}  // namespace detail

/// Axis-aligned voxel box, `min` inclusive and `max` exclusive.
struct Box {
  std::array<std::int64_t, 3> min{0, 0, 0};
  std::array<std::int64_t, 3> max{0, 0, 0};

  [[nodiscard]] std::int64_t size(std::size_t axis) const { return max[axis] - min[axis]; }
  [[nodiscard]] std::int64_t voxelCount() const { return size(0) * size(1) * size(2); }
};

/// The largest volume edge accepted, in voxels: 2^20. Its cube still counts in 64 bits, so no
/// product of dimensions that passed checkVolumeDims overflows.
inline constexpr std::int64_t kMaxVolumeEdge = std::int64_t{1} << 20;

/// Throws std::invalid_argument unless every dimension is between 1 and kMaxVolumeEdge. `what`
/// names the volume in the message. Dimensions from parameters and files go through this before
/// anything is computed from them.
/// The identity of a file (absolute path, size, time of the last change) or of a directory
/// (path, number of files, their total size and latest change), as text.
[[nodiscard]] std::string inputIdentity(const std::filesystem::path& path);

void checkVolumeDims(const std::array<std::int64_t, 3>& dims, std::string_view what);

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

/// Width of the samples of a volume as stored in its files.
enum class SampleType : std::uint8_t { kUInt8, kUInt16 };

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
  /// Width of the input's samples: 8 bit when every grey value of `readRegion` is at most 255.
  /// A dataset stores the grey values in this width (ADR 0021).
  [[nodiscard]] virtual SampleType sampleType() const { return SampleType::kUInt16; }
  /// Whether reading small regions in any order costs much more than reading the volume once,
  /// slice by slice: slices that are stored compressed or decoded as a whole, such as TIFF stacks
  /// in a ZIP archive, or files on a network share, where every small read is a request of its
  /// own. `writeDataset` first copies such a volume to a temporary raw file.
  [[nodiscard]] virtual bool slowRandomAccess() const { return false; }
  /// Whether reading is efficient only forward from the start, as in a compressed stream: going
  /// back means decompressing again from the beginning. `writeDataset` always copies such a
  /// volume, in one pass in slice order (implies slowRandomAccess).
  [[nodiscard]] virtual bool sequentialAccess() const { return false; }
  /// Lets go of memory held for faster reading, such as caches of decoded data or pages of a
  /// memory-mapped input; later reads work as before. Called once a source has been copied, and
  /// for a raw copy now and then while it is read.
  virtual void releaseMemory() const {}
  /// What tells this input from another of the same size, for sources that read files: the
  /// path, the size and the time of the last change (inputIdentity). An interrupted import
  /// resumes only for the same identity. Empty when the source has no such thing.
  [[nodiscard]] virtual std::string identity() const { return {}; }

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

/// Another source with `factor` slices per slice spacing along z, interpolated linearly between
/// neighbouring slices: (nz - 1) * factor + 1 slices at a pitch of z / factor. Fills the gaps of
/// slices thinner than their spacing (the new voxel size has no slice thickness) and makes long
/// voxels closer to cubes; the interpolated grey values are estimates, not measurements.
class InterpolatedSource final : public VolumeSource {
 public:
  InterpolatedSource(const VolumeSource& base, int factor);
  [[nodiscard]] std::array<std::int64_t, 3> dims() const override;
  [[nodiscard]] VoxelSize voxelSize() const override;
  [[nodiscard]] ValueMapping valueMapping() const override { return base_.valueMapping(); }
  [[nodiscard]] SampleType sampleType() const override { return base_.sampleType(); }
  [[nodiscard]] bool slowRandomAccess() const override { return base_.slowRandomAccess(); }
  [[nodiscard]] bool sequentialAccess() const override { return base_.sequentialAccess(); }
  void releaseMemory() const override { base_.releaseMemory(); }
  [[nodiscard]] std::string identity() const override { return base_.identity(); }
  void readRegion(const Box& box, std::span<std::uint16_t> out) const override;

 private:
  const VolumeSource& base_;
  std::int64_t factor_;
};

/// Type of the samples in a raw file (ADR 0044). Unsigned 8 and 16 bit are the grey values
/// themselves; signed samples are shifted into the unsigned range (int8 by 128, int16 by 32768),
/// which keeps them exactly; 32-bit integers that lie within 0..65535 are kept as they are, and
/// wider ones and floats are mapped linearly onto 0..65535 over a value range. The mapping is
/// recorded with the dataset (ValueMapping, ADR 0015).
enum class RawSampleType : std::uint8_t {
  kUInt8,
  kUInt16,
  kInt8,
  kInt16,
  kUInt32,
  kInt32,
  kFloat32,
  kFloat64
};

/// Bytes of one sample.
[[nodiscard]] std::size_t rawSampleBytes(RawSampleType type);
/// "uint8", "uint16", "int8", "int16", "uint32", "int32", "float32", "float64".
[[nodiscard]] std::string_view rawSampleTypeName(RawSampleType type);
/// The type of one of those names, also "float" and "double"; nothing for others.
[[nodiscard]] std::optional<RawSampleType> rawSampleTypeFromName(std::string_view name);

/// Layout of a raw volume file: voxels x fastest, optionally preceded by a vendor header and
/// followed by a footer, both of which are skipped.
struct RawLayout {
  RawLayout() = default;
  RawLayout(const std::array<std::int64_t, 3>& dims_in, const VoxelSize& voxel_size_in,
            SampleType sample_type_in = SampleType::kUInt16,
            std::endian byte_order_in = std::endian::little,
            std::optional<std::uint64_t> header_bytes_in = std::nullopt)
      : dims(dims_in),
        voxel_size(voxel_size_in),
        sample_type(sample_type_in),
        byte_order(byte_order_in),
        header_bytes(header_bytes_in) {}

  std::array<std::int64_t, 3> dims{0, 0, 0};
  VoxelSize voxel_size;
  SampleType sample_type = SampleType::kUInt16;
  std::endian byte_order = std::endian::little;
  /// Bytes before the voxel data. When unset, everything in the file beyond the voxel data is
  /// taken to be a header at the start (the common case for proprietary CT raw files).
  std::optional<std::uint64_t> header_bytes;
  /// The samples of the file when they are not unsigned 8 or 16 bit (ADR 0044); `sample_type`
  /// then gives nothing. See storedSampleType.
  std::optional<RawSampleType> stored_type;
  /// 32-bit and float samples: the values mapped to grey 0 and 65535. Unset: estimated from 33
  /// slices spread over the volume (over its first quarter when it is compressed and holds more
  /// than 4 GiB), widened by 10 % on each side; 32-bit integers within 0..65535 are then kept as
  /// they are.
  std::optional<std::array<double, 2>> value_range;
  /// Bytes of the file before compressed voxel data, as in formats whose text header is followed
  /// by gzip or zlib data in the same file. `header_bytes` counts in the decompressed data.
  std::uint64_t compressed_offset = 0;
};

/// The type of the samples of `layout`: `stored_type`, else `sample_type`.
[[nodiscard]] RawSampleType storedSampleType(const RawLayout& layout);

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
  [[nodiscard]] SampleType sampleType() const override;
  /// Identity for unsigned 8- and 16-bit samples, else how the samples became grey values.
  [[nodiscard]] ValueMapping valueMapping() const override;
  /// 32-bit and float samples: the values mapped to grey 0 and 65535, as given or estimated;
  /// nothing when the samples are kept exactly.
  [[nodiscard]] std::optional<std::array<double, 2>> valueRange() const;
  /// Samples read so far that lay outside the value range or were not finite (each region
  /// counted once); they became grey 0 or 65535 (not a number: 0).
  [[nodiscard]] std::uint64_t clippedValues() const;
  /// Header size in bytes, as given or detected.
  [[nodiscard]] std::uint64_t headerBytes() const { return header_bytes_; }
  /// True when the file lies on a network share (SMB, NFS, a mapped network drive): the import
  /// then reads it once, slice by slice, instead of twice in small pieces (ADR 0020).
  [[nodiscard]] bool slowRandomAccess() const override { return on_network_share_; }
  [[nodiscard]] std::string identity() const override { return identity_; }
  /// Lets the pages read so far go from the process's memory; they are read again when needed.
  void releaseMemory() const override;
  void readRegion(const Box& box, std::span<std::uint16_t> out) const override;

 private:
  struct Mapping;
  std::unique_ptr<Mapping> mapping_;
  std::unique_ptr<detail::RawSamples> samples_;
  RawLayout layout_;
  std::uint64_t header_bytes_ = 0;
  bool on_network_share_ = false;
  std::string identity_;
};

/// Raw volume file compressed with gzip (`.raw.gz`) or zlib, decompressed with zlib as it is read
/// (ADR 0020). gzip can only be read forward, so this source is read in one pass in slice order;
/// reading a region before the last one starts again at the beginning of the file. A header in
/// the uncompressed data is skipped when `header_bytes` is given (default 0: the size of the
/// uncompressed data is not known without reading it all). Concatenated gzip members (pigz) are
/// read as one stream.
class GzipRawSource final : public VolumeSource {
 public:
  GzipRawSource(const std::filesystem::path& path, const RawLayout& layout);
  ~GzipRawSource() override;

  [[nodiscard]] std::array<std::int64_t, 3> dims() const override { return layout_.dims; }
  [[nodiscard]] VoxelSize voxelSize() const override { return layout_.voxel_size; }
  [[nodiscard]] SampleType sampleType() const override;
  [[nodiscard]] ValueMapping valueMapping() const override;
  [[nodiscard]] std::optional<std::array<double, 2>> valueRange() const;
  [[nodiscard]] std::uint64_t clippedValues() const;
  [[nodiscard]] std::uint64_t headerBytes() const { return header_bytes_; }
  [[nodiscard]] std::string identity() const override { return identity_; }
  [[nodiscard]] bool slowRandomAccess() const override { return true; }
  [[nodiscard]] bool sequentialAccess() const override { return true; }
  void readRegion(const Box& box, std::span<std::uint16_t> out) const override;

 private:
  struct Stream;
  std::unique_ptr<Stream> stream_;
  std::unique_ptr<detail::RawSamples> samples_;
  RawLayout layout_;
  std::uint64_t header_bytes_ = 0;
  std::string identity_;
};

/// Whether `path` is a gzip file (it starts with the gzip magic bytes 1f 8b).
[[nodiscard]] bool isGzipFile(const std::filesystem::path& path);

/// The JSON sidecar of a raw volume: `scan.json` for `scan.raw` and for `scan.raw.gz`.
[[nodiscard]] std::filesystem::path rawSidecarPath(const std::filesystem::path& raw);

/// The layout of a raw volume file without a sidecar, as detectRawLayout found it (ADR 0039).
struct RawLayoutGuess {
  /// Dimensions, sample type, byte order and header size; the voxel size when the name gives it.
  RawLayout layout;
  bool voxel_size_known = false;
  /// Where the dimensions come from: "file name" or "grey values".
  std::string dims_from;
  /// How clearly the grey values show the rows and slices: 1 - (difference of neighbours along
  /// y and z) / (difference of voxels far apart), the worse of the two; 1 for dimensions from the
  /// file name that the grey values do not contradict.
  double confidence = 0.0;
};

/// Finds the layout of a raw volume file that has no sidecar (ADR 0039). The name (of the file,
/// else of its directory) may give the dimensions ("512x512x400"), the sample type ("16bit",
/// "uint8"), the byte order ("bigendian") and the voxel size ("0.05mm", "50um"). What it does
/// not give is read from the grey values: the sample type and byte order from which bytes vary
/// smoothly, the row length and the slice size as the offsets at which voxels are most alike
/// (neighbours along y and z), and the number of slices from the file size; what is left over,
/// less than a slice and at most 1 MiB, is a header at the start. Gives nothing when the grey
/// values show no rows and slices, such as noise or a compressed file whose name says nothing.
[[nodiscard]] std::optional<RawLayoutGuess> detectRawLayout(const std::filesystem::path& path);

/// The voxel size a file name (or its directory's) gives, as detectRawLayout reads it.
[[nodiscard]] std::optional<VoxelSize> voxelSizeFromName(const std::filesystem::path& path);

/// A raw volume file: gzip-compressed (GzipRawSource, also when compressed data starts at
/// `layout.compressed_offset`) or plain (MappedRawSource). `header_bytes` receives the header size
/// skipped.
[[nodiscard]] std::unique_ptr<VolumeSource> openRawVolume(const std::filesystem::path& path,
                                                          const RawLayout& layout,
                                                          std::uint64_t* header_bytes = nullptr);

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
  /// 8 bit when all parts are.
  [[nodiscard]] SampleType sampleType() const override;
  [[nodiscard]] bool slowRandomAccess() const override;
  [[nodiscard]] bool sequentialAccess() const override;
  void releaseMemory() const override;
  void readRegion(const Box& box, std::span<std::uint16_t> out) const override;

  [[nodiscard]] int axis() const { return axis_; }
  [[nodiscard]] std::string identity() const override;
  [[nodiscard]] std::size_t partCount() const { return parts_.size(); }
  [[nodiscard]] const VolumeSource& part(std::size_t i) const { return *parts_.at(i); }
  /// First voxel of part `i` along `axis`.
  [[nodiscard]] std::int64_t partStart(std::size_t i) const { return starts_[i]; }

 private:
  std::vector<std::unique_ptr<VolumeSource>> parts_;
  int axis_ = 2;
  std::vector<std::int64_t> starts_;  // one more than parts: the last is dims_[axis_]
  std::array<std::int64_t, 3> dims_{};
};

}  // namespace voxelsieve
