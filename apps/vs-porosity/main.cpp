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

#include "voxelsieve/cli.hpp"
#include "voxelsieve/dataset.hpp"
#include "voxelsieve/io.hpp"
#include "voxelsieve/materials.hpp"
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
  --materials <dir>       Material volume of the dataset (vs-segment): each material is compared
                          with its own grey value, so a darker material is no zone
)";

struct Options {
  std::filesystem::path dataset;
  std::filesystem::path out;
  std::filesystem::path materials;
  voxelsieve::PorosityOptions porosity;
};

Options parse(voxelsieve::cli::Arguments& args) {
  Options options;
  while (args.next()) {
    if (args.is("--out")) {
      options.out = args.value();
    } else if (args.is("--min-pore")) {
      options.porosity.min_pore_voxels = args.integer("porosity", "min_pore_voxels");
    } else if (args.is("--zone-sigma")) {
      options.porosity.zone_sigma = args.number("porosity", "zone_sigma");
    } else if (args.is("--zone-min")) {
      options.porosity.min_zone_void_fraction = args.number("porosity", "min_zone_void_fraction");
    } else if (args.is("--materials")) {
      options.materials = args.value();
    } else if (args.positional() && options.dataset.empty()) {
      options.dataset = args.arg();
    } else {
      args.unknown();
    }
  }
  if (options.dataset.empty() || options.out.empty()) {
    throw voxelsieve::cli::UsageError("A dataset and --out are required");
  }
  return options;
}

int run(const Options& options) {
  const auto start = std::chrono::steady_clock::now();
  const auto dataset = voxelsieve::Dataset::open(options.dataset);
  std::optional<voxelsieve::MaterialVolume> materials;
  voxelsieve::PorosityOptions porosity = options.porosity;
  if (!options.materials.empty()) {
    materials = voxelsieve::MaterialVolume::open(options.materials);
    porosity.materials = &*materials;
  }
  const auto result = voxelsieve::analyzePorosity(dataset, porosity);
  const auto analysed = std::chrono::steady_clock::now();

  std::filesystem::create_directories(options.out);
  voxelsieve::writePorosityJson(result, options.out / "porosity.json");
  voxelsieve::writePorosityImages(dataset, result, options.out);
  voxelsieve::writePorosityVdb(dataset, result, options.out / "porosity.vdb");
  const auto written = std::chrono::steady_clock::now();

  std::cout << std::fixed << std::setprecision(4) << "part volume        " << result.part_volume_mm3
            << " mm^3\n"
            << "pores              " << result.pores.size() << ", " << result.poreVolumeMm3()
            << " mm^3\n"
            << "zones              " << result.zones.size() << ", " << result.zoneVoidVolumeMm3()
            << " mm^3 void\n"
            << "porosity           " << 100.0 * result.porosity() << " %\n"
            << std::setprecision(2) << "material level     " << result.material_level << " (noise "
            << result.noise_sigma << ")\n";
  for (const auto& [id, level] : result.material_levels) {
    std::cout << "  material " << id << "       " << level << "\n";
  }
  std::cout << "time               analyse "
            << std::chrono::duration<double>(analysed - start).count() << " s, write "
            << std::chrono::duration<double>(written - analysed).count() << " s\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  return voxelsieve::cli::runTool("vs-porosity", kUsage, argc, argv, parse, run);
}
