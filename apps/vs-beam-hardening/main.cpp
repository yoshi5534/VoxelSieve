// vs-beam-hardening: corrects beam hardening (cupping and streaks) of a sieved dataset and writes a
// new dataset (docs/adr/0033-beam-hardening-correction.md). With --measure it only reports the
// material grey value by depth below the surface.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "voxelsieve/beam_hardening.hpp"
#include "voxelsieve/cli.hpp"
#include "voxelsieve/dataset.hpp"
#include "voxelsieve/io.hpp"
#include "voxelsieve/render.hpp"

namespace {

using Json = nlohmann::json;

constexpr std::string_view kUsage = R"(Usage: vs-beam-hardening <dataset> --out <dir> [options]
       vs-beam-hardening <dataset> --measure [options]

Corrects beam hardening of a dataset written by vs-sieve and writes a new dataset with the same
bricks and active voxels; the input stays as it is. Beam hardening darkens the material far from
the surface (cupping) and between dense or thick regions (streaks).

The reprojection method (default) projects every slice across the rotation axis again,
reconstructs the path lengths to the powers 2 to --order and adds the combination that makes the
material most uniform. The depth method scales the grey values by the material level at their
depth below the surface; it corrects cupping of one material only, but needs no rotation axis.
Both estimate the correction on a coarse level of the dataset and interpolate it.

Prints the material grey value by depth below the surface and the cupping, before and after.

Options:
  --out <dir>             Dataset to write (required without --measure)
  --measure               Only measure the cupping of the dataset
  --method <name>         reprojection (default) or depth
  --axis <x|y|z>          Rotation axis of the scan (default z)
  --order <n>             Highest power of the path length, 2 to 4 (default 3)
  --resolution <n>        Voxels across the level the correction is estimated on (default 256)
  --value-type <type>     native (default) or float
  --json <file>           Also write the result as JSON
  --slices <png>          Also write a slice across the rotation axis of the input and the
                          result side by side
)";

struct Options {
  std::filesystem::path dataset;
  std::filesystem::path out;
  bool measure = false;
  std::filesystem::path json;
  std::filesystem::path slices;
  voxelsieve::BeamHardeningOptions correction;
};

Options parse(voxelsieve::cli::Arguments& args) {
  Options options;
  while (args.next()) {
    if (args.positional()) {
      if (!options.dataset.empty()) {
        throw voxelsieve::cli::UsageError("Unexpected argument: " + std::string(args.arg()));
      }
      options.dataset = args.arg();
    } else if (args.is("--out")) {
      options.out = args.value();
    } else if (args.is("--measure")) {
      options.measure = true;
    } else if (args.is("--method")) {
      options.correction.method = voxelsieve::parseBeamHardeningMethod(args.value());
    } else if (args.is("--axis")) {
      const std::string axis = args.value();
      if (axis != "x" && axis != "y" && axis != "z") {
        throw voxelsieve::cli::UsageError("--axis must be x, y or z");
      }
      options.correction.rotation_axis = axis[0] - 'x';
    } else if (args.is("--order")) {
      options.correction.order = static_cast<int>(args.integer("beam_hardening", "order"));
    } else if (args.is("--resolution")) {
      options.correction.resolution = args.integer("beam_hardening", "resolution");
    } else if (args.is("--value-type")) {
      const std::string type = args.value();
      if (type != "native") {
        options.correction.output.value_type = voxelsieve::parseValueType(type);
      }
    } else if (args.is("--json")) {
      options.json = args.value();
    } else if (args.is("--slices")) {
      options.slices = args.value();
    } else {
      args.unknown();
    }
  }
  if (options.dataset.empty() || (options.out.empty() && !options.measure)) {
    throw voxelsieve::cli::UsageError("A dataset and --out, or --measure, are required");
  }
  return options;
}

Json profileJson(const voxelsieve::CuppingProfile& profile) {
  return {{"level", profile.level},
          {"cupping", profile.cupping},
          {"depth_mm", profile.depth_mm},
          {"material_level", profile.level_grey}};
}

/// Prints the profiles side by side, at most about 20 rows.
void printProfiles(const voxelsieve::CuppingProfile& before,
                   const voxelsieve::CuppingProfile* after) {
  std::cout << "depth mm   material" << (after != nullptr ? "   corrected" : "") << '\n';
  const std::size_t rows = before.depth_mm.size();
  const std::size_t step = std::max<std::size_t>(1, (rows + 19) / 20);
  for (std::size_t b = 0; b < rows; b += step) {
    std::cout << std::fixed << std::setprecision(2) << std::setw(8) << before.depth_mm[b]
              << std::setprecision(0) << std::setw(11) << before.level_grey[b];
    if (after != nullptr && b < after->level_grey.size()) {
      std::cout << std::setw(12) << after->level_grey[b];
    }
    std::cout << '\n';
  }
}

