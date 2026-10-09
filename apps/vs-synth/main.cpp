// vs-synth: turns a closed STL mesh or a built-in sample part into a synthetic CT scan with
// artificial lunkers, loosened microstructure, noise and CT artefacts. Writes <prefix>.raw plus a
// <prefix>.json sidecar with the ground truth of every defect.

#include <array>
#include <chrono>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#include "voxelsieve/io.hpp"
#include "voxelsieve/mesh.hpp"
#include "voxelsieve/parts.hpp"
#include "voxelsieve/synthetic.hpp"
#include "voxelsieve/telemetry.hpp"

namespace {

constexpr std::string_view kUsage = R"(Usage: vs-synth <part.stl> --out <prefix> [options]
       vs-synth --box <x> <y> <z> --out <prefix> [options]
       vs-synth --part <housing|bracket|hub|bellhousing> --out <prefix> [options]

Writes <prefix>.raw (uint16, little endian, x fastest) and <prefix>.json with the ground truth.
STL coordinates are taken as mm. Sizes left at 0 are chosen from the part size.

Geometry:
  --out <prefix>              Output path without extension (required)
  --box <x> <y> <z>           Use a box with these edge lengths in mm instead of an STL file
  --part <name>               Use a sample casting: housing, bracket, hub (30 to 50 mm long)
                              or bellhousing (75 mm across)
  --scale <f>                 Scale of the sample part (default 1)
  --stl <file>                Also write the mesh of the part as STL
  --voxel-size <mm>           Voxel edge length (default 0.1)
  --padding <mm>              Air around the part (default 1)
  --seed <n>                  Seed for defects, noise and artefacts (default 42)

Defects:
  --lunker <n>                Number of shrinkage cavities (default 0)
  --lunker-radius <mm>        Enclosing radius of a lunker (default 5 % of the smallest extent)
  --lunker-spread <f>         Lunker radii vary down to (1 - f) times the radius (default 0)
  --hot-spots                 Place lunkers and loosening zones in the thickest sections
  --loosening <n>             Number of zones of loosened microstructure (default 0)
  --loosening-radius <mm>     Zone radius (default 10 % of the smallest extent)
  --loosening-porosity <f>    Void fraction in a zone (default 0.05)
  --loosening-pore <mm>       Pore radius in a zone (default 0.6 voxels)

Grey values and artefacts:
  --air <value>               Grey value of air (default 1000)
  --material <value>          Grey value of material (default 20000)
  --noise <sigma>             Gaussian noise in grey values (default 500, 0 disables)
  --cupping <f>               Beam-hardening darkening inside the part, e.g. 0.1 (default 0)
  --cupping-depth <mm>        Depth at which cupping reaches 63 % (default 20 % of smallest extent)
  --blur <mm>                 Unsharpness: sigma of a Gaussian point spread function (default 0)
  --rings <n>                 Number of ring artefacts around the z axis (default 0)
  --ring-strength <sigma>     Ring amplitude in grey values (default 300)
  --telemetry <file>          Write time and resource use per phase as JSON
  -h, --help                  Show this help
)";

struct Options {
  std::filesystem::path input;
  std::filesystem::path out;
  std::optional<std::array<double, 3>> box;
  std::string part;
  double scale = 1.0;
  std::filesystem::path stl;
  voxelsieve::SyntheticSpec spec;
  std::filesystem::path telemetry;
};

