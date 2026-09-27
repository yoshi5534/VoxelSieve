// vs-porosity: finds internal pores and zones of loosened microstructure in a sieved dataset and
// writes the result as JSON, projection images and a VDB file for Blender or Houdini.

#include <chrono>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#include "voxelsieve/dataset.hpp"
#include "voxelsieve/io.hpp"
#include "voxelsieve/porosity.hpp"

namespace {

constexpr std::string_view kUsage = R"(Usage: vs-porosity <dataset> --out <dir> [options]

Analyses a dataset written by vs-sieve and writes to <dir>:
  porosity.json           part volume, porosity, every pore and zone
  projection_[xyz].png    part thickness in grey, pores red, zones yellow
  porosity.vdb            grids "pores" and "zones" to view next to the part

Options:
  --out <dir>             Output directory (required)
  --min-pore <voxels>     Smallest pore reported (default 3)
  --zone-sigma <k>        Zone detection limit in standard deviations (default 5)
  --zone-min <fraction>   Smallest void fraction of a zone block (default 0.01)
  --cache <MB>            Brick cache size (default 1024)
  -h, --help              Show this help
)";

struct Options {
  std::filesystem::path dataset;
  std::filesystem::path out;
  voxelsieve::PorosityOptions porosity;
  std::size_t cache_mb = 1024;
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
    } else if (arg == "--min-pore") {
      options.porosity.min_pore_voxels = std::stoll(next());
    } else if (arg == "--zone-sigma") {
      options.porosity.zone_sigma = std::stod(next());
    } else if (arg == "--zone-min") {
      options.porosity.min_zone_void_fraction = std::stod(next());
    } else if (arg == "--cache") {
      options.cache_mb = std::stoull(next());
    } else if (!arg.starts_with("-") && options.dataset.empty()) {
      options.dataset = arg;
    } else {
      throw std::invalid_argument("Unknown option: " + std::string(arg));
    }
  }
  if (options.dataset.empty() || options.out.empty()) {
    throw std::invalid_argument("A dataset and --out are required");
  }
  return options;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto options = parse(argc, argv);
    if (!options) {
      std::cout << kUsage;
      return 0;
    }
    const auto start = std::chrono::steady_clock::now();
    const auto dataset = voxelsieve::Dataset::open(options->dataset, options->cache_mb << 20U);
    const auto result = voxelsieve::analyzePorosity(dataset, options->porosity);
    const auto analysed = std::chrono::steady_clock::now();

    std::filesystem::create_directories(options->out);
    voxelsieve::writeJson(options->out / "porosity.json", voxelsieve::toJson(result));
    voxelsieve::writePorosityImages(dataset, result, options->out);
    voxelsieve::writePorosityVdb(dataset, result, options->out / "porosity.vdb");
    const auto written = std::chrono::steady_clock::now();

    std::cout << std::fixed << std::setprecision(4) << "part volume        "
              << result.part_volume_mm3 << " mm^3\n"
              << "pores              " << result.pores.size() << ", " << result.poreVolumeMm3()
              << " mm^3\n"
              << "zones              " << result.zones.size() << ", " << result.zoneVoidVolumeMm3()
              << " mm^3 void\n"
              << "porosity           " << 100.0 * result.porosity() << " %\n"
              << std::setprecision(2) << "material level     " << result.material_level
              << " (noise " << result.noise_sigma << ")\n"
              << "time               analyse "
              << std::chrono::duration<double>(analysed - start).count() << " s, write "
              << std::chrono::duration<double>(written - analysed).count() << " s\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "vs-porosity: " << error.what() << "\n\n" << kUsage;
    return 1;
  }
}