/// The middle slice across `axis` of each dataset side by side, grey values from the 1st to the
/// 99th percentile of the first.
void writeSlices(const std::filesystem::path& file,
                 const std::vector<const voxelsieve::Dataset*>& datasets, int axis) {
  const auto& info = datasets.front()->info();
  const auto a = static_cast<std::size_t>(axis);
  const std::size_t u = a == 0 ? 1 : 0;
  const std::size_t v = a == 2 ? 1 : 2;
  voxelsieve::Box plane{{0, 0, 0}, info.dims};
  plane.min[a] = info.dims[a] / 2;
  plane.max[a] = plane.min[a] + 1;
  const std::int64_t width = info.dims[u];
  const std::int64_t height = info.dims[v];
  std::vector<std::vector<float>> values;
  for (const voxelsieve::Dataset* dataset : datasets) {
    values.emplace_back(static_cast<std::size_t>(plane.voxelCount()));
    dataset->readRegion(0, plane, values.back(), -1.0F);
  }
  std::vector<float> kept = values.front();
  std::erase_if(kept, [](float value) { return value < 0.0F; });
  float low = info.air_level;
  float high = 2.0F * info.threshold - info.air_level;
  if (kept.size() > 100) {
    std::ranges::sort(kept);
    low = kept[kept.size() / 100];
    high = kept[kept.size() - 1 - kept.size() / 100];
  }
  high = std::max(high, low + 1.0F);
  constexpr int kGap = 8;
  voxelsieve::RenderImage image;
  const auto panels = static_cast<int>(datasets.size());
  image.width = panels * static_cast<int>(width) + (panels - 1) * kGap;
  image.height = static_cast<int>(height);
  image.pixels.assign(
      static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 3, 255);
  for (int p = 0; p < panels; ++p) {
    for (std::int64_t y = 0; y < height; ++y) {
      for (std::int64_t x = 0; x < width; ++x) {
        const float value =
            values[static_cast<std::size_t>(p)][static_cast<std::size_t>(x + width * y)];
        const float t = std::clamp((value - low) / (high - low), 0.0F, 1.0F);
        const auto grey = static_cast<std::uint8_t>(std::lround(255.0F * t));
        // Image rows run downwards; y up.
        const std::size_t pixel =
            (static_cast<std::size_t>(height - 1 - y) * static_cast<std::size_t>(image.width) +
             static_cast<std::size_t>(std::int64_t{p} * (width + kGap) + x)) *
            3;
        image.pixels[pixel] = image.pixels[pixel + 1] = image.pixels[pixel + 2] = grey;
      }
    }
  }
  voxelsieve::writePng(image, file);
}

int run(const Options& options) {
  const auto dataset = voxelsieve::Dataset::open(options.dataset);
  if (options.measure) {
    const int level = voxelsieve::beamHardeningLevel(dataset, options.correction.resolution);
    const auto profile = voxelsieve::measureCupping(dataset, level);
    printProfiles(profile, nullptr);
    std::cout << std::fixed << std::setprecision(1) << "cupping            "
              << 100.0 * profile.cupping << " % (level " << level << ")\n";
    if (!options.json.empty()) {
      voxelsieve::writeJson(options.json, {{"before", profileJson(profile)}});
    }
    return 0;
  }
  const auto start = std::chrono::steady_clock::now();
  const auto info = voxelsieve::correctBeamHardening(dataset, options.out, options.correction);
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  printProfiles(info.before, &info.after);
  std::cout << std::fixed << std::setprecision(1) << "method             "
            << voxelsieve::beamHardeningMethodName(info.method) << " (level " << info.level << ")\n"
            << "cupping            " << 100.0 * info.before.cupping << " % -> "
            << 100.0 * info.after.cupping << " %\n"
            << std::setprecision(0) << "material spread    " << info.material_spread_before
            << " -> " << info.material_spread_after << " grey values (" << info.fit_voxels
            << " voxels)\n"
            << "bricks             " << info.dataset.levels.front().bricks.size() << ", "
            << info.dataset.active_voxel_count << " active voxels\n"
            << std::setprecision(2) << "time               " << seconds << " s\n";
  if (!options.json.empty()) {
    Json json = {{"method", std::string(voxelsieve::beamHardeningMethodName(info.method))},
                 {"level", info.level},
                 {"before", profileJson(info.before)},
                 {"after", profileJson(info.after)},
                 {"material_spread_before", info.material_spread_before},
                 {"material_spread_after", info.material_spread_after},
                 {"fit_voxels", info.fit_voxels},
                 {"seconds", seconds}};
    if (!info.coefficients.empty()) {
      json["coefficients"] = info.coefficients;
    }
    voxelsieve::writeJson(options.json, json);
  }
  if (!options.slices.empty()) {
    const auto output = voxelsieve::Dataset::open(options.out);
    writeSlices(options.slices, {&dataset, &output}, options.correction.rotation_axis);
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  return voxelsieve::cli::runTool("vs-beam-hardening", kUsage, argc, argv, parse, run);
}