std::optional<Options> parse(int argc, char** argv) {
  Options options;
  options.spec.noise_sigma = 500.0;
  options.spec.ring_strength = 300.0;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        throw std::invalid_argument("Missing value for " + std::string(arg));
      }
      return argv[++i];
    };
    auto& spec = options.spec;
    if (arg == "-h" || arg == "--help") {
      return std::nullopt;
    } else if (arg == "--out") {
      options.out = next();
    } else if (arg == "--telemetry") {
      options.telemetry = next();
    } else if (arg == "--box") {
      std::array<double, 3> size{};
      for (auto& edge : size) {
        edge = std::stod(next());
      }
      options.box = size;
    } else if (arg == "--part") {
      options.part = next();
    } else if (arg == "--scale") {
      options.scale = std::stod(next());
    } else if (arg == "--stl") {
      options.stl = next();
    } else if (arg == "--blur") {
      spec.blur_sigma_mm = std::stod(next());
    } else if (arg == "--voxel-size") {
      spec.voxel_size = std::stod(next());
    } else if (arg == "--padding") {
      spec.padding_mm = std::stod(next());
    } else if (arg == "--seed") {
      spec.seed = std::stoull(next());
    } else if (arg == "--lunker") {
      spec.lunker_count = std::stoi(next());
    } else if (arg == "--lunker-radius") {
      spec.lunker_radius_mm = std::stod(next());
    } else if (arg == "--lunker-spread") {
      spec.lunker_size_spread = std::stod(next());
    } else if (arg == "--hot-spots") {
      spec.defects_at_hot_spots = true;
    } else if (arg == "--loosening") {
      spec.loosening_count = std::stoi(next());
    } else if (arg == "--loosening-radius") {
      spec.loosening_radius_mm = std::stod(next());
    } else if (arg == "--loosening-porosity") {
      spec.loosening_porosity = std::stod(next());
    } else if (arg == "--loosening-pore") {
      spec.loosening_pore_radius_mm = std::stod(next());
    } else if (arg == "--air") {
      spec.air_value = static_cast<std::uint16_t>(std::stoul(next()));
    } else if (arg == "--material") {
      spec.material_value = static_cast<std::uint16_t>(std::stoul(next()));
    } else if (arg == "--noise") {
      spec.noise_sigma = std::stod(next());
    } else if (arg == "--cupping") {
      spec.cupping = std::stod(next());
    } else if (arg == "--cupping-depth") {
      spec.cupping_depth_mm = std::stod(next());
    } else if (arg == "--rings") {
      spec.ring_count = std::stoi(next());
    } else if (arg == "--ring-strength") {
      spec.ring_strength = std::stod(next());
    } else if (!arg.starts_with("-") && options.input.empty()) {
      options.input = arg;
    } else {
      throw std::invalid_argument("Unknown option: " + std::string(arg));
    }
  }
  const int geometries =
      (options.input.empty() ? 0 : 1) + (options.box ? 1 : 0) + (options.part.empty() ? 0 : 1);
  if (geometries != 1 || options.out.empty()) {
    throw std::invalid_argument("--out and one of an STL file, --box or --part are required");
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
    voxelsieve::Telemetry telemetry("vs-synth");
    const voxelsieve::TelemetryScope scope(telemetry);
    const auto start = std::chrono::steady_clock::now();
    voxelsieve::Mesh mesh;
    if (options->box) {
      mesh = voxelsieve::boxMesh(*options->box);
    } else if (!options->part.empty()) {
      voxelsieve::SamplePartOptions part;
      part.scale = options->scale;
      // Facets about as large as the voxels; the mesh is the ground truth either way.
      part.resolution_mm = options->spec.voxel_size[0];
      mesh = voxelsieve::samplePartMesh(options->part, part);
    } else {
      mesh = voxelsieve::readStl(options->input);
    }
    if (!options->stl.empty()) {
      voxelsieve::writeStl(options->stl, mesh);
    }
    const voxelsieve::SyntheticScan scan(mesh, options->spec);
    for (const auto& warning : scan.warnings()) {
      std::cerr << "warning: " << warning << '\n';
    }
    const auto raw_path = std::filesystem::path(options->out).concat(".raw");
    const auto json_path = std::filesystem::path(options->out).concat(".json");
    voxelsieve::writeRaw(raw_path, scan);
    const auto json = scan.toJson();
    voxelsieve::writeJson(json_path, json);
    const auto done = std::chrono::steady_clock::now();

    const auto dims = scan.dims();
    const auto& truth = json.at("ground_truth");
    std::cout << std::fixed << std::setprecision(3) << "dims               " << dims[0] << " x "
              << dims[1] << " x " << dims[2] << '\n'
              << "part volume        " << truth.at("mesh_volume_mm3").get<double>() << " mm^3\n"
              << "defects            " << scan.defects().size() << ", void volume "
              << truth.at("void_volume_mm3").get<double>() << " mm^3 (porosity "
              << 100.0 * truth.at("porosity").get<double>() << " %)\n"
              << "time               " << std::chrono::duration<double>(done - start).count()
              << " s\n"
              << "wrote              " << raw_path.string() << ", " << json_path.string() << '\n';
    voxelsieve::reportTelemetry(telemetry, options->telemetry);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "vs-synth: " << error.what() << "\n\n" << kUsage;
    return 1;
  }
}
