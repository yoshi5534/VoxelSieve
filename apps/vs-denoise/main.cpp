// vs-denoise: denoises a sieved dataset with non-local means or a learned model and writes a new
// dataset (docs/adr/0023-denoising.md). With --benchmark it compares the methods on a synthetic
// scan whose noise-free grey values and defects are known.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "voxelsieve/dataset.hpp"
#include "voxelsieve/denoise.hpp"
#include "voxelsieve/io.hpp"
#include "voxelsieve/model.hpp"
#include "voxelsieve/parts.hpp"
#include "voxelsieve/porosity.hpp"
#include "voxelsieve/render.hpp"
#include "voxelsieve/synthetic.hpp"
#include "voxelsieve/telemetry.hpp"

namespace {

using Json = nlohmann::json;

constexpr std::string_view kUsage = R"(Usage: vs-denoise <dataset> --out <dir> [options]
       vs-denoise --benchmark <dir> [--model <file.vsm>] [options]

Denoises a dataset written by vs-sieve and writes a new dataset with the same bricks and active
voxels; the input stays as it is. Non-local means (the default) averages voxels whose
neighbourhoods look alike, so edges, thin walls and small pores keep their contrast. With
--model, a learned denoising model (.vsm, tools/models/train_denoiser.py) runs instead.

With --benchmark, a synthetic casting with small lunkers, a loosened zone, blur and correlated
noise is written to <dir>/scan.raw (train a model on it, then run again with --model), sieved, and
denoised with non-local means and the model. Every result is scored against the noise-free scan
and its defects: grey-value error on edges, material and air, lunkers found, false pores and
zones. Writes benchmark.json, benchmark.md and slices.png to <dir>.

Options:
  --out <dir>             Dataset to write (required without --benchmark)
  --model <file.vsm>      Denoise with a learned model
  --strength <beta>       Non-local means strength (default 1)
  --search <r>            Non-local means search radius in voxels (default 3)
  --patch <r>             Non-local means patch radius in voxels (default 1)
  --limit <r>             Non-local means similarity limit (default 1.3)
  --noise <sigma>         Noise sigma in grey values (default: estimated)
  --tile <n>              Tile edge in voxels (default 64, with a model 128)
  --value-type <type>     native (default) or float
  --reference <raw>       Noise-free volume of the same size (headerless little-endian uint16)
                          to score the result against
  --json <file>           Also write the result as JSON
  --cache <MB>            Brick cache size (default 1024)
  --telemetry <file>      Write time and resource use per phase as JSON
  -h, --help              Show this help

Benchmark options:
  --benchmark <dir>       Run the benchmark in this directory
  --bench-noise <sigma>   Noise of the synthetic scan (default 1000, contrast 10000)
  --bench-voxel <mm>      Voxel size of the synthetic scan (default 0.25)
)";

struct Options {
  std::filesystem::path dataset;
  std::filesystem::path out;
  std::filesystem::path model;
  std::filesystem::path reference;
  std::filesystem::path json;
  std::filesystem::path benchmark;
  double bench_noise = 1000.0;
  double bench_voxel = 0.25;
  std::size_t cache_mb = 1024;
  voxelsieve::DenoiseOptions denoise;
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
    if (!arg.starts_with("-")) {
      if (!options.dataset.empty()) {
        throw std::invalid_argument("Unexpected argument: " + std::string(arg));
      }
      options.dataset = arg;
    } else if (arg == "-h" || arg == "--help") {
      return std::nullopt;
    } else if (arg == "--out") {
      options.out = next();
    } else if (arg == "--model") {
      options.model = next();
    } else if (arg == "--strength") {
      options.denoise.non_local_means.strength = std::stod(next());
    } else if (arg == "--search") {
      options.denoise.non_local_means.search_radius = std::stoi(next());
    } else if (arg == "--limit") {
      options.denoise.non_local_means.similarity_limit = std::stod(next());
    } else if (arg == "--patch") {
      options.denoise.non_local_means.patch_radius = std::stoi(next());
    } else if (arg == "--noise") {
      options.denoise.noise_sigma = std::stod(next());
    } else if (arg == "--tile") {
      options.denoise.tile = std::stoll(next());
    } else if (arg == "--value-type") {
      const std::string type = next();
      if (type != "native") {
        options.denoise.output.value_type = voxelsieve::parseValueType(type);
      }
    } else if (arg == "--reference") {
      options.reference = next();
    } else if (arg == "--json") {
      options.json = next();
    } else if (arg == "--cache") {
      options.cache_mb = std::stoull(next());
    } else if (arg == "--telemetry") {
      options.telemetry = next();
    } else if (arg == "--benchmark") {
      options.benchmark = next();
    } else if (arg == "--bench-noise") {
      options.bench_noise = std::stod(next());
    } else if (arg == "--bench-voxel") {
      options.bench_voxel = std::stod(next());
    } else {
      throw std::invalid_argument("Unknown option: " + std::string(arg));
    }
  }
  if (options.benchmark.empty() && (options.dataset.empty() || options.out.empty())) {
    throw std::invalid_argument("A dataset and --out, or --benchmark, are required");
  }
  return options;
}

double secondsSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

Json scoreJson(const voxelsieve::DenoiseScore& score) {
  return {{"rmse", score.rmse},
          {"rmse_edges", score.rmse_edges},
          {"rmse_material", score.rmse_material},
          {"rmse_air", score.rmse_air},
          {"voxels", score.voxels},
          {"edge_voxels", score.edge_voxels},
          {"material_voxels", score.material_voxels},
          {"air_voxels", score.air_voxels}};
}

void printScore(const voxelsieve::DenoiseScore& score) {
  std::cout << std::setprecision(1) << "rmse               " << score.rmse << " (edges "
            << score.rmse_edges << ", material " << score.rmse_material << ", air "
            << score.rmse_air << ")\n";
}

// ---------------------------------------------------------------------------------------------
// Benchmark

/// One denoising result of the benchmark.
struct Variant {
  std::string name;
  std::filesystem::path dir;
  double seconds = 0.0;
};

/// Lunkers of the ground truth found as pores: a pore whose centre lies within half the
/// lunker's enclosing radius (at least 1.5 voxels) of its centre. Pores matching no lunker are
/// false.
Json scoreDefects(const voxelsieve::SyntheticScan& scan, const voxelsieve::PorosityResult& result) {
  const auto origin = scan.originMm();
  const voxelsieve::VoxelSize& voxel = scan.voxelSize();
  std::vector<bool> used(result.pores.size(), false);
  int lunkers = 0;
  int found = 0;
  int small_lunkers = 0;
  int small_found = 0;
  double volume_error = 0.0;
  for (const voxelsieve::Defect& defect : scan.defects()) {
    if (defect.type != voxelsieve::DefectType::kLunker) {
      continue;
    }
    ++lunkers;
    const bool small = defect.radius_mm < 2.0 * voxel[0];
    small_lunkers += small ? 1 : 0;
    const auto truth =
        voxel.toVoxels({defect.center_mm[0] - origin[0], defect.center_mm[1] - origin[1],
                        defect.center_mm[2] - origin[2]});
    const double reach = std::max(1.5, 0.5 * defect.radius_mm / voxel[0]);
    for (std::size_t p = 0; p < result.pores.size(); ++p) {
      const auto& c = result.pores[p].center_voxels;
      if (!used[p] && std::hypot(c[0] - truth[0], c[1] - truth[1], c[2] - truth[2]) <= reach) {
        used[p] = true;
        ++found;
        small_found += small ? 1 : 0;
        volume_error +=
            std::abs(result.pores[p].volume_mm3 - defect.void_volume_mm3) / defect.void_volume_mm3;
        break;
      }
    }
  }
  const auto false_pores = std::count(used.begin(), used.end(), false);
  return {{"lunkers", lunkers},
          {"found", found},
          {"small_lunkers", small_lunkers},
          {"small_found", small_found},
          {"false_pores", false_pores},
          {"mean_volume_error", found > 0 ? volume_error / found : 0.0},
          {"zones", result.zones.size()},
          {"noise_sigma", result.noise_sigma}};
}

