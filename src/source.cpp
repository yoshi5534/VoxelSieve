#include "voxelsieve/source.hpp"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <bit>
#include <boost/iostreams/device/mapped_file.hpp>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "detail/mapped_read.hpp"
#include "detail/network.hpp"
#include "detail/pages.hpp"
#include "voxelsieve/telemetry.hpp"

namespace voxelsieve {
namespace {

void checkRegion(const Box& box, const std::array<std::int64_t, 3>& dims,
                 std::span<std::uint16_t> out) {
  for (std::size_t i = 0; i < 3; ++i) {
    if (box.min[i] < 0 || box.max[i] > dims[i] || box.min[i] > box.max[i]) {
      throw std::out_of_range("Region outside the volume");
    }
  }
  if (static_cast<std::int64_t>(out.size()) != box.voxelCount()) {
    throw std::invalid_argument("Output buffer size does not match the region");
  }
}

/// Copies `box` row by row from a dense x-fastest array with dimensions `dims`.
void copyRows(const std::uint16_t* data, const std::array<std::int64_t, 3>& dims, const Box& box,
              std::span<std::uint16_t> out) {
  const auto row_length = static_cast<std::size_t>(box.size(0));
  std::size_t offset = 0;
  for (std::int64_t z = box.min[2]; z < box.max[2]; ++z) {
    for (std::int64_t y = box.min[1]; y < box.max[1]; ++y) {
      const auto source = static_cast<std::size_t>(box.min[0] + dims[0] * (y + dims[1] * z));
      std::memcpy(&out[offset], data + source, row_length * sizeof(std::uint16_t));
      offset += row_length;
    }
  }
}

/// One sample of type T from `p`, its bytes reversed when the file's byte order is not the
/// machine's. memcpy handles samples that are not aligned (headers of odd length).
template <class T>
T loadSample(const unsigned char* p, bool swap) {
  std::array<unsigned char, sizeof(T)> bytes{};
  std::memcpy(bytes.data(), p, sizeof(T));
  if (swap) {
    std::reverse(bytes.begin(), bytes.end());
  }
  T value{};
  std::memcpy(&value, bytes.data(), sizeof(T));
  return value;
}

/// Grey value of `value` under `mapping`, clipped to 0..65535; values outside and not a number
/// are counted in `outside`.
std::uint16_t mapToGrey(double value, const ValueMapping& mapping, std::uint64_t& outside) {
  const double grey = std::round(mapping.toGrey(value));
  if (!(grey >= 0.0 && grey <= 65535.0)) {
    ++outside;
    if (std::isnan(grey)) {
      return 0;
    }
  }
  return static_cast<std::uint16_t>(std::clamp(grey, 0.0, 65535.0));
}

template <class T>
void convertTyped(const unsigned char* samples, std::size_t count, bool swap, bool mapped,
                  const ValueMapping& mapping, std::uint16_t* target, std::uint64_t& outside) {
  for (std::size_t i = 0; i < count; ++i) {
    const T value = loadSample<T>(samples + i * sizeof(T), swap);
    if constexpr (std::is_same_v<T, std::int8_t>) {
      target[i] = static_cast<std::uint16_t>(value + 128);
    } else if constexpr (std::is_same_v<T, std::int16_t>) {
      target[i] = static_cast<std::uint16_t>(value + 32768);
    } else if constexpr (std::is_integral_v<T>) {
      if (mapped) {
        target[i] = mapToGrey(static_cast<double>(value), mapping, outside);
      } else if (value < 0 || value > 65535) {
        ++outside;
        target[i] = value < 0 ? 0 : 65535;
      } else {
        target[i] = static_cast<std::uint16_t>(value);
      }
    } else {
      target[i] = mapToGrey(static_cast<double>(value), mapping, outside);
    }
  }
}

}  // namespace

namespace detail {

/// How the samples of a raw file become 16-bit grey values (ADR 0040), and the count of values
/// that were clipped.
struct RawSamples {
  RawSampleType type = RawSampleType::kUInt16;
  bool swap = false;
  /// Wide samples mapped linearly over `range`; false for the types that are kept exactly.
  bool mapped = false;
  std::array<double, 2> range{0.0, 65535.0};
  ValueMapping mapping;
  mutable std::mutex mutex;
  mutable std::set<std::array<std::int64_t, 6>> counted;  // regions whose clipped values count
  mutable std::uint64_t clipped = 0;

