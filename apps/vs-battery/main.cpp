// vs-battery: inspects a battery cell in a sieved dataset or in section images (housing and
// deformation, electrode layers, anode overhang, foreign particles, gas voids), or writes a
// synthetic cell scan with known geometry to try it on.

#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "voxelsieve/battery.hpp"
#include "voxelsieve/cells.hpp"
#include "voxelsieve/dataset.hpp"
#include "voxelsieve/io.hpp"
#include "voxelsieve/report.hpp"
#include "voxelsieve/telemetry.hpp"

namespace {

constexpr std::string_view kUsage = R"(Usage: vs-battery <dataset> --out <dir> [options]
       vs-battery --images <zip|folder> --out <dir> [--image-cell <name>] [options]
       vs-battery --cell <cylindrical|stacked> --out <prefix> [cell options]
       vs-battery --print-template > my_battery_template.html

Inspects the battery cell in a dataset written by vs-sieve and writes to <dir>:
  battery.json              housing, layers, overhang per edge and sample, particles, voids
  cross_section.png         normal to the cell axis (across the layers of a stack), with the
                            cathode (red) and anode (blue) layers, particles and voids marked
  longitudinal.png          along the cell axis through the middle
  overhang_<edge>.png       the section with the smallest anode overhang at each edge
  report.html, report.json  with --order: the test report, assessed against order.battery

Section images instead of a volume (cylindrical cells): a folder or ZIP archive with a folder
per cell holding axial_images/<angle>.png (longitudinal sections through the axis, the top row
at the high end, the right half at the angle, the left half opposite) and
radial_images/<position mm>.png (cross-sections). Without --image-cell every cell is inspected
into <dir>/<cell>/, and <dir>/cells.csv and <dir>/cells.json summarise them.
  --images <zip|folder>     Source of the section images
  --image-cell <name>       Only this cell, written to <dir>
  --list-cells              Print the cells in the source and exit
  --pixel-size <mm>         Pixel edge length (default: from the positions of the cross-sections
                            over the height of the longitudinal sections)

Options:
  --out <dir>               Output directory (required)
  --order <file>            Inspection order with the requirements; writes the report (see
                            examples/battery_order.json)
  --template <file>         Report template instead of the built-in one
  --print-template          Print the built-in report template and exit
  --type <t>                cylindrical or stacked (pouch, prismatic); default from the shape
  --axis <x|y|z>            Cell axis or stack normal; default from the shape
  --angles <n>              Radial profiles per cross-section of a cylindrical cell (default 360)
  --sections <n>            Cross-sections along the axis, or profiles per in-plane direction of
                            a stack (default 5)
  --overhang-sections <n>   Sections across the electrode edges: angles around a cylindrical
                            cell; a stack gets n / 4 per edge (default 36)
  --average <voxels>        Voxels on each side averaged along the layers (default 2)
  --anode-brighter          The anode layers are the brighter ones (resolved copper foil)
  --particle-threshold <v>  Grey value of foreign particles (default: as far above the brighter
                            electrode as that is above the gaps between the layers)
  --void-threshold <v>      Grey value below which a voxel is gas (default: half way between
                            air and the gaps)
  --min-particle <voxels>   Smallest particle reported (default 2)
  --min-void <voxels>       Smallest void reported (default 8)
  --max-end-deviation <mm>  Layer ends farther than this from the median end of their edge
                            (lost layers, tabs) are left out of the overhang (default 1)
  --cache <MB>              Brick cache size (default 1024)
  --telemetry <file>        Write time and resource use per phase as JSON

Synthetic cell (writes <prefix>.raw and <prefix>.json with the ground truth):
  --cell <type>             cylindrical (wound, in a can) or stacked (sheets in a housing)
  --voxel-size <mm>         Voxel edge length (default 0.025)
  --turns <n>               Turns of a cylindrical cell (default 6)
  --layers <n>              Cathode sheets of a stacked cell (default 5)
  --overhang <mm>           Anode overhang at every edge (default 0.3)
  --ovality <f>             A cylindrical cell f times wider along x (default 0)
  --extension <layer> <edge> <mm> [<from deg> <to deg>]
                            A cathode edge that reaches further out by <mm>: edge 0/1 = low/high
                            end of a cylindrical cell, 0-3 = low x, high x, low y, high y of a
                            stack; the angles limit it around a cylindrical cell
  --particle <x> <y> <z> <r>  A metal particle, mm from the volume centre
  --void <x> <y> <z> <r>    A gas void, mm from the volume centre
  --noise <sigma>           Gaussian noise in grey values (default 800)
  --seed <n>                Noise seed (default 7)
  -h, --help                Show this help
)";

