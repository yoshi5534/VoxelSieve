// vs-phantom: writes a synthetic CT phantom as <prefix>.raw plus a <prefix>.json sidecar that
// holds the format description and the analytic ground truth.

#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#include "voxelsieve/io.hpp"
#include "voxelsieve/phantom.hpp"

namespace {

constexpr std::string_view kUsage = R"(Usage: vs-phantom --out <prefix> [options]

Writes <prefix>.raw (uint16, little endian, x fastest) and <prefix>.json.

Options:
  --out <prefix>        Output path without extension (required)
  --dims <n>            Voxels per axis (default 128)
  --voxel-size <mm>     Voxel edge length in mm (default 0.1)
  --noise <sigma>       Gaussian noise in grey values (default 500, 0 disables)
  --seed <n>            Noise seed (default 42)
  --solid               Solid box instead of a hollow one
  -h, --help            Show this help
)";

struct Options {
  std::filesystem::path out;
  voxelsieve::PhantomSpec spec = voxelsieve::defaultPhantomSpec();
};

std::optional<Options> parse(int argc, char** argv) {
  Options options;
  options.spec.noise_sigma = 500.0;
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
    } else if (arg == "--dims") {
      const std::int64_t n = std::stoll(next());
      options.spec.dims = {n, n, n};
    } else if (arg == "--voxel-size") {
      options.spec.voxel_size_mm = std::stod(next());
    } else if (arg == "--noise") {
      options.spec.noise_sigma = std::stod(next());
    } else if (arg == "--seed") {
      options.spec.seed = std::stoull(next());
    } else if (arg == "--solid") {
      options.spec.wall_thickness_mm = 0.0;
    } else {
      throw std::invalid_argument("Unknown option: " + std::string(arg));
    }
  }
  if (options.out.empty()) {
    throw std::invalid_argument("--out is required");
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
    const auto raw_path = std::filesystem::path(options->out).concat(".raw");
    const auto json_path = std::filesystem::path(options->out).concat(".json");
    voxelsieve::writeRaw(raw_path, voxelsieve::generatePhantom(options->spec));
    voxelsieve::writeJson(json_path, voxelsieve::phantomToJson(options->spec));
    std::cout << "Wrote " << raw_path.string() << " and " << json_path.string() << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "vs-phantom: " << error.what() << "\n\n" << kUsage;
    return 1;
  }
}
