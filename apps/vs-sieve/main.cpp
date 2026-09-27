// vs-sieve: converts a raw CT volume into a sparse OpenVDB grid that keeps the part, its internal
// voids and an air margin, and drops the air connected to the volume boundary.

#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

#include "voxelsieve/io.hpp"
#include "voxelsieve/sieve.hpp"
#include "voxelsieve/vdb.hpp"

namespace {

constexpr std::string_view kUsage = R"(Usage: vs-sieve <input.raw> --out <output.vdb> [options]

Reads a headerless uint16 raw volume (little endian, x fastest). Dimensions and voxel size come
from <input>.json (as written by vs-phantom) unless given on the command line.

Options:
  --out <file.vdb>        Output file (required)
  --dims <x> <y> <z>      Volume dimensions in voxels
  --voxel-size <mm>       Voxel edge length in mm
  --threshold <value>     Air/material grey value (default: Otsu estimate)
  --margin <voxels>       Air margin kept around the part (default 3)
  --min-material <n>      Voxels above threshold for a block to count as material (default 1)
  --dense                 Write every voxel without sieving (baseline for comparison)
  -h, --help              Show this help
)";

struct Options {
  std::filesystem::path input;
  std::filesystem::path out;
  std::optional<std::array<std::int64_t, 3>> dims;
  std::optional<double> voxel_size_mm;
  voxelsieve::SieveOptions sieve;
  bool dense = false;
};

std::optional<Options> parse(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        throw std::invalid_argument("Missing value for " + std::string(arg));
      }
      return argv[++i];
    };
    if (arg == "-h" || arg == "--help") {
      return std::nullopt;
    } else if (arg == "--out") {
      options.out = next();
    } else if (arg == "--dims") {
      std::array<std::int64_t, 3> dims{};
      for (auto& d : dims) {
        d = std::stoll(next());
      }
      options.dims = dims;
    } else if (arg == "--voxel-size") {
      options.voxel_size_mm = std::stod(next());
    } else if (arg == "--threshold") {
      options.sieve.threshold = std::stof(next());
    } else if (arg == "--margin") {
      options.sieve.margin_voxels = std::stoi(next());
    } else if (arg == "--min-material") {
      options.sieve.min_material_voxels = std::stoi(next());
    } else if (arg == "--dense") {
      options.dense = true;
    } else if (!arg.starts_with("-") && options.input.empty()) {
      options.input = arg;
    } else {
      throw std::invalid_argument("Unknown option: " + std::string(arg));
    }
  }
  if (options.input.empty() || options.out.empty()) {
    throw std::invalid_argument("An input file and --out are required");
  }
  return options;
}

struct Geometry {
  std::array<std::int64_t, 3> dims{};
  double voxel_size_mm = 0.0;
};

/// Takes dims and voxel size from the command line, falling back to the JSON sidecar.
Geometry resolveGeometry(const Options& options) {
  if (options.dims && options.voxel_size_mm) {
    return {*options.dims, *options.voxel_size_mm};
  }
  auto sidecar = options.input;
  sidecar.replace_extension(".json");
  if (!std::filesystem::exists(sidecar)) {
    throw std::invalid_argument("No --dims/--voxel-size given and no sidecar " + sidecar.string());
  }
  std::ifstream in(sidecar);
  const auto json = nlohmann::json::parse(in);
  return {options.dims ? *options.dims : json.at("dims").get<std::array<std::int64_t, 3>>(),
          options.voxel_size_mm ? *options.voxel_size_mm : json.at("voxel_size_mm").get<double>()};
}

double megabytes(std::uintmax_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); }

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto options = parse(argc, argv);
    if (!options) {
      std::cout << kUsage;
      return 0;
    }
    const Geometry geometry = resolveGeometry(*options);

    const auto start = std::chrono::steady_clock::now();
    const auto volume = voxelsieve::readRaw(options->input, geometry.dims, geometry.voxel_size_mm);
    const auto loaded = std::chrono::steady_clock::now();

    openvdb::FloatGrid::Ptr grid;
    if (options->dense) {
      grid = voxelsieve::toDenseFloatGrid(volume);
    } else {
      const auto result = voxelsieve::sieve(volume, options->sieve);
      grid = result.grid;
      const auto& s = result.stats;
      std::cout << "threshold          " << s.threshold << " (air level " << s.air_level << ")\n"
                << "blocks             " << s.block_count << " total, " << s.material_block_count
                << " material, " << s.outside_air_block_count << " outside air\n";
    }
    const auto converted = std::chrono::steady_clock::now();
    voxelsieve::writeVdb(options->out, {grid});
    const auto written = std::chrono::steady_clock::now();

    const auto seconds = [](auto a, auto b) {
      return std::chrono::duration<double>(b - a).count();
    };
    const auto active = grid->activeVoxelCount();
    const auto raw_bytes = std::filesystem::file_size(options->input);
    const auto vdb_bytes = std::filesystem::file_size(options->out);
    std::cout << std::fixed << std::setprecision(2) << "active voxels      " << active << " of "
              << volume.voxelCount() << " ("
              << 100.0 * static_cast<double>(active) / static_cast<double>(volume.voxelCount())
              << " %)\n"
              << "file size          " << megabytes(raw_bytes) << " MB raw -> "
              << megabytes(vdb_bytes) << " MB vdb ("
              << 100.0 * static_cast<double>(vdb_bytes) / static_cast<double>(raw_bytes) << " %)\n"
              << "time               read " << seconds(start, loaded) << " s, convert "
              << seconds(loaded, converted) << " s, write " << seconds(converted, written)
              << " s\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "vs-sieve: " << error.what() << "\n\n" << kUsage;
    return 1;
  }
}
