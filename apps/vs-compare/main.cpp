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

#include "voxelsieve/cli.hpp"
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
the deviation and a colour per vertex) and deviation_view_[12].png.

Flexible parts that do not keep their nominal shape in the scanner can be bent onto the CAD model
after the rigid alignment (--non-rigid); the deviation is then measured on the bent surface, and
how far each point was moved is written beside it (displacement_view_[12].png).

Options:
  --out <dir>             Output directory (required)
  --align <mode>          auto: principal axes, then best fit (default); refine: best fit from
                          --initial; none: use --initial as it is
  --initial <m>           CAD to scan transform, 12 or 16 comma-separated values of a row-major
                          3x4 or 4x4 matrix in mm (default identity)
  --tolerance <mm>        Deviations within +-tolerance are in tolerance (default 0.1)
  --all-surfaces          Also compare the surfaces of closed internal voids (pores)
  --aligned-stl           Also write the CAD model in scan coordinates (cad_aligned.stl)
  --non-rigid             Bend the surface onto the CAD model after the rigid alignment
  --non-rigid-spacing <mm>
                          Control point spacing of the deformation; smaller features stay
                          deviations (default: an eighth of the part's largest extent)
  --non-rigid-stiffness <s>
                          Resistance against stretching (default 1): high values let only thin
                          sections bend, low values follow almost any smooth deviation
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

Options parse(voxelsieve::cli::Arguments& args) {
  Options options;
  while (args.next()) {
    if (args.is("--out")) {
      options.out = args.value();
    } else if (args.is("--align")) {
      options.compare.alignment = voxelsieve::alignmentFromString(args.value());
    } else if (args.is("--initial")) {
      options.compare.initial = voxelsieve::RigidTransform::fromMatrix(parseNumbers(args.value()));
    } else if (args.is("--tolerance")) {
      options.compare.tolerance_mm = args.number("compare_cad", "tolerance_mm");
    } else if (args.is("--all-surfaces")) {
      options.compare.outer_surface_only = false;
    } else if (args.is("--non-rigid")) {
      options.compare.deformation.enabled = true;
    } else if (args.is("--non-rigid-spacing")) {
      options.compare.deformation.spacing_mm = args.number("compare_cad", "non_rigid_spacing_mm");
    } else if (args.is("--non-rigid-stiffness")) {
      options.compare.deformation.stiffness = args.number("compare_cad", "non_rigid_stiffness");
    } else if (args.is("--aligned-stl")) {
      options.aligned_stl = true;
    } else if (args.positional() && options.surface.empty()) {
      options.surface = args.arg();
    } else if (args.positional() && options.cad.empty()) {
      options.cad = args.arg();
    } else {
      args.unknown();
    }
  }
  if (options.surface.empty() || options.cad.empty() || options.out.empty()) {
    throw voxelsieve::cli::UsageError("A surface file, a CAD model and --out are required");
  }
  return options;
}

int run(const Options& options) {
  const auto mask = voxelsieve::SurfaceMask::open(options.surface);
  const auto cad = voxelsieve::readStl(options.cad);
  const auto result = voxelsieve::compareToCad(mask, cad, options.compare);
  voxelsieve::writeComparison(result, options.out);
  if (options.aligned_stl) {
    voxelsieve::writeAlignedCad(result, cad, options.out / "cad_aligned.stl");
  }
  auto json = voxelsieve::toJson(result);
  json["cad_path"] = std::filesystem::absolute(options.cad).string();
  voxelsieve::writeJson(options.out / "compare.json", json);
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
  if (const auto& d = result.deformation; d.applied) {
    std::cout << std::setprecision(4) << "non-rigid          moved up to " << d.displacement.max_mm
              << " mm (mean " << d.displacement.mean_mm << " mm), spacing " << d.spacing_mm
              << " mm, fit rms " << d.fit_rms_mm << " mm; rigid alone: rms " << d.rigid.rms_mm
              << " mm, " << std::setprecision(1) << 100.0 * d.rigid.within_tolerance
              << " % within\n";
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  return voxelsieve::cli::runTool("vs-compare", kUsage, argc, argv, parse, run);
}
