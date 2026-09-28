#include "voxelsieve/voxel_size.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace voxelsieve {
namespace {

std::string number(double value) {
  std::ostringstream out;
  out << value;
  return out.str();
}

}  // namespace

double VoxelSize::minMm() const { return std::ranges::min(pitch_mm); }

double VoxelSize::maxMm() const { return std::ranges::max(pitch_mm); }

double VoxelSize::meanMm() const { return std::cbrt(volumeMm3()); }

VoxelSize VoxelSize::scaled(double factor) const {
  VoxelSize result(pitch_mm[0] * factor, pitch_mm[1] * factor, pitch_mm[2] * factor);
  result.slice_thickness_mm = slice_thickness_mm;
  return result;
}

void VoxelSize::validate() const {
  for (const double pitch : pitch_mm) {
    if (!(pitch > 0.0) || !std::isfinite(pitch)) {
      throw std::invalid_argument("Voxel size must be positive: " + describe(*this));
    }
  }
  if (!(slice_thickness_mm >= 0.0) || slice_thickness_mm > pitch_mm[2] * (1.0 + 1e-9)) {
    throw std::invalid_argument("Slice thickness must be between 0 and the pitch along z: " +
                                describe(*this));
  }
}

void writeVoxelSize(nlohmann::json& object, const VoxelSize& size) {
  object["voxel_size_mm"] = size;
  if (size.slice_thickness_mm > 0.0) {
    object["slice_thickness_mm"] = size.slice_thickness_mm;
  }
}

VoxelSize voxelSizeFromJson(const nlohmann::json& value) {
  if (value.is_number()) {
    return {value.get<double>()};
  }
  if (value.is_array() && value.size() == 3) {
    return {value[0].get<double>(), value[1].get<double>(), value[2].get<double>()};
  }
  throw std::invalid_argument("A voxel size is a number or [x, y, z] in mm, not " + value.dump());
}

void to_json(nlohmann::json& json,
             const VoxelSize& size) {  // NOLINT(readability-identifier-naming)
  if (size.isotropic()) {
    json = size.pitch_mm[0];
  } else {
    json = size.pitch_mm;
  }
}

void from_json(const nlohmann::json& json,
               VoxelSize& size) {  // NOLINT(readability-identifier-naming)
  size = voxelSizeFromJson(json);
}

VoxelSize readVoxelSize(const nlohmann::json& object) {
  VoxelSize size = voxelSizeFromJson(object.at("voxel_size_mm"));
  size.slice_thickness_mm = object.value("slice_thickness_mm", 0.0);
  return size;
}

VoxelSize parseVoxelSize(const std::string& text) {
  std::vector<double> values;
  std::stringstream stream(text);
  std::string part;
  while (std::getline(stream, part, ',')) {
    values.push_back(std::stod(part));
  }
  if (values.size() == 1) {
    return {values[0]};
  }
  if (values.size() == 3) {
    return {values[0], values[1], values[2]};
  }
  throw std::invalid_argument("A voxel size is one value or three separated by commas: " + text);
}

std::string describe(const VoxelSize& size) {
  std::string text = size.isotropic()
                         ? number(size.pitch_mm[0])
                         : number(size.pitch_mm[0]) + " x " + number(size.pitch_mm[1]) + " x " +
                               number(size.pitch_mm[2]);
  text += " mm";
  if (size.slice_thickness_mm > 0.0) {
    text += ", slices " + number(size.slice_thickness_mm) + " mm thick";
  }
  return text;
}

}  // namespace voxelsieve
