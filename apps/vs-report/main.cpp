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

#include "voxelsieve/dataset.hpp"
#include "voxelsieve/io.hpp"
#include "voxelsieve/porosity.hpp"
#include "voxelsieve/report.hpp"
#include "voxelsieve/surface.hpp"
#include "voxelsieve/telemetry.hpp"

namespace {

constexpr std::string_view kUsage =
    R"(Usage: vs-report <dataset> --order <order.json> --out <dir> [options]
       vs-report --print-template > my_template.html

Analyses a dataset written by vs-sieve, evaluates it against the acceptance limits of the
inspection order (BDG P 202 scheme) and writes to <dir>:
  report.html             test report, self-contained; print it to PDF from the browser
  report.json             all report data, including the evaluation
  porosity.json, projection_[xyz].png, porosity.vdb   as written by vs-porosity

Options:
  --order <file>          Inspection order: laboratory, customer, part, scan settings and
                          acceptance limits (see examples/inspection_order.json)
  --out <dir>             Output directory (required)
  --surface <file.vss>    Surface written by vs-surface: adds 3D views of the part and its
                          pores
  --comparison <dir>      Output of vs-compare: adds the nominal-actual comparison
  --template <file>       Report template instead of the built-in one
  --print-template        Print the built-in template and exit
  --min-pore <voxels>     Smallest pore reported (default 3)
  --zone-sigma <k>        Zone detection limit in standard deviations (default 5)
  --zone-min <fraction>   Smallest void fraction of a zone block (default 0.01)
  --cache <MB>            Brick cache size (default 1024)
  --telemetry <file>      Write time and resource use per phase as JSON
  -h, --help              Show this help
)";

struct Options {
  std::filesystem::path dataset;
  std::filesystem::path order;
  std::filesystem::path out;
  std::filesystem::path report_template;
  std::filesystem::path surface;
  std::filesystem::path comparison;
  bool print_template = false;
  voxelsieve::PorosityOptions porosity;
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
    } else if (arg == "--order") {
      options.order = next();
    } else if (arg == "--out") {
      options.out = next();
    } else if (arg == "--telemetry") {
      options.telemetry = next();
    } else if (arg == "--surface") {
      options.surface = next();
    } else if (arg == "--comparison") {
      options.comparison = next();
    } else if (arg == "--template") {
      options.report_template = next();
    } else if (arg == "--print-template") {
      options.print_template = true;
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
  if (!options.print_template &&
      (options.dataset.empty() || options.order.empty() || options.out.empty())) {
    throw std::invalid_argument("A dataset, --order and --out are required");
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

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto options = parse(argc, argv);
    if (!options) {
      std::cout << kUsage;
      return 0;
    }
    voxelsieve::Telemetry telemetry("vs-report");
    const voxelsieve::TelemetryScope scope(telemetry);
    if (options->print_template) {
      std::cout << voxelsieve::defaultReportTemplate();
      return 0;
    }
    const nlohmann::json order = nlohmann::json::parse(readFile(options->order));
    const auto zones = voxelsieve::inspectionZonesFromJson(order);
    const std::string report_template = options->report_template.empty()
                                            ? std::string(voxelsieve::defaultReportTemplate())
                                            : readFile(options->report_template);

    const auto dataset = voxelsieve::Dataset::open(options->dataset, options->cache_mb << 20U);
    const auto result = voxelsieve::analyzePorosity(dataset, options->porosity);
    const auto evaluation = voxelsieve::evaluate(result, zones);

    std::filesystem::create_directories(options->out);
    voxelsieve::writeJson(options->out / "porosity.json", voxelsieve::toJson(result));
    voxelsieve::writePorosityImages(dataset, result, options->out);
    voxelsieve::writePorosityVdb(dataset, result, options->out / "porosity.vdb");

    std::vector<std::string> warnings;
    nlohmann::json data = voxelsieve::reportData(order, result, options->porosity, evaluation,
                                                 options->out, &warnings);
    if (!options->surface.empty()) {
      voxelsieve::addPartImages(data, voxelsieve::SurfaceMask::open(options->surface), result);
    }
    if (!options->comparison.empty()) {
      voxelsieve::addComparison(data, options->comparison);
    }
    std::ofstream(options->out / "report.html", std::ios::binary)
        << voxelsieve::renderTemplate(report_template, data);
    data.erase("images");
    voxelsieve::writeJson(options->out / "report.json", data);

    for (const std::string& warning : warnings) {
      std::cerr << "warning: " << warning << "\n";
    }
    std::cout << "report             " << (options->out / "report.html").string() << "\n";
    if (zones.empty()) {
      std::cout << "evaluation         none (no acceptance limits in the order)\n";
    } else {
      for (const auto& zone : evaluation.zones) {
        std::cout << "zone               " << zone.name << ": "
                  << (zone.passed() ? "passed" : "FAILED") << "\n";
      }
      std::cout << "result             " << (evaluation.passed() ? "passed" : "FAILED") << "\n";
    }
    voxelsieve::reportTelemetry(telemetry, options->telemetry);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "vs-report: " << error.what() << "\n\n" << kUsage;
    return 1;
  }
}