/// How much of the contrast of each lunker a result keeps: the grey-value deficit below the
/// material value of the voxels whose centre lies inside the lunker, relative to the same deficit
/// in the noise-free scan. Smoothing fills small lunkers in and lowers the ratio; noise leaves
/// it near 1 on average but hides the lunkers. The contrast-to-noise ratio of the small lunkers,
/// their mean deficit over the residual noise of the material, says how well they stand out.
Json scoreLunkerContrast(const voxelsieve::SyntheticScan& scan,
                         const voxelsieve::VolumeSource& clean, const voxelsieve::Dataset& data,
                         double rmse_material) {
  const auto origin = scan.originMm();
  const voxelsieve::VoxelSize& voxel = scan.voxelSize();
  const auto dims = scan.dims();
  const double material = scan.spec().material_value;
  double ratio_sum = 0.0;
  double small_ratio_sum = 0.0;
  double small_deficit_sum = 0.0;
  int count = 0;
  int small = 0;
  for (const voxelsieve::Defect& defect : scan.defects()) {
    if (defect.type != voxelsieve::DefectType::kLunker) {
      continue;
    }
    voxelsieve::Box box;
    for (std::size_t a = 0; a < 3; ++a) {
      const double lo = (defect.center_mm[a] - defect.radius_mm - origin[a]) / voxel[a];
      const double hi = (defect.center_mm[a] + defect.radius_mm - origin[a]) / voxel[a];
      box.min[a] = std::clamp<std::int64_t>(static_cast<std::int64_t>(std::floor(lo)), 0, dims[a]);
      box.max[a] =
          std::clamp<std::int64_t>(static_cast<std::int64_t>(std::ceil(hi)) + 1, 0, dims[a]);
    }
    std::vector<std::uint16_t> truth(static_cast<std::size_t>(box.voxelCount()));
    std::vector<float> values(truth.size());
    clean.readRegion(box, truth);
    data.readRegion(0, box, values, data.info().air_level);
    double truth_sum = 0.0;
    double value_sum = 0.0;
    int inside = 0;
    std::size_t i = 0;
    for (std::int64_t z = box.min[2]; z < box.max[2]; ++z) {
      for (std::int64_t y = box.min[1]; y < box.max[1]; ++y) {
        for (std::int64_t x = box.min[0]; x < box.max[0]; ++x, ++i) {
          const std::array<double, 3> p{origin[0] + static_cast<double>(x) * voxel[0],
                                        origin[1] + static_cast<double>(y) * voxel[1],
                                        origin[2] + static_cast<double>(z) * voxel[2]};
          const bool in_void = std::any_of(
              defect.spheres.begin(), defect.spheres.end(), [&](const voxelsieve::Sphere& s) {
                return std::hypot(p[0] - s.center_mm[0], p[1] - s.center_mm[1],
                                  p[2] - s.center_mm[2]) < s.radius_mm;
              });
          if (in_void) {
            truth_sum += truth[i];
            value_sum += values[i];
            ++inside;
          }
        }
      }
    }
    if (inside == 0) {
      continue;
    }
    const double truth_deficit = material - truth_sum / inside;
    const double deficit = material - value_sum / inside;
    if (truth_deficit <= 0.0) {
      continue;
    }
    ratio_sum += deficit / truth_deficit;
    ++count;
    if (defect.radius_mm < 2.0 * voxel[0]) {
      small_ratio_sum += deficit / truth_deficit;
      small_deficit_sum += deficit;
      ++small;
    }
  }
  return {{"lunkers", count},
          {"contrast_kept", count > 0 ? ratio_sum / count : 0.0},
          {"small_lunkers", small},
          {"small_contrast_kept", small > 0 ? small_ratio_sum / small : 0.0},
          {"small_cnr",
           small > 0 && rmse_material > 0.0 ? small_deficit_sum / small / rmse_material : 0.0}};
}