  [[nodiscard]] std::size_t bytes() const { return rawSampleBytes(type); }

  /// Sets up the mapping. `sample` gives the values of slice z (or a part of it) for the
  /// estimate of the range; slices are taken from 0 to `last` (inclusive).
  void setUp(const RawLayout& layout, std::int64_t last,
             const std::function<std::vector<double>(std::int64_t)>& sample) {
    type = storedSampleType(layout);
    swap = rawSampleBytes(type) > 1 && layout.byte_order != std::endian::native;
    const bool wide = type == RawSampleType::kUInt32 || type == RawSampleType::kInt32 ||
                      type == RawSampleType::kFloat32 || type == RawSampleType::kFloat64;
    if (type == RawSampleType::kInt8) {
      mapping = {-128.0, 1.0};
    } else if (type == RawSampleType::kInt16) {
      mapping = {-32768.0, 1.0};
    }
    if (!wide) {
      if (layout.value_range) {
        throw std::invalid_argument("A value range applies only to 32-bit and float samples");
      }
      return;
    }
    const bool is_float = type == RawSampleType::kFloat32 || type == RawSampleType::kFloat64;
    if (layout.value_range) {
      range = *layout.value_range;
      mapped = true;
    } else {
      constexpr std::int64_t kSampleSlices = 33;
      const std::int64_t count = std::min(kSampleSlices, last + 1);
      std::vector<std::array<double, 2>> ranges(static_cast<std::size_t>(count));
      for (std::int64_t i = 0; i < count; ++i) {
        const std::int64_t z = count == 1 ? 0 : i * last / (count - 1);
        std::array<double, 2> found{std::numeric_limits<double>::infinity(),
                                    -std::numeric_limits<double>::infinity()};
        for (const double v : sample(z)) {
          if (std::isfinite(v)) {
            found = {std::min(found[0], v), std::max(found[1], v)};
          }
        }
        ranges[static_cast<std::size_t>(i)] = found;
      }
      std::array<double, 2> found{std::numeric_limits<double>::infinity(),
                                  -std::numeric_limits<double>::infinity()};
      for (const auto& r : ranges) {
        found = {std::min(found[0], r[0]), std::max(found[1], r[1])};
      }
      if (!(found[0] <= found[1])) {
        throw std::runtime_error("The raw file holds no finite values");
      }
      if (!is_float && found[0] >= 0.0 && found[1] <= 65535.0) {
        return;  // 32-bit integers that fit: kept exactly
      }
      const double margin = found[1] > found[0] ? 0.1 * (found[1] - found[0]) : 0.5;
      range = {found[0] - margin, found[1] + margin};
      if (!is_float) {
        range[0] = std::floor(range[0]);
        range[1] = std::ceil(range[1]);
      }
      mapped = true;
    }
    if (!(std::isfinite(range[0]) && std::isfinite(range[1]) && range[0] < range[1])) {
      throw std::invalid_argument("The value range must be two finite numbers, low before high");
    }
    mapping = {range[0], (range[1] - range[0]) / 65535.0};
  }

  /// Converts `count` samples; values outside the range are added to `outside`.
  void convert(const unsigned char* samples, std::size_t count, std::uint16_t* target,
               std::uint64_t& outside) const {
    switch (type) {
      case RawSampleType::kUInt8:
        std::copy(samples, samples + count, target);
        return;
      case RawSampleType::kUInt16:
        std::memcpy(target, samples, count * sizeof(std::uint16_t));
        if (swap) {
          std::transform(target, target + count, target, [](std::uint16_t v) {
            return static_cast<std::uint16_t>((v >> 8U) | (v << 8U));
          });
        }
        return;
      case RawSampleType::kInt8:
        convertTyped<std::int8_t>(samples, count, swap, mapped, mapping, target, outside);
        return;
      case RawSampleType::kInt16:
        convertTyped<std::int16_t>(samples, count, swap, mapped, mapping, target, outside);
        return;
      case RawSampleType::kUInt32:
        convertTyped<std::uint32_t>(samples, count, swap, mapped, mapping, target, outside);
        return;
      case RawSampleType::kInt32:
        convertTyped<std::int32_t>(samples, count, swap, mapped, mapping, target, outside);
        return;
      case RawSampleType::kFloat32:
        convertTyped<float>(samples, count, swap, mapped, mapping, target, outside);
        return;
      case RawSampleType::kFloat64:
        convertTyped<double>(samples, count, swap, mapped, mapping, target, outside);
        return;
    }
  }

