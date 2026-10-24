// vs-thickness: wall thickness at every point of the surface of a sieved dataset (ADR 0030).

#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#include "voxelsieve/dataset.hpp"
#include "voxelsieve/surface.hpp"
#include "voxelsieve/telemetry.hpp"
#include "voxelsieve/thickness.hpp"

namespace {

constexpr std::string_view kUsage =
    R"(Usage: vs-thickness <dataset> --out <dir> [options]

Measures the wall thickness at every point of the surface of a dataset written by vs-sieve and
writes to <dir>: wall_thickness.json (statistics, regions thinner or thicker than the limits),
thickness.ply (the surface with the thickness and a colour per vertex) and
thickness_view_[12].png. Without --surface, the surface is extracted first (surface.vss).

Methods:
  ray     along the inward surface normal through the grey values to the opposite face of the
          wall; sub-voxel accurate for walls with roughly parallel faces (plates, housings)
  sphere  diameter of the largest sphere inside the material that touches the surface point;
          defined also in corners, at ribs and fillets (castings)

Options:
  --out <dir>             Output directory (required)
  --surface <file.vss>    Surface written by vs-surface
  --method <name>         ray (default) or sphere
  --min-wall <mm>         Smallest permitted wall: thinner regions are listed (default none)
  --max-wall <mm>         Largest permitted wall: thicker regions are listed (default none)
  --max-thickness <mm>    Thicker walls stay unmeasured (default: half the middle extent of
                          the part)
  --search-angle <deg>    Ray method: largest angle between the ray and the face it leaves the
                          wall through (default 30)
  --all-surfaces          Also measure on the surfaces of closed internal voids
  --max-triangles <n>     Triangle budget of the measured surface (default 2000000)
  --cache <MB>            Brick cache size (default 1024)
  --telemetry <file>      Write time and resource use per phase as JSON
  -h, --help              Show this help
)";

struct Options {
  std::filesystem::path dataset;
  std::filesystem::path surface;
  std::filesystem::path out;
  voxelsieve::WallThicknessOptions thickness;
  std::size_t cache_mb = 1024;
  std::filesystem::path telemetry;
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
    } else if (arg == "--surface") {
      options.surface = next();
    } else if (arg == "--method") {
      options.thickness.method = voxelsieve::thicknessMethodFromString(next());
    } else if (arg == "--min-wall") {
      options.thickness.min_wall_mm = std::stod(next());
    } else if (arg == "--max-wall") {
      options.thickness.max_wall_mm = std::stod(next());
    } else if (arg == "--max-thickness") {
      options.thickness.max_thickness_mm = std::stod(next());
    } else if (arg == "--search-angle") {
      options.thickness.search_angle_deg = std::stod(next());
    } else if (arg == "--all-surfaces") {
      options.thickness.outer_surface_only = false;
    } else if (arg == "--max-triangles") {
      options.thickness.max_triangles = std::stoull(next());
    } else if (arg == "--cache") {
      options.cache_mb = std::stoull(next());
    } else if (arg == "--telemetry") {
      options.telemetry = next();
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

void printRegions(const char* label, const std::vector<voxelsieve::ThicknessRegion>& regions) {
  for (std::size_t i = 0; i < std::min<std::size_t>(regions.size(), 5); ++i) {
    const auto& r = regions[i];
    std::cout << label << r.id << ": " << std::setprecision(3) << r.extreme_mm << " mm, "
              << std::setprecision(2) << r.area_mm2 << " mm^2 at " << r.location_mm[0] << ' '
              << r.location_mm[1] << ' ' << r.location_mm[2] << " mm\n";
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto options = parse(argc, argv);
    if (!options) {
      std::cout << kUsage;
      return 0;
    }
    voxelsieve::Telemetry telemetry("vs-thickness");
    const voxelsieve::TelemetryScope scope(telemetry);
    const auto dataset = voxelsieve::Dataset::open(options->dataset, options->cache_mb << 20U);
    std::filesystem::create_directories(options->out);
    std::filesystem::path surface = options->surface;
    if (surface.empty()) {
      surface = options->out / "surface.vss";
      (void)voxelsieve::writeSurface(dataset, surface);
    }
    const auto result = voxelsieve::analyzeWallThickness(
        dataset, voxelsieve::SurfaceMask::open(surface), options->thickness);
    voxelsieve::writeWallThickness(result, options->out);
    const auto& s = result.stats;
    std::cout << std::fixed << "method             " << voxelsieve::toString(result.method)
              << ", walls up to " << std::setprecision(3) << result.max_thickness_mm << " mm\n"
              << "measured           " << std::setprecision(1)
              << (s.area_mm2 > 0.0 ? 100.0 * s.measured_area_mm2 / s.area_mm2 : 0.0) << " % of "
              << s.area_mm2 << " mm^2\n"
              << std::setprecision(3) << "thickness          min " << s.min_mm << " mm, median "
              << s.percentiles_mm[2] << " mm, mean " << s.mean_mm << " mm, max " << s.max_mm
              << " mm\n";
    if (result.min_wall_mm > 0.0) {
      std::cout << "thinner than " << result.min_wall_mm << " mm: " << std::setprecision(2)
                << 100.0 * s.below_min << " % of the surface, " << result.thin_regions.size()
                << " regions\n";
      printRegions("  thin region ", result.thin_regions);
    }
    if (result.max_wall_mm > 0.0) {
      std::cout << std::setprecision(3) << "thicker than " << result.max_wall_mm
                << " mm: " << std::setprecision(2) << 100.0 * s.above_max << " % of the surface, "
                << result.thick_regions.size() << " regions\n";
      printRegions("  thick region ", result.thick_regions);
    }
    voxelsieve::reportTelemetry(telemetry, options->telemetry);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "vs-thickness: " << error.what() << "\n\n" << kUsage;
    return 1;
  }
}
