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

#include "voxelsieve/cli.hpp"
#include "voxelsieve/io.hpp"
#include "voxelsieve/mesh.hpp"
#include "voxelsieve/parts.hpp"
#include "voxelsieve/synthetic.hpp"

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
  --noise-correlation <mm>    Correlation length of the noise, e.g. 0.7 voxels (default 0: white)
  --cupping <f>               Beam-hardening darkening inside the part, e.g. 0.1 (default 0)
  --cupping-depth <mm>        Depth at which cupping reaches 63 % (default 20 % of smallest extent)
  --blur <mm>                 Unsharpness: sigma of a Gaussian point spread function (default 0)
  --rings <n>                 Number of ring artefacts around the z axis (default 0)
  --ring-strength <sigma>     Ring amplitude in grey values (default 300)
)";

struct Options {
  std::filesystem::path input;
  std::filesystem::path out;
  std::optional<std::array<double, 3>> box;
  std::string part;
  double scale = 1.0;
  std::filesystem::path stl;
  voxelsieve::SyntheticSpec spec;
};

Options parse(voxelsieve::cli::Arguments& args) {
  Options options;
  options.spec.noise_sigma = 500.0;
  options.spec.ring_strength = 300.0;
  while (args.next()) {
    auto& spec = options.spec;
    if (args.is("--out")) {
      options.out = args.value();
    } else if (args.is("--box")) {
      std::array<double, 3> size{};
      for (auto& edge : size) {
        edge = args.number();
      }
      options.box = size;
    } else if (args.is("--part")) {
      options.part = args.value();
    } else if (args.is("--scale")) {
      options.scale = args.number();
    } else if (args.is("--stl")) {
      options.stl = args.value();
    } else if (args.is("--blur")) {
      spec.blur_sigma_mm = args.number();
    } else if (args.is("--voxel-size")) {
      spec.voxel_size = args.number();
    } else if (args.is("--padding")) {
      spec.padding_mm = args.number();
    } else if (args.is("--seed")) {
      spec.seed = std::stoull(args.value());
    } else if (args.is("--lunker")) {
      spec.lunker_count = static_cast<int>(args.integer());
    } else if (args.is("--lunker-radius")) {
      spec.lunker_radius_mm = args.number();
    } else if (args.is("--lunker-spread")) {
      spec.lunker_size_spread = args.number();
    } else if (args.is("--hot-spots")) {
      spec.defects_at_hot_spots = true;
    } else if (args.is("--loosening")) {
      spec.loosening_count = static_cast<int>(args.integer());
    } else if (args.is("--loosening-radius")) {
      spec.loosening_radius_mm = args.number();
    } else if (args.is("--loosening-porosity")) {
      spec.loosening_porosity = args.number();
    } else if (args.is("--loosening-pore")) {
      spec.loosening_pore_radius_mm = args.number();
    } else if (args.is("--air")) {
      spec.air_value = static_cast<std::uint16_t>(std::stoul(args.value()));
    } else if (args.is("--material")) {
      spec.material_value = static_cast<std::uint16_t>(std::stoul(args.value()));
    } else if (args.is("--noise")) {
      spec.noise_sigma = args.number();
    } else if (args.is("--noise-correlation")) {
      spec.noise_correlation_mm = args.number();
    } else if (args.is("--cupping")) {
      spec.cupping = args.number();
    } else if (args.is("--cupping-depth")) {
      spec.cupping_depth_mm = args.number();
    } else if (args.is("--rings")) {
      spec.ring_count = static_cast<int>(args.integer());
    } else if (args.is("--ring-strength")) {
      spec.ring_strength = args.number();
    } else if (args.positional() && options.input.empty()) {
      options.input = args.arg();
    } else {
      args.unknown();
    }
  }
  const int geometries =
      (options.input.empty() ? 0 : 1) + (options.box ? 1 : 0) + (options.part.empty() ? 0 : 1);
  if (geometries != 1 || options.out.empty()) {
    throw voxelsieve::cli::UsageError("--out and one of an STL file, --box or --part are required");
  }
  return options;
}

int run(const Options& options) {
  const auto start = std::chrono::steady_clock::now();
  voxelsieve::Mesh mesh;
  if (options.box) {
    mesh = voxelsieve::boxMesh(*options.box);
  } else if (!options.part.empty()) {
    voxelsieve::SamplePartOptions part;
    part.scale = options.scale;
    // Facets about as large as the voxels; the mesh is the ground truth either way.
    part.resolution_mm = options.spec.voxel_size[0];
    mesh = voxelsieve::samplePartMesh(options.part, part);
  } else {
    mesh = voxelsieve::readStl(options.input);
  }
  if (!options.stl.empty()) {
    voxelsieve::writeStl(options.stl, mesh);
  }
  const voxelsieve::SyntheticScan scan(mesh, options.spec);
  for (const auto& warning : scan.warnings()) {
    std::cerr << "warning: " << warning << '\n';
  }
  const auto raw_path = std::filesystem::path(options.out).concat(".raw");
  const auto json_path = std::filesystem::path(options.out).concat(".json");
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
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  return voxelsieve::cli::runTool("vs-synth", kUsage, argc, argv, parse, run);
}