  /// The values of `count` samples, for the estimate of the range.
  [[nodiscard]] std::vector<double> values(const unsigned char* samples, std::size_t count) const {
    std::vector<double> result(count);
    for (std::size_t i = 0; i < count; ++i) {
      const unsigned char* p = samples + i * bytes();
      switch (type) {
        case RawSampleType::kUInt32:
          result[i] = loadSample<std::uint32_t>(p, swap);
          break;
        case RawSampleType::kInt32:
          result[i] = loadSample<std::int32_t>(p, swap);
          break;
        case RawSampleType::kFloat32:
          result[i] = loadSample<float>(p, swap);
          break;
        case RawSampleType::kFloat64:
          result[i] = loadSample<double>(p, swap);
          break;
        default:
          result[i] = 0.0;
      }
    }
    return result;
  }

  /// Adds the clipped values of a region read, once per region.
  void count(const Box& box, std::uint64_t outside) const {
    if (outside == 0) {
      return;
    }
    const std::scoped_lock lock(mutex);
    if (counted.insert({box.min[0], box.min[1], box.min[2], box.max[0], box.max[1], box.max[2]})
            .second) {
      clipped += outside;
    }
  }

  [[nodiscard]] SampleType greyType() const {
    return type == RawSampleType::kUInt8 || type == RawSampleType::kInt8 ? SampleType::kUInt8
                                                                         : SampleType::kUInt16;
  }
};

}  // namespace detail

std::size_t rawSampleBytes(RawSampleType type) {
  switch (type) {
    case RawSampleType::kUInt8:
    case RawSampleType::kInt8:
      return 1;
    case RawSampleType::kUInt16:
    case RawSampleType::kInt16:
      return 2;
    case RawSampleType::kUInt32:
    case RawSampleType::kInt32:
    case RawSampleType::kFloat32:
      return 4;
    case RawSampleType::kFloat64:
      return 8;
  }
  return 2;
}

namespace {

constexpr std::array<std::pair<RawSampleType, std::string_view>, 8> kSampleTypeNames{{
    {RawSampleType::kUInt8, "uint8"},
    {RawSampleType::kUInt16, "uint16"},
    {RawSampleType::kInt8, "int8"},
    {RawSampleType::kInt16, "int16"},
    {RawSampleType::kUInt32, "uint32"},
    {RawSampleType::kInt32, "int32"},
    {RawSampleType::kFloat32, "float32"},
    {RawSampleType::kFloat64, "float64"},
}};

}  // namespace

std::string_view rawSampleTypeName(RawSampleType type) {
  for (const auto& [t, name] : kSampleTypeNames) {
    if (t == type) {
      return name;
    }
  }
  return "uint16";
}

std::optional<RawSampleType> rawSampleTypeFromName(std::string_view name) {
  if (name == "float") {
    return RawSampleType::kFloat32;
  }
  if (name == "double") {
    return RawSampleType::kFloat64;
  }
  for (const auto& [t, n] : kSampleTypeNames) {
    if (n == name) {
      return t;
    }
  }
  return std::nullopt;
}

RawSampleType storedSampleType(const RawLayout& layout) {
  if (layout.stored_type) {
    return *layout.stored_type;
  }
  return layout.sample_type == SampleType::kUInt8 ? RawSampleType::kUInt8 : RawSampleType::kUInt16;
}

void checkVolumeDims(const std::array<std::int64_t, 3>& dims, std::string_view what) {
  for (const std::int64_t d : dims) {
    if (d <= 0 || d > kMaxVolumeEdge) {
      throw std::invalid_argument(std::string(what) + ": dimensions must be between 1 and " +
                                  std::to_string(kMaxVolumeEdge) + " voxels per axis, not " +
                                  std::to_string(dims[0]) + " x " + std::to_string(dims[1]) +
                                  " x " + std::to_string(dims[2]));
    }
  }
}

void MemorySource::readRegion(const Box& box, std::span<std::uint16_t> out) const {
  checkRegion(box, volume_.dims, out);
  copyRows(volume_.data.data(), volume_.dims, box, out);
}

InterpolatedSource::InterpolatedSource(const VolumeSource& base, int factor)
    : base_(base), factor_(factor) {
  if (factor < 1) {
    throw std::invalid_argument("The interpolation factor must be at least 1");
  }
}

std::array<std::int64_t, 3> InterpolatedSource::dims() const {
  auto dims = base_.dims();
  dims[2] = (dims[2] - 1) * factor_ + 1;
  return dims;
}

VoxelSize InterpolatedSource::voxelSize() const {
  VoxelSize size = base_.voxelSize();
  if (factor_ > 1) {
    size.pitch_mm[2] /= static_cast<double>(factor_);
    size.slice_thickness_mm = 0.0;
  }
  return size;
}

void InterpolatedSource::readRegion(const Box& box, std::span<std::uint16_t> out) const {
  checkRegion(box, dims(), out);
  if (factor_ == 1) {
    base_.readRegion(box, out);
    return;
  }
  // The base slices around the region, read at once, then blended per output slice.
  const std::int64_t first = box.min[2] / factor_;
  const std::int64_t last = std::min(base_.dims()[2] - 1, (box.max[2] - 1 + factor_ - 1) / factor_);
  Box read = box;
  read.min[2] = first;
  read.max[2] = last + 1;
  std::vector<std::uint16_t> slices(static_cast<std::size_t>(read.voxelCount()));
  base_.readRegion(read, slices);
  const auto plane = static_cast<std::size_t>(box.size(0) * box.size(1));
  for (std::int64_t z = box.min[2]; z < box.max[2]; ++z) {
    const std::int64_t below = z / factor_;
    const double t = static_cast<double>(z - below * factor_) / static_cast<double>(factor_);
    const auto* low = slices.data() + static_cast<std::size_t>(below - first) * plane;
    const auto* high = t > 0.0 ? low + plane : low;
    auto* target = out.data() + static_cast<std::size_t>(z - box.min[2]) * plane;
    for (std::size_t i = 0; i < plane; ++i) {
      target[i] = static_cast<std::uint16_t>(
          std::lround((1.0 - t) * static_cast<double>(low[i]) + t * static_cast<double>(high[i])));
    }
  }
}

struct MappedRawSource::Mapping {
  boost::iostreams::mapped_file_source file;
  std::string path;
};

MappedRawSource::MappedRawSource(const std::filesystem::path& path,
                                 const std::array<std::int64_t, 3>& dims,
                                 const VoxelSize& voxel_size)
    : MappedRawSource(path, RawLayout{dims, voxel_size, SampleType::kUInt16, std::endian::little,
                                      std::uint64_t{0}}) {}

MappedRawSource::MappedRawSource(const std::filesystem::path& path, const RawLayout& layout)
    : mapping_(std::make_unique<Mapping>()),
      samples_(std::make_unique<detail::RawSamples>()),
      layout_(layout),
      identity_(inputIdentity(path)) {
  checkVolumeDims(layout.dims, path.string());
  const std::size_t sample_bytes = rawSampleBytes(storedSampleType(layout));
  const std::uintmax_t payload =
      static_cast<std::uintmax_t>(layout.dims[0] * layout.dims[1] * layout.dims[2]) * sample_bytes;
  const std::uintmax_t file_bytes = std::filesystem::file_size(path);
  if (file_bytes < payload) {
    throw std::runtime_error(path.string() + " is smaller than its voxel data (" +
                             std::to_string(file_bytes) + " < " + std::to_string(payload) +
                             " bytes); check dimensions and sample type");
  }
  header_bytes_ = layout.header_bytes.value_or(file_bytes - payload);
  if (header_bytes_ + payload > file_bytes) {
    throw std::runtime_error("Header of " + std::to_string(header_bytes_) +
                             " bytes plus voxel data "
                             "exceeds the size of " +
                             path.string());
  }
  mapping_->path = path.string();
  mapping_->file.open(path.string());
  if (!mapping_->file.is_open()) {
    throw std::runtime_error("Cannot map " + path.string());
  }
  on_network_share_ = detail::onNetworkShare(path);
  // Up to a million samples of a slice, spread over it, for the estimate of the value range.
  const auto slice = static_cast<std::size_t>(layout.dims[0] * layout.dims[1]);
  const std::size_t step = std::max<std::size_t>(1, slice >> 20U);
  samples_->setUp(layout_, layout.dims[2] - 1, [&](std::int64_t z) {
    const auto* data =
        reinterpret_cast<const unsigned char*>(mapping_->file.data()) + header_bytes_;
    std::vector<double> values;
    detail::readMapped(mapping_->path, [&] {
      for (std::size_t i = 0; i < slice; i += step) {
        const auto index = static_cast<std::size_t>(z) * slice + i;
        values.push_back(samples_->values(data + index * sample_bytes, 1).front());
      }
    });
    return values;
  });
}

MappedRawSource::~MappedRawSource() = default;

SampleType MappedRawSource::sampleType() const { return samples_->greyType(); }
ValueMapping MappedRawSource::valueMapping() const { return samples_->mapping; }
std::optional<std::array<double, 2>> MappedRawSource::valueRange() const {
  return samples_->mapped ? std::optional(samples_->range) : std::nullopt;
}
std::uint64_t MappedRawSource::clippedValues() const {
  const std::scoped_lock lock(samples_->mutex);
  return samples_->clipped;
}

void MappedRawSource::releaseMemory() const {
  detail::releaseMappedPages(mapping_->file.data(), mapping_->file.size());
}

void MappedRawSource::readRegion(const Box& box, std::span<std::uint16_t> out) const {
  const auto& dims = layout_.dims;
  checkRegion(box, dims, out);
  const std::size_t bytes = samples_->bytes();
  const auto* data = reinterpret_cast<const unsigned char*>(mapping_->file.data()) + header_bytes_;
  const auto row_length = static_cast<std::size_t>(box.size(0));
  // Read ahead: the rows of each slice of the region lie in one span of the file.
  for (std::int64_t z = box.min[2]; z < box.max[2]; ++z) {
    const auto first = static_cast<std::size_t>(box.min[0] + dims[0] * (box.min[1] + dims[1] * z));
    const auto last =
        static_cast<std::size_t>(box.max[0] + dims[0] * (box.max[1] - 1 + dims[1] * z));
    detail::prefetchMappedPages(data + first * bytes, (last - first) * bytes);
  }
  std::size_t offset = 0;
  std::uint64_t outside = 0;
  // An I/O error while reading the mapping becomes an exception instead of ending the process.
  detail::readMapped(mapping_->path, [&] {
    for (std::int64_t z = box.min[2]; z < box.max[2]; ++z) {
      for (std::int64_t y = box.min[1]; y < box.max[1]; ++y) {
        const auto first = static_cast<std::size_t>(box.min[0] + dims[0] * (y + dims[1] * z));
        samples_->convert(data + first * bytes, row_length, &out[offset], outside);
        offset += row_length;
      }
    }
  });
  samples_->count(box, outside);
  // Reads of a mapping are page faults, which no process counter reports as bytes read.
  countRead(CountedRead::kMappedFile, offset * bytes);
}

/// The decompressed bytes of a gzip file, read forward; going back starts again.
struct GzipRawSource::Stream {
  static constexpr std::size_t kChunk = std::size_t{1} << 20U;

