#include "voxelsieve/source.hpp"

#include <algorithm>
#include <bit>
#include <boost/iostreams/device/mapped_file.hpp>
#include <cstring>
#include <stdexcept>
#include <string>

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

}  // namespace

void MemorySource::readRegion(const Box& box, std::span<std::uint16_t> out) const {
  checkRegion(box, volume_.dims, out);
  copyRows(volume_.data.data(), volume_.dims, box, out);
}

struct MappedRawSource::Mapping {
  boost::iostreams::mapped_file_source file;
};

MappedRawSource::MappedRawSource(const std::filesystem::path& path,
                                 const std::array<std::int64_t, 3>& dims, double voxel_size_mm)
    : mapping_(std::make_unique<Mapping>()), dims_(dims), voxel_size_mm_(voxel_size_mm) {
  static_assert(std::endian::native == std::endian::little,
                "Raw sources assume a little-endian host.");
  const auto expected_bytes =
      static_cast<std::uintmax_t>(dims[0] * dims[1] * dims[2]) * sizeof(std::uint16_t);
  if (std::filesystem::file_size(path) != expected_bytes) {
    throw std::runtime_error("File size of " + path.string() + " does not match dimensions (" +
                             std::to_string(expected_bytes) + " bytes expected)");
  }
  mapping_->file.open(path.string());
  if (!mapping_->file.is_open()) {
    throw std::runtime_error("Cannot map " + path.string());
  }
}

MappedRawSource::~MappedRawSource() = default;

void MappedRawSource::readRegion(const Box& box, std::span<std::uint16_t> out) const {
  checkRegion(box, dims_, out);
  // The mapping is page aligned, so the data pointer is suitably aligned for uint16_t.
  const auto* data = reinterpret_cast<const std::uint16_t*>(mapping_->file.data());
  copyRows(data, dims_, box, out);
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

}  // namespace voxelsieve
