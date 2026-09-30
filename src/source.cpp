#include "voxelsieve/source.hpp"

#include <algorithm>
#include <bit>
#include <boost/iostreams/device/mapped_file.hpp>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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
  for (const std::int64_t d : layout.dims) {
    if (d <= 0) {
      throw std::invalid_argument("Raw volume dimensions must be positive");
    }
  }
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
}

MappedRawSource::~MappedRawSource() = default;

void MappedRawSource::readRegion(const Box& box, std::span<std::uint16_t> out) const {
  const auto& dims = layout_.dims;
  checkRegion(box, dims, out);
  const std::size_t bytes = sampleBytes(layout_.sample_type);
  const bool swap = layout_.byte_order != std::endian::native;
  const auto* data = reinterpret_cast<const unsigned char*>(mapping_->file.data()) + header_bytes_;
  const auto row_length = static_cast<std::size_t>(box.size(0));
  std::size_t offset = 0;
  for (std::int64_t z = box.min[2]; z < box.max[2]; ++z) {
    for (std::int64_t y = box.min[1]; y < box.max[1]; ++y) {
      const auto first = static_cast<std::size_t>(box.min[0] + dims[0] * (y + dims[1] * z));
      const unsigned char* row = data + first * bytes;
      std::uint16_t* target = &out[offset];
      if (layout_.sample_type == SampleType::kUInt8) {
        std::copy(row, row + row_length, target);
      } else {
        // memcpy handles headers of odd length, where the samples are not 2-byte aligned.
        std::memcpy(target, row, row_length * sizeof(std::uint16_t));
        if (swap) {
          std::transform(target, target + row_length, target, [](std::uint16_t v) {
            return static_cast<std::uint16_t>((v >> 8U) | (v << 8U));
          });
        }
      }
      offset += row_length;
    }
  }
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

bool ConcatSource::slowRandomAccess() const {
  return std::any_of(parts_.begin(), parts_.end(),
                     [](const auto& part) { return part->slowRandomAccess(); });
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
