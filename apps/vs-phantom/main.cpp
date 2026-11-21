// vs-phantom: writes a synthetic CT phantom as <prefix>.raw plus a <prefix>.json sidecar that
// holds the format description and the analytic ground truth.

#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#include "voxelsieve/cli.hpp"
#include "voxelsieve/io.hpp"
#include "voxelsieve/phantom.hpp"
#include "voxelsieve/source.hpp"

namespace {

constexpr std::string_view kUsage = R"(Usage: vs-phantom --out <prefix> [options]

Writes <prefix>.raw (uint16, little endian, x fastest) and <prefix>.json.

Options:
  --out <prefix>        Output path without extension (required)
  --dims <n>            Voxels per axis (default 128)
  --voxel-size <mm>     Voxel edge length in mm, or x,y,z (default 0.1)
  --noise <sigma>       Gaussian noise in grey values (default 500, 0 disables)
  --seed <n>            Noise seed (default 42)
  --solid               Solid box instead of a hollow one
)";

struct Options {
  std::filesystem::path out;
  voxelsieve::PhantomSpec spec = voxelsieve::defaultPhantomSpec();
};

Options parse(voxelsieve::cli::Arguments& args) {
  Options options;
  options.spec.noise_sigma = 500.0;
  while (args.next()) {
    if (args.is("--out")) {
      options.out = args.value();
    } else if (args.is("--dims")) {
      const std::int64_t n = args.integer();
      options.spec.dims = {n, n, n};
    } else if (args.is("--voxel-size")) {
      options.spec.voxel_size = voxelsieve::parseVoxelSize(args.value());
    } else if (args.is("--noise")) {
      options.spec.noise_sigma = args.number();
    } else if (args.is("--seed")) {
      options.spec.seed = std::stoull(args.value());
    } else if (args.is("--solid")) {
      options.spec.wall_thickness_mm = 0.0;
    } else {
      args.unknown();
    }
  }
  if (options.out.empty()) {
    throw voxelsieve::cli::UsageError("--out is required");
  }
  return options;
}

int run(const Options& options) {
  const auto raw_path = std::filesystem::path(options.out).concat(".raw");
  const auto json_path = std::filesystem::path(options.out).concat(".json");
  // Streamed in slabs that are computed in parallel, so the volume never has to fit in memory.
  voxelsieve::writeRaw(raw_path, voxelsieve::PhantomSource(options.spec));
  voxelsieve::writeJson(json_path, voxelsieve::phantomToJson(options.spec));
  std::cout << "Wrote " << raw_path.string() << " and " << json_path.string() << '\n';
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  return voxelsieve::cli::runTool("vs-phantom", kUsage, argc, argv, parse, run);
}