  Stream(std::filesystem::path file_path, std::uint64_t start)
      : path(std::move(file_path)), input(kChunk), scratch(kChunk), first_byte(start) {
    file.open(path, std::ios::binary);
    if (!file) {
      throw std::runtime_error("Cannot open " + path.string());
    }
    file.seekg(static_cast<std::streamoff>(first_byte));
    if (inflateInit2(&z, 32 + MAX_WBITS) != Z_OK) {  // 32: a gzip or a zlib header
      throw std::runtime_error("Cannot initialise zlib");
    }
  }
  Stream(const Stream&) = delete;
  Stream& operator=(const Stream&) = delete;
  Stream(Stream&&) = delete;
  Stream& operator=(Stream&&) = delete;
  ~Stream() { inflateEnd(&z); }

  void rewind() {
    file.clear();
    file.seekg(static_cast<std::streamoff>(first_byte));
    inflateReset(&z);
    z.avail_in = 0;
    position = 0;
  }

  /// Moves to decompressed byte `offset`, reading forward or starting again.
  void seek(std::uint64_t offset) {
    if (offset < position) {
      rewind();
    }
    while (position < offset) {
      const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(offset - position, kChunk));
      read(scratch.data(), n);
    }
  }

  void read(unsigned char* out, std::size_t count) {
    z.next_out = out;
    z.avail_out = static_cast<uInt>(count);
    while (z.avail_out > 0) {
      if (z.avail_in == 0) {
        file.read(reinterpret_cast<char*>(input.data()), static_cast<std::streamsize>(kChunk));
        z.next_in = input.data();
        z.avail_in = static_cast<uInt>(file.gcount());
        if (z.avail_in == 0) {
          throw std::runtime_error(path.string() +
                                   " ends before its voxel data; check dimensions, sample type "
                                   "and header");
        }
      }
      const int status = inflate(&z, Z_NO_FLUSH);
      if (status == Z_STREAM_END) {
        inflateReset(&z);  // another gzip member may follow (pigz, concatenated files)
      } else if (status != Z_OK && status != Z_BUF_ERROR) {
        throw std::runtime_error("Cannot decompress " + path.string() + ": " +
                                 (z.msg != nullptr ? z.msg : "zlib error"));
      }
    }
    position += count;
  }

