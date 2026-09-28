// vs-compare: nominal-actual comparison of a scanned surface (vs-surface) with the CAD model of
// the part as STL.

#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "voxelsieve/compare.hpp"
#include "voxelsieve/io.hpp"
#include "voxelsieve/mesh.hpp"
#include "voxelsieve/surface.hpp"

namespace {

constexpr std::string_view kUsage =
    R"(Usage: vs-compare <surface.vss> <cad.stl> --out <dir> [options]

Aligns the CAD model (STL in mm) to the surface of a scan written by vs-surface and measures the
signed deviation of every surface point from it: positive where the part has more material than
nominal, negative where material is missing. Writes compare.json, deviation.ply (the surface with
the deviation and a colour per vertex) and deviation_[xyz].png.

Options:
  --out <dir>             Output directory (required)
  --align <mode>          auto: principal axes, then best fit (default); refine: best fit from
                          --initial; none: use --initial as it is
  --initial <m>           CAD to scan transform, 12 or 16 comma-separated values of a row-major
                          3x4 or 4x4 matrix in mm (default identity)
  --tolerance <mm>        Deviations within +-tolerance are in tolerance (default 0.1)
  --all-surfaces          Also compare the surfaces of closed internal voids (pores)
  --aligned-stl           Also write the CAD model in scan coordinates (cad_aligned.stl)
  -h, --help              Show this help
)";

struct Options {
  std::filesystem::path surface;
  std::filesystem::path cad;
  std::filesystem::path out;
  bool aligned_stl = false;
  voxelsieve::CompareOptions compare;
};

std::vector<double> parseNumbers(const std::string& text) {
  std::vector<double> values;
  std::stringstream stream(text);
  std::string item;
  while (std::getline(stream, item, ',')) {
    values.push_back(std::stod(item));
  }
  return values;
}

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
    } else if (arg == "--align") {
      options.compare.alignment = voxelsieve::alignmentFromString(next());
    } else if (arg == "--initial") {
      options.compare.initial = voxelsieve::RigidTransform::fromMatrix(parseNumbers(next()));
    } else if (arg == "--tolerance") {
      options.compare.tolerance_mm = std::stod(next());
    } else if (arg == "--all-surfaces") {
      options.compare.outer_surface_only = false;
    } else if (arg == "--aligned-stl") {
      options.aligned_stl = true;
    } else if (!arg.starts_with("-") && options.surface.empty()) {
      options.surface = arg;
    } else if (!arg.starts_with("-") && options.cad.empty()) {
      options.cad = arg;
    } else {
      throw std::invalid_argument("Unknown option: " + std::string(arg));
    }
  }
  if (options.surface.empty() || options.cad.empty() || options.out.empty()) {
    throw std::invalid_argument("A surface file, a CAD model and --out are required");
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
    const auto mask = voxelsieve::SurfaceMask::open(options->surface);
    const auto cad = voxelsieve::readStl(options->cad);
    const auto result = voxelsieve::compareToCad(mask, cad, options->compare);
    voxelsieve::writeComparison(result, options->out);
    if (options->aligned_stl) {
      voxelsieve::writeAlignedCad(result, cad, options->out / "cad_aligned.stl");
    }
    auto json = voxelsieve::toJson(result);
    json["cad_path"] = std::filesystem::absolute(options->cad).string();
    voxelsieve::writeJson(options->out / "compare.json", json);
    const auto& s = result.stats;
    const auto& t = result.cad_to_scan.translation;
    std::cout << std::fixed << std::setprecision(4) << "alignment          "
              << voxelsieve::toString(result.alignment) << ", rotation " << std::setprecision(2)
              << result.cad_to_scan.angleDegrees() << " deg, translation " << std::setprecision(3)
              << t[0] << ' ' << t[1] << ' ' << t[2] << " mm\n"
              << std::setprecision(4) << "fit                rms " << result.fit_rms_mm << " mm, "
              << std::setprecision(1) << 100.0 * result.fit_inliers << " % of the points, "
              << result.fit_iterations << " iterations\n"
              << std::setprecision(4) << "deviation          mean " << s.mean_mm << " mm, rms "
              << s.rms_mm << " mm, min " << s.min_mm << " mm, max " << s.max_mm << " mm\n"
              << std::setprecision(3) << "tolerance          +-" << result.tolerance_mm
              << " mm: " << std::setprecision(1) << 100.0 * s.within_tolerance << " % within, "
              << 100.0 * s.above_tolerance << " % above, " << 100.0 * s.below_tolerance
              << " % below\n"
              << "surface            " << s.vertices << " points, " << s.area_mm2 << " mm^2; "
              << result.dropped_components << " internal surfaces left out ("
              << result.dropped_area_mm2 << " mm^2)\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "vs-compare: " << error.what() << "\n\n" << kUsage;
    return 1;
  }
}
