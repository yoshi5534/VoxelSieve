#include "voxelsieve/io.hpp"

#include <bit>
#include <fstream>
#include <stdexcept>
#include <string>

namespace voxelsieve {

static_assert(std::endian::native == std::endian::little,
              "Raw I/O assumes a little-endian host; add byte swapping for big-endian targets.");

void writeRaw(const std::filesystem::path& path, const Volume16& volume) {
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    throw std::runtime_error("Cannot open for writing: " + path.string());
  }
  out.write(reinterpret_cast<const char*>(volume.data.data()),
            static_cast<std::streamsize>(volume.data.size() * sizeof(std::uint16_t)));
  if (!out) {
    throw std::runtime_error("Write failed: " + path.string());
  }
}

Volume16 readRaw(const std::filesystem::path& path, const std::array<std::int64_t, 3>& dims,
                 double voxel_size_mm) {
  Volume16 volume(dims, voxel_size_mm);
  const auto expected_bytes = volume.data.size() * sizeof(std::uint16_t);
  if (std::filesystem::file_size(path) != expected_bytes) {
    throw std::runtime_error("File size of " + path.string() + " does not match dimensions (" +
                             std::to_string(expected_bytes) + " bytes expected)");
  }
  std::ifstream in(path, std::ios::binary);
  in.read(reinterpret_cast<char*>(volume.data.data()),
          static_cast<std::streamsize>(expected_bytes));
  if (!in) {
    throw std::runtime_error("Read failed: " + path.string());
  }
  return volume;
}

nlohmann::json phantomToJson(const PhantomSpec& spec) {
  nlohmann::json pores = nlohmann::json::array();
  for (const Pore& pore : spec.pores) {
    pores.push_back({{"center_mm", pore.center_mm}, {"radius_mm", pore.radius_mm}});
  }
  return {
      {"format",
       {{"type", "raw"}, {"dtype", "uint16"}, {"endianness", "little"}, {"order", "xyz"}}},
      {"dims", spec.dims},
      {"voxel_size_mm", spec.voxel_size_mm},
      {"phantom",
       {{"air_value", spec.air_value},
        {"material_value", spec.material_value},
        {"noise_sigma", spec.noise_sigma},
        {"seed", spec.seed},
        {"outer_size_mm", spec.outer_size_mm},
        {"wall_thickness_mm", spec.wall_thickness_mm},
        {"pores", pores},
        {"material_volume_mm3", phantomMaterialVolumeMm3(spec)}}},
  };
}

void writeJson(const std::filesystem::path& path, const nlohmann::json& json) {
  std::ofstream out(path);
  if (!out) {
    throw std::runtime_error("Cannot open for writing: " + path.string());
  }
  out << json.dump(2) << '\n';
}

}  // namespace voxelsieve
