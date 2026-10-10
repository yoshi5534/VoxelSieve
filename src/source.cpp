#include "voxelsieve/source.hpp"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <bit>
#include <boost/iostreams/device/mapped_file.hpp>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "detail/network.hpp"
#include "detail/pages.hpp"

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

std::size_t sampleBytes(SampleType type) { return type == SampleType::kUInt8 ? 1 : 2; }

/// Converts `count` samples of a raw file to 16-bit grey values.
void convertSamples(const unsigned char* samples, std::size_t count, const RawLayout& layout,
                    std::uint16_t* target) {
  if (layout.sample_type == SampleType::kUInt8) {
    std::copy(samples, samples + count, target);
    return;
  }
  // memcpy handles headers of odd length, where the samples are not 2-byte aligned.
  std::memcpy(target, samples, count * sizeof(std::uint16_t));
  if (layout.byte_order != std::endian::native) {
    std::transform(target, target + count, target, [](std::uint16_t v) {
      return static_cast<std::uint16_t>((v >> 8U) | (v << 8U));
    });
  }
}

void checkDims(const RawLayout& layout) {
  for (const std::int64_t d : layout.dims) {
    if (d <= 0) {
      throw std::invalid_argument("Raw volume dimensions must be positive");
    }
  }
}

}  // namespace

void MemorySource::readRegion(const Box& box, std::span<std::uint16_t> out) const {
  checkRegion(box, volume_.dims, out);
  copyRows(volume_.data.data(), volume_.dims, box, out);
}

struct MappedRawSource::Mapping {
  boost::iostreams::mapped_file_source file;
};

MappedRawSource::MappedRawSource(const std::filesystem::path& path,
                                 const std::array<std::int64_t, 3>& dims,
                                 const VoxelSize& voxel_size)
    : MappedRawSource(path, RawLayout{dims, voxel_size, SampleType::kUInt16, std::endian::little,
                                      std::uint64_t{0}}) {}

MappedRawSource::MappedRawSource(const std::filesystem::path& path, const RawLayout& layout)
    : mapping_(std::make_unique<Mapping>()), layout_(layout) {
  checkDims(layout);
  const std::uintmax_t payload =
      static_cast<std::uintmax_t>(layout.dims[0] * layout.dims[1] * layout.dims[2]) *
      sampleBytes(layout.sample_type);
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
  mapping_->file.open(path.string());
  if (!mapping_->file.is_open()) {
    throw std::runtime_error("Cannot map " + path.string());
  }
  on_network_share_ = detail::onNetworkShare(path);
}

MappedRawSource::~MappedRawSource() = default;

void MappedRawSource::releaseMemory() const {
  detail::releaseMappedPages(mapping_->file.data(), mapping_->file.size());
}

void MappedRawSource::readRegion(const Box& box, std::span<std::uint16_t> out) const {
  const auto& dims = layout_.dims;
  checkRegion(box, dims, out);
  const std::size_t bytes = sampleBytes(layout_.sample_type);
  const auto* data = reinterpret_cast<const unsigned char*>(mapping_->file.data()) + header_bytes_;
  const auto row_length = static_cast<std::size_t>(box.size(0));
  std::size_t offset = 0;
  for (std::int64_t z = box.min[2]; z < box.max[2]; ++z) {
    for (std::int64_t y = box.min[1]; y < box.max[1]; ++y) {
      const auto first = static_cast<std::size_t>(box.min[0] + dims[0] * (y + dims[1] * z));
      convertSamples(data + first * bytes, row_length, layout_, &out[offset]);
      offset += row_length;
    }
  }
}

/// The decompressed bytes of a gzip file, read forward; going back starts again.
struct GzipRawSource::Stream {
  static constexpr std::size_t kChunk = std::size_t{1} << 20U;

  explicit Stream(std::filesystem::path file_path)
      : path(std::move(file_path)), input(kChunk), scratch(kChunk) {
    file.open(path, std::ios::binary);
    if (!file) {
      throw std::runtime_error("Cannot open " + path.string());
    }
    if (inflateInit2(&z, 16 + MAX_WBITS) != Z_OK) {  // 16: gzip header and trailer
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
    file.seekg(0);
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
  std::uint64_t position = 0;  // decompressed bytes read
  std::mutex mutex;
};

GzipRawSource::GzipRawSource(const std::filesystem::path& path, const RawLayout& layout)
    : stream_(std::make_unique<Stream>(path)),
      layout_(layout),
      header_bytes_(layout.header_bytes.value_or(0)) {
  checkDims(layout);
}

GzipRawSource::~GzipRawSource() = default;

void GzipRawSource::readRegion(const Box& box, std::span<std::uint16_t> out) const {
  const auto& dims = layout_.dims;
  checkRegion(box, dims, out);
  const std::size_t bytes = sampleBytes(layout_.sample_type);
  // Whole rows of whole slices lie one after another in the file: one read per slice.
  const bool whole_slices = box.size(0) == dims[0] && box.size(1) == dims[1];
  const auto run = static_cast<std::size_t>(whole_slices ? dims[0] * dims[1] : box.size(0));
  std::vector<unsigned char> samples(run * bytes);
  const std::scoped_lock lock(stream_->mutex);
  std::size_t offset = 0;
  for (std::int64_t z = box.min[2]; z < box.max[2]; ++z) {
    for (std::int64_t y = box.min[1]; y < box.max[1]; y += whole_slices ? dims[1] : 1) {
      const auto first = static_cast<std::uint64_t>(box.min[0] + dims[0] * (y + dims[1] * z));
      stream_->seek(header_bytes_ + first * bytes);
      stream_->read(samples.data(), samples.size());
      convertSamples(samples.data(), run, layout_, &out[offset]);
      offset += run;
    }
  }
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
  if (isGzipFile(path)) {
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