/// A z slice of each variant next to each other, the noise-free scan first, grey values from air
/// (black) to material (white).
void writeSlices(const std::filesystem::path& file, const voxelsieve::VolumeSource& clean,
                 const std::vector<voxelsieve::Dataset>& datasets, std::int64_t z, float air,
                 float material) {
  const auto dims = clean.dims();
  const auto width = static_cast<int>(dims[0]);
  const auto height = static_cast<int>(dims[1]);
  constexpr int kGap = 6;
  const int panels = static_cast<int>(datasets.size()) + 1;
  voxelsieve::RenderImage image;
  image.width = panels * width + (panels - 1) * kGap;
  image.height = height;
  image.pixels.assign(static_cast<std::size_t>(image.width) * image.height * 3, 255);
  const auto put = [&](int panel, std::int64_t x, std::int64_t y, float value) {
    const float t = std::clamp((value - air) / (material - air), 0.0F, 1.0F);
    const auto grey = static_cast<std::uint8_t>(std::lround(255.0F * t));
    // Image rows run downwards; y up.
    const std::size_t pixel = (static_cast<std::size_t>(height - 1 - y) * image.width +
                               static_cast<std::size_t>(std::int64_t{panel} * (width + kGap) + x)) *
                              3;
    image.pixels[pixel] = image.pixels[pixel + 1] = image.pixels[pixel + 2] = grey;
  };
  const voxelsieve::Box plane{{0, 0, z}, {dims[0], dims[1], z + 1}};
  std::vector<std::uint16_t> truth(static_cast<std::size_t>(plane.voxelCount()));
  clean.readRegion(plane, truth);
  std::vector<float> values(truth.size());
  for (std::int64_t y = 0; y < dims[1]; ++y) {
    for (std::int64_t x = 0; x < dims[0]; ++x) {
      put(0, x, y, truth[static_cast<std::size_t>(x + dims[0] * y)]);
    }
  }
  for (std::size_t d = 0; d < datasets.size(); ++d) {
    datasets[d].readRegion(0, plane, values, air);
    for (std::int64_t y = 0; y < dims[1]; ++y) {
      for (std::int64_t x = 0; x < dims[0]; ++x) {
        put(static_cast<int>(d) + 1, x, y, values[static_cast<std::size_t>(x + dims[0] * y)]);
      }
    }
  }
  voxelsieve::writePng(image, file);
}