struct Options {
  std::filesystem::path dataset;
  std::filesystem::path out;
  voxelsieve::BatteryOptions battery;
  std::size_t cache_mb = 1024;
  std::filesystem::path telemetry;
  std::filesystem::path order;
  std::filesystem::path report_template;
  bool print_template = false;
  std::optional<voxelsieve::CellSpec> cell;
  std::filesystem::path images;
  std::string image_cell;
  bool list_cells = false;
};

int axisOf(std::string_view name) {
  if (name == "x" || name == "0") {
    return 0;
  }
  if (name == "y" || name == "1") {
    return 1;
  }
  if (name == "z" || name == "2") {
    return 2;
  }
  throw std::invalid_argument("Unknown axis: " + std::string(name));
}

voxelsieve::CellType typeOf(std::string_view name) {
  if (name == "cylindrical") {
    return voxelsieve::CellType::kCylindrical;
  }
  if (name == "stacked" || name == "pouch" || name == "prismatic") {
    return voxelsieve::CellType::kStacked;
  }
  throw std::invalid_argument("Unknown cell type: " + std::string(name));
}

std::optional<Options> parse(int argc, char** argv) {
  Options options;
  voxelsieve::CellSpec cell = voxelsieve::defaultCylindricalCell();
  cell.noise_sigma = 800.0;
  bool synthetic = false;
  // Cell options given before --cell are applied to the cell type chosen there.
  std::optional<double> overhang;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        throw std::invalid_argument("Missing value for " + std::string(arg));
      }
      return argv[++i];
    };
    const auto has_number = [&]() {
      if (i + 1 >= argc) {
        return false;
      }
      const std::string_view v = argv[i + 1];
      return !v.empty() && (v[0] != '-' || (v.size() > 1 && std::isdigit(v[1]) != 0));
    };
    if (arg == "-h" || arg == "--help") {
      return std::nullopt;
    } else if (arg == "--out") {
      options.out = next();
    } else if (arg == "--telemetry") {
      options.telemetry = next();
    } else if (arg == "--order") {
      options.order = next();
    } else if (arg == "--template") {
      options.report_template = next();
    } else if (arg == "--print-template") {
      options.print_template = true;
      return options;
    } else if (arg == "--type") {
      options.battery.cell_type = typeOf(next());
    } else if (arg == "--axis") {
      options.battery.axis = axisOf(next());
    } else if (arg == "--angles") {
      options.battery.angles = std::stoi(next());
    } else if (arg == "--sections") {
      options.battery.sections = std::stoi(next());
    } else if (arg == "--overhang-sections") {
      options.battery.overhang_sections = std::stoi(next());
    } else if (arg == "--average") {
      options.battery.average_voxels = std::stoi(next());
    } else if (arg == "--anode-brighter") {
      options.battery.cathode_brighter = false;
    } else if (arg == "--particle-threshold") {
      options.battery.particle_threshold = std::stof(next());
    } else if (arg == "--void-threshold") {
      options.battery.void_threshold = std::stof(next());
    } else if (arg == "--min-particle") {
      options.battery.min_particle_voxels = std::stoll(next());
    } else if (arg == "--max-end-deviation") {
      options.battery.max_end_deviation_mm = std::stod(next());
    } else if (arg == "--min-void") {
      options.battery.min_void_voxels = std::stoll(next());
    } else if (arg == "--images") {
      options.images = next();
    } else if (arg == "--image-cell") {
      options.image_cell = next();
    } else if (arg == "--list-cells") {
      options.list_cells = true;
    } else if (arg == "--pixel-size") {
      options.battery.image_pixel_mm = std::stod(next());
    } else if (arg == "--cache") {
      options.cache_mb = std::stoull(next());
    } else if (arg == "--cell") {
      const auto type = typeOf(next());
      const auto keep = cell;
      cell = type == voxelsieve::CellType::kCylindrical ? voxelsieve::defaultCylindricalCell()
                                                        : voxelsieve::defaultStackedCell();
      cell.voxel_size = keep.voxel_size;
      cell.noise_sigma = keep.noise_sigma;
      cell.seed = keep.seed;
      cell.ovality = keep.ovality;
      cell.particles = keep.particles;
      cell.voids = keep.voids;
      cell.extensions = keep.extensions;
      synthetic = true;
    } else if (arg == "--voxel-size") {
      cell.voxel_size = voxelsieve::VoxelSize(std::stod(next()));
    } else if (arg == "--turns") {
      cell.turns = std::stoi(next());
    } else if (arg == "--layers") {
      cell.cathode_layers = std::stoi(next());
    } else if (arg == "--overhang") {
      overhang = std::stod(next());
    } else if (arg == "--ovality") {
      cell.ovality = std::stod(next());
    } else if (arg == "--extension") {
      voxelsieve::CathodeExtension e;
      e.layer = std::stoi(next());
      e.edge = std::stoi(next());
      e.length_mm = std::stod(next());
      if (has_number()) {
        e.from_deg = std::stod(next());
        e.to_deg = std::stod(next());
      }
      cell.extensions.push_back(e);
    } else if (arg == "--particle" || arg == "--void") {
      voxelsieve::CellSphere s;
      for (double& c : s.center_mm) {
        c = std::stod(next());
      }
      s.radius_mm = std::stod(next());
      (arg == "--particle" ? cell.particles : cell.voids).push_back(s);
    } else if (arg == "--noise") {
      cell.noise_sigma = std::stod(next());
    } else if (arg == "--seed") {
      cell.seed = std::stoull(next());
    } else if (!arg.starts_with("-") && options.dataset.empty()) {
      options.dataset = arg;
    } else {
      throw std::invalid_argument("Unknown option: " + std::string(arg));
    }
  }
  if (!options.images.empty() && options.list_cells) {
    return options;
  }
  const int sources =
      (synthetic ? 1 : 0) + (options.dataset.empty() ? 0 : 1) + (options.images.empty() ? 0 : 1);
  if (options.out.empty() || sources != 1) {
    throw std::invalid_argument("--out and one of a dataset, --images or --cell are required");
  }
  if (synthetic) {
    if (overhang) {
      cell.overhang_mm = {*overhang, *overhang, *overhang, *overhang};
    }
    // Room for the cell at another voxel size than the default.
    const auto size = voxelsieve::cellOuterSizeMm(cell);
    for (std::size_t a = 0; a < 3; ++a) {
      cell.dims[a] = static_cast<std::int64_t>(std::ceil(size[a] / cell.voxel_size[a])) + 16;
    }
    options.cell = cell;
  }
  return options;
}