  std::filesystem::path path;
  std::ifstream file;
  z_stream z{};
  std::vector<unsigned char> input;
  std::vector<unsigned char> scratch;
  std::uint64_t position = 0;    // decompressed bytes read
  std::uint64_t first_byte = 0;  // of the compressed data in the file
  std::mutex mutex;
};

GzipRawSource::GzipRawSource(const std::filesystem::path& path, const RawLayout& layout)
    : stream_(std::make_unique<Stream>(path, layout.compressed_offset)),
      samples_(std::make_unique<detail::RawSamples>()),
      layout_(layout),
      header_bytes_(layout.header_bytes.value_or(0)),
      identity_(inputIdentity(path)) {
  checkVolumeDims(layout.dims, path.string());
  // deflate packs at most about 1032 bytes into one: a file that would hold more voxels than
  // that is refused before any of it is allocated (a damaged or wrong header).
  const std::uintmax_t file_bytes = std::filesystem::file_size(path);
  const std::uintmax_t compressed =
      file_bytes > layout.compressed_offset ? file_bytes - layout.compressed_offset : 0;
  const auto voxels = static_cast<std::uintmax_t>(layout.dims[0] * layout.dims[1] * layout.dims[2]);
  if (voxels * rawSampleBytes(storedSampleType(layout)) / 1100 > compressed) {
    throw std::runtime_error(path.string() + " is too small for compressed data of " +
                             std::to_string(voxels) + " voxels; check dimensions and sample type");
  }
  // The value range from slices spread over the volume; of a large file from the first quarter
  // of the slices only, since going further would mean decompressing much of it twice.
  const auto slice = static_cast<std::size_t>(layout.dims[0] * layout.dims[1]);
  const std::size_t bytes = rawSampleBytes(storedSampleType(layout));
  constexpr std::uintmax_t kWholeVolumeBytes = std::uintmax_t{4} << 30U;
  const bool whole = voxels * bytes <= kWholeVolumeBytes;
  samples_->setUp(layout_,
                  whole ? layout.dims[2] - 1 : std::max<std::int64_t>(0, (layout.dims[2] - 1) / 4),
                  [&](std::int64_t z) {
                    std::vector<unsigned char> data(slice * bytes);
                    stream_->seek(header_bytes_ + static_cast<std::uint64_t>(z) * slice * bytes);
                    stream_->read(data.data(), data.size());
                    return samples_->values(data.data(), slice);
                  });
  stream_->rewind();
}