int runBenchmark(const Options& options) {
  const std::filesystem::path& dir = options.benchmark;
  std::filesystem::create_directories(dir);
  voxelsieve::SamplePartOptions part;
  part.resolution_mm = options.bench_voxel;
  const voxelsieve::Mesh mesh = voxelsieve::samplePartMesh("housing", part);
  voxelsieve::SyntheticSpec spec;
  spec.voxel_size = voxelsieve::VoxelSize(options.bench_voxel);
  spec.air_value = 2000;
  spec.material_value = 12000;
  spec.seed = 7;
  spec.lunker_count = 16;
  spec.lunker_radius_mm = 3.0 * options.bench_voxel;
  spec.lunker_size_spread = 0.6;  // 1.2 to 3 voxels
  spec.loosening_count = 1;
  spec.loosening_radius_mm = 12.0 * options.bench_voxel;
  spec.loosening_porosity = 0.08;
  spec.blur_sigma_mm = 0.6 * options.bench_voxel;
  spec.noise_sigma = options.bench_noise;
  spec.noise_correlation_mm = 0.7 * options.bench_voxel;
  const voxelsieve::SyntheticScan noisy(mesh, spec);
  voxelsieve::SyntheticSpec clean_spec = spec;
  clean_spec.noise_sigma = 0.0;
  const voxelsieve::SyntheticScan clean(mesh, clean_spec);
  const auto dims = noisy.dims();
  std::cout << "scan               " << dims[0] << " x " << dims[1] << " x " << dims[2]
            << " voxels, " << noisy.defects().size() << " defects, noise " << spec.noise_sigma
            << " (correlated over " << spec.noise_correlation_mm << " mm), contrast "
            << spec.material_value - spec.air_value << '\n';
  if (!std::filesystem::exists(dir / "scan.raw")) {
    voxelsieve::writeRaw(dir / "scan.raw", noisy);
    voxelsieve::writeJson(dir / "scan.json", noisy.toJson());
  }

  std::vector<Variant> variants;
  voxelsieve::DatasetOptions sieve;
  sieve.threshold = 0.5F * static_cast<float>(spec.air_value + spec.material_value);
  sieve.min_material_voxels = 8;
  // The noise-free scan shows what the analyses find at best.
  const auto ideal = dir / "noise-free.vsieve";
  std::filesystem::remove_all(ideal);
  (void)voxelsieve::writeDataset(clean, ideal, sieve);
  variants.push_back({"noise-free", ideal, 0.0});
  const auto input = dir / "noisy.vsieve";
  std::filesystem::remove_all(input);
  (void)voxelsieve::writeDataset(noisy, input, sieve);
  variants.push_back({"noisy", input, 0.0});
  const auto dataset = voxelsieve::Dataset::open(input, options.cache_mb << 20U);

  voxelsieve::DenoiseOptions nlm = options.denoise;
  nlm.method = voxelsieve::DenoiseMethod::kNonLocalMeans;
  nlm.model = nullptr;
  const auto nlm_dir = dir / "nlm.vsieve";
  std::filesystem::remove_all(nlm_dir);
  auto start = std::chrono::steady_clock::now();
  const auto nlm_info = voxelsieve::denoiseDataset(dataset, nlm_dir, nlm);
  variants.push_back({"non-local means", nlm_dir, secondsSince(start)});

  std::optional<voxelsieve::Model> model;
  if (!options.model.empty()) {
    model.emplace(voxelsieve::Model::load(options.model));
    voxelsieve::DenoiseOptions learned = options.denoise;
    learned.method = voxelsieve::DenoiseMethod::kModel;
    learned.model = &*model;
    const auto model_dir = dir / "model.vsieve";
    std::filesystem::remove_all(model_dir);
    start = std::chrono::steady_clock::now();
    (void)voxelsieve::denoiseDataset(dataset, model_dir, learned);
    variants.push_back({"model (" + model->spec().name + ")", model_dir, secondsSince(start)});
  }

  Json results = Json::array();
  std::vector<voxelsieve::Dataset> datasets;
  std::ostringstream table;
  table << std::fixed
        << "| Variant | Seconds | RMSE | Edges | Material | Air | Lunker contrast | Small lunker "
           "contrast | Small lunker CNR | Lunkers found | False pores | Zones |\n"
        << "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |\n";
  for (const Variant& variant : variants) {
    datasets.push_back(voxelsieve::Dataset::open(variant.dir, options.cache_mb << 20U));
    const voxelsieve::Dataset& data = datasets.back();
    const auto score = voxelsieve::scoreDenoising(data, clean);
    const auto porosity = voxelsieve::analyzePorosity(data);
    const Json defects = scoreDefects(noisy, porosity);
    const Json contrast = scoreLunkerContrast(noisy, clean, data, score.rmse_material);
    results.push_back({{"variant", variant.name},
                       {"seconds", variant.seconds},
                       {"score", scoreJson(score)},
                       {"lunker_contrast", contrast},
                       {"defects", defects}});
    table << "| " << variant.name << " | " << std::setprecision(1) << variant.seconds << " | "
          << std::setprecision(0) << score.rmse << " | " << score.rmse_edges << " | "
          << score.rmse_material << " | " << score.rmse_air << " | " << std::setprecision(2)
          << contrast.at("contrast_kept").get<double>() << " | "
          << contrast.at("small_contrast_kept").get<double>() << " | " << std::setprecision(1)
          << contrast.at("small_cnr").get<double>() << " | " << defects.at("found").get<int>()
          << " / " << defects.at("lunkers").get<int>() << " | "
          << defects.at("false_pores").get<std::int64_t>() << " | "
          << defects.at("zones").get<std::size_t>() << " |\n";
  }
  // The slice through the smallest lunker.
  const auto smallest = std::min_element(
      noisy.defects().begin(), noisy.defects().end(), [](const auto& a, const auto& b) {
        return (a.type == voxelsieve::DefectType::kLunker ? a.radius_mm : 1e9) <
               (b.type == voxelsieve::DefectType::kLunker ? b.radius_mm : 1e9);
      });
  std::int64_t z = dims[2] / 2;
  if (smallest != noisy.defects().end()) {
    z = std::clamp<std::int64_t>(
        std::llround((smallest->center_mm[2] - noisy.originMm()[2]) / spec.voxel_size[2]), 0,
        dims[2] - 1);
  }
  datasets.erase(datasets.begin());  // the first panel shows the noise-free scan itself
  writeSlices(dir / "slices.png", clean, datasets, z, spec.air_value, spec.material_value);

  const Json report = {{"scan", noisy.toJson()},
                       {"noise_sigma_estimate", nlm_info.noise.sigma},
                       {"noise_correlation_estimate", nlm_info.noise.correlation(1, 0, 0)},
                       {"non_local_means",
                        {{"strength", nlm.non_local_means.strength},
                         {"similarity_limit", nlm.non_local_means.similarity_limit},
                         {"search_radius", nlm.non_local_means.search_radius},
                         {"patch_radius", nlm.non_local_means.patch_radius}}},
                       {"slice_z", z},
                       {"results", results}};
  voxelsieve::writeJson(dir / "benchmark.json", report);
  std::ofstream(dir / "benchmark.md") << table.str();
  std::cout << "noise estimate     " << std::setprecision(0) << nlm_info.noise.sigma
            << ", neighbour correlation " << std::setprecision(2)
            << nlm_info.noise.correlation(1, 0, 0) << "\n\n"
            << table.str() << "\nwrote              " << (dir / "benchmark.json").string() << ", "
            << (dir / "benchmark.md").string() << ", " << (dir / "slices.png").string()
            << " (z = " << z << ")\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto options = parse(argc, argv);
    if (!options) {
      std::cout << kUsage;
      return 0;
    }
    voxelsieve::Telemetry telemetry("vs-denoise");
    const voxelsieve::TelemetryScope scope(telemetry);
    if (!options->benchmark.empty()) {
      const int status = runBenchmark(*options);
      voxelsieve::reportTelemetry(telemetry, options->telemetry);
      return status;
    }
    const auto start = std::chrono::steady_clock::now();
    const auto dataset = voxelsieve::Dataset::open(options->dataset, options->cache_mb << 20U);
    voxelsieve::DenoiseOptions denoise = options->denoise;
    std::optional<voxelsieve::Model> model;
    if (!options->model.empty()) {
      model.emplace(voxelsieve::Model::load(options->model));
      denoise.method = voxelsieve::DenoiseMethod::kModel;
      denoise.model = &*model;
    }
    const auto info = voxelsieve::denoiseDataset(dataset, options->out, denoise);
    const double seconds = secondsSince(start);
    std::cout << std::fixed << std::setprecision(1) << "method             "
              << voxelsieve::denoiseMethodName(info.method)
              << (info.model.empty() ? "" : " (" + info.model + ")") << '\n'
              << "noise sigma        " << info.noise.sigma << " (correlation with x, y, z "
              << "neighbours " << std::setprecision(2) << info.noise.correlation(1, 0, 0) << ", "
              << info.noise.correlation(0, 1, 0) << ", " << info.noise.correlation(0, 0, 1) << ")\n"
              << std::setprecision(1);
    const auto output = voxelsieve::Dataset::open(options->out, options->cache_mb << 20U);
    const double after = voxelsieve::estimateNoise(output, 1).sigma;
    std::cout << "noise after        " << after << '\n'
              << "bricks             " << info.dataset.levels.front().bricks.size() << ", "
              << info.dataset.active_voxel_count << " active voxels\n"
              << std::setprecision(2) << "time               " << seconds << " s\n";
    Json json = {{"method", std::string(voxelsieve::denoiseMethodName(info.method))},
                 {"noise_sigma", info.noise.sigma},
                 {"noise_correlation",
                  {info.noise.correlation(1, 0, 0), info.noise.correlation(0, 1, 0),
                   info.noise.correlation(0, 0, 1)}},
                 {"noise_sigma_after", after},
                 {"seconds", seconds}};
    if (!info.model.empty()) {
      json["model"] = info.model;
    }
    if (!options->reference.empty()) {
      const voxelsieve::MappedRawSource reference(options->reference, info.dataset.dims,
                                                  info.dataset.voxel_size);
      const auto score = voxelsieve::scoreDenoising(output, reference);
      printScore(score);
      json["score"] = scoreJson(score);
    }
    if (!options->json.empty()) {
      voxelsieve::writeJson(options->json, json);
    }
    voxelsieve::reportTelemetry(telemetry, options->telemetry);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "vs-denoise: " << error.what() << "\n\n" << kUsage;
    return 1;
  }
}