int writeCell(const voxelsieve::CellSpec& spec, const Options& options) {
  const voxelsieve::CellSource source(spec);
  const auto raw = std::filesystem::path(options.out).concat(".raw");
  const auto sidecar = std::filesystem::path(options.out).concat(".json");
  if (!options.out.parent_path().empty()) {
    std::filesystem::create_directories(options.out.parent_path());
  }
  voxelsieve::writeRaw(raw, source);
  const nlohmann::json json = {
      {"format",
       {{"type", "raw"}, {"dtype", "uint16"}, {"endianness", "little"}, {"order", "xyz"}}},
      {"dims", spec.dims},
      {"voxel_size_mm", spec.voxel_size},
      {"ground_truth", voxelsieve::cellGroundTruth(spec)}};
  voxelsieve::writeJson(sidecar, json);
  std::cout << "dims               " << spec.dims[0] << " x " << spec.dims[1] << " x "
            << spec.dims[2] << '\n'
            << "wrote              " << raw.string() << ", " << sidecar.string() << '\n';
  return 0;
}

std::string readFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("Cannot read " + path.string());
  }
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string mm(double value) {
  if (!std::isfinite(value)) {
    return "-";
  }
  std::ostringstream text;
  text << std::fixed << std::setprecision(3) << value << " mm";
  return text.str();
}