GzipRawSource::~GzipRawSource() = default;

SampleType GzipRawSource::sampleType() const { return samples_->greyType(); }
ValueMapping GzipRawSource::valueMapping() const { return samples_->mapping; }
std::optional<std::array<double, 2>> GzipRawSource::valueRange() const {
  return samples_->mapped ? std::optional(samples_->range) : std::nullopt;
}
std::uint64_t GzipRawSource::clippedValues() const {
  const std::scoped_lock lock(samples_->mutex);
  return samples_->clipped;
}

void GzipRawSource::readRegion(const Box& box, std::span<std::uint16_t> out) const {
  const auto& dims = layout_.dims;
  checkRegion(box, dims, out);
  const std::size_t bytes = samples_->bytes();
  // Whole rows of whole slices lie one after another in the file: one read per slice.
  const bool whole_slices = box.size(0) == dims[0] && box.size(1) == dims[1];
  const auto run = static_cast<std::size_t>(whole_slices ? dims[0] * dims[1] : box.size(0));
  std::vector<unsigned char> samples(run * bytes);
  std::uint64_t outside = 0;
  {
    const std::scoped_lock lock(stream_->mutex);
    std::size_t offset = 0;
    for (std::int64_t z = box.min[2]; z < box.max[2]; ++z) {
      for (std::int64_t y = box.min[1]; y < box.max[1]; y += whole_slices ? dims[1] : 1) {
        const auto first = static_cast<std::uint64_t>(box.min[0] + dims[0] * (y + dims[1] * z));
        stream_->seek(header_bytes_ + first * bytes);
        stream_->read(samples.data(), samples.size());
        samples_->convert(samples.data(), run, &out[offset], outside);
        offset += run;
      }
    }
  }
  samples_->count(box, outside);
}

