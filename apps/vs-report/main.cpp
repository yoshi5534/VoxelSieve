// vs-report: analyses a sieved dataset for porosity, evaluates it against the limits of an
// inspection order and writes a test report as a self-contained HTML file (print to PDF).

#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "voxelsieve/cli.hpp"
#include "voxelsieve/dataset.hpp"
#include "voxelsieve/io.hpp"
#include "voxelsieve/porosity.hpp"
#include "voxelsieve/report.hpp"
#include "voxelsieve/surface.hpp"

namespace {

constexpr std::string_view kUsage =
    R"(Usage: vs-report <dataset> --order <order.json> --out <dir> [options]
       vs-report --print-template > my_template.html
       vs-report --print-first-article-template > my_first_article_template.html

Analyses a dataset written by vs-sieve, evaluates it against the acceptance limits of the
inspection order and writes to <dir>:
  report.html             test report, self-contained; print it to PDF from the browser
  report.json             all report data, including the evaluation
  porosity.json, projection_[xyz].png, porosity.vdb   as written by vs-porosity

Options:
  --order <file>          Inspection order: laboratory, customer, part, scan settings and
                          acceptance limits per inspection zone, as BDG P 202 limits or a
                          BDG P 203 porosity key, in regions made of boxes, spheres,
                          cylinders and STL meshes (see examples/inspection_order.json and
                          examples/inspection_order_p203.json)
  --out <dir>             Output directory (required)
  --surface <file.vss>    Surface written by vs-surface: adds 3D views of the part and its
                          pores
  --comparison <dir>      Output of vs-compare: adds the nominal-actual comparison
  --wall-thickness <dir>  Output of vs-thickness: adds the wall thickness, evaluated against
                          the "wall_thickness" limits of the order
  --template <file>       Report template instead of the built-in one
  --print-template        Print the built-in template and exit
  --print-first-article-template
                          Print the built-in first article report template (used by the
                          first_article_report operation) and exit
  --min-pore <voxels>     Smallest pore reported (default 3)
  --zone-sigma <k>        Zone detection limit in standard deviations (default 5)
  --zone-min <fraction>   Smallest void fraction of a zone block (default 0.01)
)";

struct Options {
  std::filesystem::path dataset;
  std::filesystem::path order;
  std::filesystem::path out;
  std::filesystem::path report_template;
  std::filesystem::path surface;
  std::filesystem::path comparison;
  std::filesystem::path wall_thickness;
  bool print_template = false;
  bool print_first_article_template = false;
  voxelsieve::PorosityOptions porosity;
};

Options parse(voxelsieve::cli::Arguments& args) {
  Options options;
  while (args.next()) {
    if (args.is("--order")) {
      options.order = args.value();
    } else if (args.is("--out")) {
      options.out = args.value();
    } else if (args.is("--surface")) {
      options.surface = args.value();
    } else if (args.is("--comparison")) {
      options.comparison = args.value();
    } else if (args.is("--wall-thickness")) {
      options.wall_thickness = args.value();
    } else if (args.is("--template")) {
      options.report_template = args.value();
    } else if (args.is("--print-template")) {
      options.print_template = true;
    } else if (args.is("--print-first-article-template")) {
      options.print_first_article_template = true;
    } else if (args.is("--min-pore")) {
      options.porosity.min_pore_voxels = args.integer();
    } else if (args.is("--zone-sigma")) {
      options.porosity.zone_sigma = args.number();
    } else if (args.is("--zone-min")) {
      options.porosity.min_zone_void_fraction = args.number();
    } else if (args.positional() && options.dataset.empty()) {
      options.dataset = args.arg();
    } else {
      args.unknown();
    }
  }
  if (!options.print_template && !options.print_first_article_template &&
      (options.dataset.empty() || options.order.empty() || options.out.empty())) {
    throw voxelsieve::cli::UsageError("A dataset, --order and --out are required");
  }
  return options;
}

std::string readFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("Cannot read " + path.string());
  }
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

int run(const Options& options) {
  if (options.print_template) {
    std::cout << voxelsieve::defaultReportTemplate();
    return 0;
  }
  if (options.print_first_article_template) {
    std::cout << voxelsieve::defaultFirstArticleTemplate();
    return 0;
  }
  const nlohmann::json order = nlohmann::json::parse(readFile(options.order));
  // Relative mesh paths of inspection regions are taken from the order's directory.
  const auto zones = voxelsieve::inspectionZonesFromJson(order, options.order.parent_path());
  const std::string report_template = options.report_template.empty()
                                          ? std::string(voxelsieve::defaultReportTemplate())
                                          : readFile(options.report_template);

  const auto dataset = voxelsieve::Dataset::open(options.dataset);
  const auto result = voxelsieve::analyzePorosity(dataset, options.porosity);
  const auto evaluation = voxelsieve::evaluate(result, zones, &dataset);

  std::filesystem::create_directories(options.out);
  voxelsieve::writeJson(options.out / "porosity.json", voxelsieve::toJson(result));
  voxelsieve::writePorosityImages(dataset, result, options.out);
  voxelsieve::writePorosityVdb(dataset, result, options.out / "porosity.vdb");

  std::vector<std::string> warnings;
  nlohmann::json data =
      voxelsieve::reportData(order, result, options.porosity, evaluation, options.out, &warnings);
  if (!options.surface.empty()) {
    voxelsieve::addPartImages(data, voxelsieve::SurfaceMask::open(options.surface), result);
  }
  if (!options.comparison.empty()) {
    voxelsieve::addComparison(data, options.comparison);
  }
  if (!options.wall_thickness.empty()) {
    voxelsieve::addWallThickness(data, options.wall_thickness, order);
  }
  std::ofstream(options.out / "report.html", std::ios::binary)
      << voxelsieve::renderTemplate(report_template, data);
  data.erase("images");
  voxelsieve::writeJson(options.out / "report.json", data);

  for (const std::string& warning : warnings) {
    std::cerr << "warning: " << warning << "\n";
  }
  std::cout << "report             " << (options.out / "report.html").string() << "\n";
  if (!data.contains("evaluation")) {
    std::cout << "evaluation         none (no acceptance limits in the order)\n";
  } else {
    // Inspection zones of the porosity, then the blocks of further analyses.
    for (const auto& zone : data["evaluation"]["zones"]) {
      std::cout << "zone               " << zone.value("name", std::string()) << ": "
                << (zone.value("passed", true) ? "passed" : "FAILED") << "\n";
    }
    std::cout << "result             "
              << (data["evaluation"].value("passed", true) ? "passed" : "FAILED") << "\n";
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  return voxelsieve::cli::runTool("vs-report", kUsage, argc, argv, parse, run);
}