void printResult(const voxelsieve::BatteryResult& result, std::chrono::duration<double> time) {
  const bool cylindrical = result.type == voxelsieve::CellType::kCylindrical;
  std::cout << "cell               " << (cylindrical ? "cylindrical" : "stacked") << ", axis "
            << "xyz"[result.axis] << '\n'
            << "outer size         " << mm(result.outer_size_mm[0]) << " x "
            << mm(result.outer_size_mm[1]) << " x " << mm(result.outer_size_mm[2]) << '\n';
  if (cylindrical) {
    std::cout << "diameter           " << mm(result.outer_mean_mm) << ", out of round "
              << mm(result.max_out_of_roundness_mm) << '\n';
  } else {
    std::cout << "thickness          " << mm(result.outer_min_size_mm) << " to "
              << mm(result.outer_max_size_mm) << '\n';
  }
  std::cout << "wall               " << mm(result.min_wall_mm) << " (thinnest)\n"
            << "layers             " << result.cathode_layers << " cathode, " << result.anode_layers
            << " anode, pitch " << mm(result.pitch_mm) << '\n';
  for (const auto& edge : result.edges) {
    std::cout << "overhang " << std::left << std::setw(10) << edge.name << std::right
              << mm(edge.min_mm) << " min, " << mm(edge.mean_mm) << " mean, " << edge.samples
              << " samples\n";
  }
  std::cout << "gaps               " << result.gaps.size() << '\n'
            << "particles          " << result.particles.size() << '\n'
            << "voids              " << result.voids.size() << '\n'
            << "time               " << time.count() << " s\n";
  for (const auto& warning : result.warnings) {
    std::cerr << "warning: " << warning << '\n';
  }
}

void writeReport(const Options& options, const std::filesystem::path& out, bool verbose = true) {
  const nlohmann::json order = nlohmann::json::parse(readFile(options.order));
  std::vector<std::string> missing;
  nlohmann::json data = voxelsieve::batteryReportData(order, out, &missing);
  const std::string report_template = options.report_template.empty()
                                          ? std::string(voxelsieve::defaultBatteryReportTemplate())
                                          : readFile(options.report_template);
  std::ofstream(out / "report.html", std::ios::binary)
      << voxelsieve::renderTemplate(report_template, data);
  data.erase("images");
  for (auto& edge : data.at("battery").at("edges")) {
    edge.erase("image");
  }
  voxelsieve::writeJson(out / "report.json", data);
  if (!verbose) {
    return;
  }
  for (const auto& field : missing) {
    std::cerr << "warning: " << field << '\n';
  }
  if (data.contains("evaluation")) {
    std::cout << "assessment         " << data.at("evaluation").at("verdict").get<std::string>()
              << '\n';
  }
  std::cout << "report             " << (out / "report.html").string() << '\n';
}

std::string csvNumber(double value) {
  if (!std::isfinite(value)) {
    return "";
  }
  std::ostringstream text;
  text << std::fixed << std::setprecision(4) << value;
  return text.str();
}