bool isGzipFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::array<char, 2> magic{};
  return in.read(magic.data(), magic.size()) && static_cast<unsigned char>(magic[0]) == 0x1f &&
         static_cast<unsigned char>(magic[1]) == 0x8b;
}

std::filesystem::path rawSidecarPath(const std::filesystem::path& raw) {
  std::filesystem::path sidecar = raw;
  if (sidecar.extension() == ".gz") {
    sidecar.replace_extension();
  }
  return sidecar.replace_extension(".json");
}

std::unique_ptr<VolumeSource> openRawVolume(const std::filesystem::path& path,
                                            const RawLayout& layout, std::uint64_t* header_bytes) {
  if (layout.compressed_offset > 0 || isGzipFile(path)) {
    auto source = std::make_unique<GzipRawSource>(path, layout);
    if (header_bytes != nullptr) {
      *header_bytes = source->headerBytes();
    }
    return source;
  }
  auto source = std::make_unique<MappedRawSource>(path, layout);
  if (header_bytes != nullptr) {
    *header_bytes = source->headerBytes();
  }
  return source;
}

void PhantomSource::readRegion(const Box& box, std::span<std::uint16_t> out) const {
  checkRegion(box, spec_.dims, out);
  std::size_t offset = 0;
  for (std::int64_t z = box.min[2]; z < box.max[2]; ++z) {
    for (std::int64_t y = box.min[1]; y < box.max[1]; ++y) {
      for (std::int64_t x = box.min[0]; x < box.max[0]; ++x) {
        out[offset++] = phantomValue(spec_, x, y, z);
      }
    }
  }
}

std::string ConcatSource::identity() const {
  std::string identity;
  for (const auto& part : parts_) {
    identity += part->identity() + ";";
  }
  return identity;
}

std::string inputIdentity(const std::filesystem::path& path) {
  std::error_code error;
  const auto ticks = [](const std::filesystem::file_time_type& time) {
    // Microseconds in 64 bits: the count of the clock itself has 128 bits on macOS.
    using Microseconds = std::chrono::duration<std::int64_t, std::micro>;
    return std::to_string(
        std::chrono::duration_cast<Microseconds>(time.time_since_epoch()).count());
  };
  const std::string identity = std::filesystem::absolute(path, error).generic_string();
  if (std::filesystem::is_directory(path, error)) {
    std::uintmax_t files = 0;
    std::uintmax_t bytes = 0;
    std::filesystem::file_time_type latest{};
    for (std::filesystem::directory_iterator it(path, error), end; !error && it != end;
         it.increment(error)) {
      std::error_code ignored;
      if (!it->is_regular_file(ignored)) {
        continue;
      }
      ++files;
      bytes += it->file_size(ignored);
      latest = std::max(latest, it->last_write_time(ignored));
    }
    return identity + " | " + std::to_string(files) + " files | " + std::to_string(bytes) +
           " bytes | " + ticks(latest);
  }
  const auto bytes = std::filesystem::file_size(path, error);
  const auto time = std::filesystem::last_write_time(path, error);
  return identity + " | " + std::to_string(error ? 0 : bytes) + " bytes | " + ticks(time);
}

ConcatSource::ConcatSource(std::vector<std::unique_ptr<VolumeSource>> parts, int axis)
    : parts_(std::move(parts)), axis_(axis) {
  if (parts_.empty()) {
    throw std::invalid_argument("ConcatSource needs at least one part");
  }
  if (axis < 0 || axis > 2) {
    throw std::invalid_argument("ConcatSource axis must be 0, 1 or 2");
  }
  const auto a = static_cast<std::size_t>(axis);
  dims_ = parts_.front()->dims();
  dims_[a] = 0;
  for (const auto& part : parts_) {
    const auto d = part->dims();
    for (std::size_t i = 0; i < 3; ++i) {
      if (i != a && d[i] != dims_[i]) {
        throw std::invalid_argument("Parts differ in size across the joining axis");
      }
    }
    if (!(part->voxelSize() == parts_.front()->voxelSize())) {
      throw std::invalid_argument("Parts differ in voxel size");
    }
    if (!(part->valueMapping() == parts_.front()->valueMapping())) {
      throw std::invalid_argument(
          "Parts map their values to 16 bit differently; give all parts the same value range");
    }
    starts_.push_back(dims_[a]);
    dims_[a] += d[a];
  }
  starts_.push_back(dims_[a]);
}

SampleType ConcatSource::sampleType() const {
  const bool all_8_bit = std::all_of(parts_.begin(), parts_.end(), [](const auto& part) {
    return part->sampleType() == SampleType::kUInt8;
  });
  return all_8_bit ? SampleType::kUInt8 : SampleType::kUInt16;
}

bool ConcatSource::slowRandomAccess() const {
  return std::any_of(parts_.begin(), parts_.end(),
                     [](const auto& part) { return part->slowRandomAccess(); });
}

bool ConcatSource::sequentialAccess() const {
  return std::any_of(parts_.begin(), parts_.end(),
                     [](const auto& part) { return part->sequentialAccess(); });
}

void ConcatSource::releaseMemory() const {
  for (const auto& part : parts_) {
    part->releaseMemory();
  }
}

void ConcatSource::readRegion(const Box& box, std::span<std::uint16_t> out) const {
  checkRegion(box, dims_, out);
  const auto a = static_cast<std::size_t>(axis_);
  std::vector<std::uint16_t> buffer;
  for (std::size_t p = 0; p < parts_.size(); ++p) {
    const std::int64_t lo = std::max(box.min[a], starts_[p]);
    const std::int64_t hi = std::min(box.max[a], starts_[p + 1]);
    if (lo >= hi) {
      continue;
    }
    Box local = box;
    local.min[a] = lo - starts_[p];
    local.max[a] = hi - starts_[p];
    buffer.resize(static_cast<std::size_t>(local.voxelCount()));
    parts_[p]->readRegion(local, buffer);
    // Scatter rows of the part into the output box.
    std::array<std::int64_t, 3> offset{0, 0, 0};  // of the part region within the box
    offset[a] = lo - box.min[a];
    const std::int64_t row = local.size(0);
    std::size_t source = 0;
    for (std::int64_t z = 0; z < local.size(2); ++z) {
      for (std::int64_t y = 0; y < local.size(1); ++y) {
        const auto target = static_cast<std::size_t>(
            offset[0] + box.size(0) * ((y + offset[1]) + box.size(1) * (z + offset[2])));
        std::copy_n(buffer.begin() + static_cast<std::ptrdiff_t>(source), row,
                    out.begin() + static_cast<std::ptrdiff_t>(target));
        source += static_cast<std::size_t>(row);
      }
    }
  }
}

}  // namespace voxelsieve