/// Every cell of the image source into its own folder, with a summary of all.
void inspectAllCells(const Options& options) {
  const std::vector<std::string> cells = voxelsieve::listCellImages(options.images);
  if (cells.empty()) {
    throw std::invalid_argument("No cells with section images in " + options.images.string());
  }
  std::filesystem::create_directories(options.out);
  std::ofstream csv(options.out / "cells.csv", std::ios::binary);
  csv << "cell,status,diameter_mm,length_mm,cathode_layers,pitch_mm";
  for (const char* edge : {"z_min", "z_max"}) {
    csv << ",overhang_" << edge << "_min_mm,overhang_" << edge << "_mean_mm,overhang_" << edge
        << "_samples";
  }
  csv << ",particles,largest_particle_mm,voids,largest_void_mm,gaps,warnings\n";
  nlohmann::json summary = nlohmann::json::array();
  int failed = 0;
  for (std::size_t n = 0; n < cells.size(); ++n) {
    const std::string& cell = cells[n];
    std::cout << "[" << n + 1 << "/" << cells.size() << "] " << cell << ": " << std::flush;
    const auto start = std::chrono::steady_clock::now();
    try {
      const voxelsieve::BatteryResult result = voxelsieve::analyzeBatteryImages(
          voxelsieve::readCellImages(options.images, cell), options.battery);
      const std::filesystem::path out = options.out / cell;
      voxelsieve::writeBatteryResult(result, out);
      if (!options.order.empty()) {
        writeReport(options, out, false);
      }
      const auto largest = [](const std::vector<voxelsieve::CellFinding>& list) {
        return list.empty() ? 0.0 : list.front().size_mm;
      };
      csv << cell << ",ok," << csvNumber(result.outer_mean_mm) << "," << csvNumber(result.length_mm)
          << "," << result.cathode_layers << "," << csvNumber(result.pitch_mm);
      nlohmann::json edges = nlohmann::json::object();
      for (const auto& edge : result.edges) {
        csv << "," << csvNumber(edge.samples > 0 ? edge.min_mm : NAN) << ","
            << csvNumber(edge.samples > 0 ? edge.mean_mm : NAN) << "," << edge.samples;
        edges[edge.name] = {{"min_mm", edge.samples > 0 ? nlohmann::json(edge.min_mm) : nullptr},
                            {"mean_mm", edge.samples > 0 ? nlohmann::json(edge.mean_mm) : nullptr},
                            {"samples", edge.samples}};
      }
      csv << "," << result.particles.size() << "," << csvNumber(largest(result.particles)) << ","
          << result.voids.size() << "," << csvNumber(largest(result.voids)) << ","
          << result.gaps.size() << "," << result.warnings.size() << "\n";
      summary.push_back({{"cell", cell},
                         {"status", "ok"},
                         {"diameter_mm", result.outer_mean_mm},
                         {"length_mm", result.length_mm},
                         {"cathode_layers", result.cathode_layers},
                         {"pitch_mm", result.pitch_mm},
                         {"overhang", edges},
                         {"particles", result.particles.size()},
                         {"voids", result.voids.size()},
                         {"gaps", result.gaps.size()},
                         {"warnings", result.warnings}});
      std::cout << "overhang";
      for (const auto& edge : result.edges) {
        std::cout << " " << edge.name << " " << (edge.samples > 0 ? mm(edge.min_mm) : "-");
      }
      std::cout << ", " << result.particles.size() << " particles, " << result.voids.size()
                << " voids ("
                << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()
                << " s)\n";
    } catch (const std::exception& error) {
      ++failed;
      std::cout << "failed: " << error.what() << '\n';
      csv << cell << ",failed" << std::string(16, ',') << "\n";
      summary.push_back({{"cell", cell}, {"status", "failed"}, {"error", error.what()}});
    }
  }
  voxelsieve::writeJson(options.out / "cells.json", summary);
  std::cout << "cells              " << cells.size() << " (" << failed << " failed)\n"
            << "summary            " << (options.out / "cells.csv").string() << '\n';
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto options = parse(argc, argv);
    if (!options) {
      std::cout << kUsage;
      return 0;
    }
    if (options->print_template) {
      std::cout << voxelsieve::defaultBatteryReportTemplate();
      return 0;
    }
    if (const auto& cell = options->cell) {
      return writeCell(*cell, *options);
    }
    if (!options->images.empty() && options->list_cells) {
      for (const std::string& name : voxelsieve::listCellImages(options->images)) {
        std::cout << name << '\n';
      }
      return 0;
    }
    voxelsieve::Telemetry telemetry("vs-battery");
    const voxelsieve::TelemetryScope scope(telemetry);
    if (!options->images.empty() && options->image_cell.empty()) {
      inspectAllCells(*options);
    } else {
      const auto start = std::chrono::steady_clock::now();
      voxelsieve::BatteryResult result;
      if (options->images.empty()) {
        const auto dataset = voxelsieve::Dataset::open(options->dataset, options->cache_mb << 20U);
        result = voxelsieve::analyzeBattery(dataset, options->battery);
      } else {
        result = voxelsieve::analyzeBatteryImages(
            voxelsieve::readCellImages(options->images, options->image_cell), options->battery);
      }
      voxelsieve::writeBatteryResult(result, options->out);
      printResult(result, std::chrono::steady_clock::now() - start);
      if (!options->order.empty()) {
        writeReport(*options, options->out);
      }
    }
    voxelsieve::reportTelemetry(telemetry, options->telemetry);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "vs-battery: " << error.what() << "\n\n" << kUsage;
    return 1;
  }
}
