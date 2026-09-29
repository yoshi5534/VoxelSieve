// vs-segment: splits the part in a sieved dataset into materials by grey value or with a learned
// model and optionally scores the result against labelled components (docs/adr/0013, 0014).

#include <chrono>
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
#include <utility>
#include <vector>

#include "voxelsieve/dataset.hpp"
#include "voxelsieve/io.hpp"
#include "voxelsieve/materials.hpp"
#include "voxelsieve/model.hpp"
#include "voxelsieve/source.hpp"
#include "voxelsieve/tiff.hpp"

namespace {

constexpr std::string_view kUsage = R"(Usage: vs-segment <dataset> --out <dir> [options]

Splits the part in a dataset written by vs-sieve into materials by grey value and writes a
material volume (materials.json and one integer brick per dataset brick). A voxel is material
when it and enough of its neighbours are above the air threshold, which removes noise spikes but
keeps walls one voxel thin; the classes are separated by thresholds (multi-level Otsu unless
given). With --model, a learned model (.vsm, tools/models) decides instead.

Options:
  --out <dir>             Material volume to write (required)
  --materials <n>         Number of material classes, 1 to 8 (default 2)
  --threshold <value>     Air threshold (default: the dataset's)
  --thresholds <a,b,..>   Thresholds between the classes (default: multi-level Otsu)
  --min-neighbours <n>    Voxels of the 3^3 neighbourhood above the threshold (default 6)
  --grow <n>              Hysteresis steps into voxels above the lower threshold (default 1)
  --grow-fraction <f>     Lower threshold between air level (0) and threshold (1) (default 0.5)
  --model <file.vsm>      Segment with a learned model; the options above do not apply
  --tile <n>              Edge of the tiles the model runs on (default 128)
  --truth <labels>...     Label volumes (TIFF stacks or ZIP archives) to score against;
                          several are joined along --join like in vs-sieve
  --folder <name>         Folder of the labels in the truth input (default target)
  --join <x|y|z>          Axis along which several truth inputs are joined (default z)
  --region <x0,y0,z0,x1,y1,z1>
                          Score only this box, for example data a model was not trained on
  --json <file>           Also write the materials and scores as JSON
  --cache <MB>            Brick cache size (default 1024)
  -h, --help              Show this help
)";

struct Options {
  std::filesystem::path dataset;
  std::filesystem::path out;
  std::filesystem::path json;
  std::filesystem::path model;
  std::optional<voxelsieve::Box> region;
  std::vector<std::filesystem::path> truth;
  std::string folder = "target";
  int join_axis = 2;
  std::size_t cache_mb = 1024;
  voxelsieve::SegmentationOptions segmentation;
  voxelsieve::ModelSegmentationOptions model_options;
};

std::optional<Options> parse(int argc, char** argv) {
  Options options;
  bool truth_list = false;  // plain arguments after --truth are more label inputs
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        throw std::invalid_argument("Missing value for " + std::string(arg));
      }
      return argv[++i];
    };
    if (!arg.starts_with("-")) {
      if (truth_list) {
        options.truth.emplace_back(arg);
      } else if (options.dataset.empty()) {
        options.dataset = arg;
      } else {
        throw std::invalid_argument("Unexpected argument: " + std::string(arg));
      }
      continue;
    }
    truth_list = false;
    if (arg == "-h" || arg == "--help") {
      return std::nullopt;
    } else if (arg == "--out") {
      options.out = next();
    } else if (arg == "--json") {
      options.json = next();
    } else if (arg == "--materials") {
      options.segmentation.materials = std::stoi(next());
    } else if (arg == "--threshold") {
      options.segmentation.air_threshold = std::stof(next());
    } else if (arg == "--thresholds") {
      std::stringstream list(next());
      std::string item;
      while (std::getline(list, item, ',')) {
        options.segmentation.material_thresholds.push_back(std::stof(item));
      }
    } else if (arg == "--min-neighbours") {
      options.segmentation.min_neighbours = std::stoi(next());
    } else if (arg == "--grow") {
      options.segmentation.grow_steps = std::stoi(next());
    } else if (arg == "--grow-fraction") {
      options.segmentation.grow_fraction = std::stof(next());
    } else if (arg == "--model") {
      options.model = next();
    } else if (arg == "--tile") {
      options.model_options.tile = std::stoll(next());
    } else if (arg == "--region") {
      std::stringstream list(next());
      std::vector<std::int64_t> values;
      std::string item;
      while (std::getline(list, item, ',')) {
        values.push_back(std::stoll(item));
      }
      if (values.size() != 6) {
        throw std::invalid_argument("--region needs x0,y0,z0,x1,y1,z1");
      }
      options.region =
          voxelsieve::Box{{values[0], values[1], values[2]}, {values[3], values[4], values[5]}};
    } else if (arg == "--truth") {
      options.truth.emplace_back(next());
      truth_list = true;
    } else if (arg == "--folder") {
      options.folder = next();
    } else if (arg == "--join") {
      const std::string axis = next();
      if (axis != "x" && axis != "y" && axis != "z") {
        throw std::invalid_argument("--join must be x, y or z");
      }
      options.join_axis = axis[0] - 'x';
    } else if (arg == "--cache") {
      options.cache_mb = std::stoull(next());
    } else {
      throw std::invalid_argument("Unknown option: " + std::string(arg));
    }
  }
  if (options.dataset.empty() || options.out.empty()) {
    throw std::invalid_argument("A dataset and --out are required");
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
    const auto start = std::chrono::steady_clock::now();
    const auto dataset = voxelsieve::Dataset::open(options->dataset, options->cache_mb << 20U);
    voxelsieve::MaterialVolumeInfo info;
    if (options->model.empty()) {
      info = voxelsieve::segmentMaterials(dataset, options->out, options->segmentation);
    } else {
      const auto model = voxelsieve::Model::load(options->model);
      info = voxelsieve::segmentMaterialsWithModel(dataset, model, options->out,
                                                   options->model_options);
    }
    const auto segmented = std::chrono::steady_clock::now();

    std::cout << std::fixed << std::setprecision(1);
    if (info.model.empty()) {
      std::cout << "air threshold      " << info.air_threshold << " (grows above "
                << info.grow_threshold << ", " << info.grow_steps << " steps, "
                << info.min_neighbours << " of 27 neighbours)\n";
    } else {
      std::cout << "model              " << info.model << "\n";
    }
    nlohmann::json json = nlohmann::json::parse(
        std::ifstream(options->out / "materials.json"));  // written by segmentMaterials
    for (const auto& material : info.materials) {
      std::cout << "material " << material.id << "         " << material.name << " from "
                << std::setprecision(1) << material.lower << ", " << material.voxel_count
                << " voxels, " << std::setprecision(1) << material.volume_mm3 << " mm^3\n";
    }
    std::cout << std::setprecision(2) << "time               "
              << std::chrono::duration<double>(segmented - start).count() << " s\n";

    if (!options->truth.empty()) {
      std::vector<std::unique_ptr<voxelsieve::VolumeSource>> parts;
      voxelsieve::TiffStackOptions tiff;
      tiff.folder = options->folder;
      for (const auto& path : options->truth) {
        parts.push_back(std::make_unique<voxelsieve::TiffStackSource>(path, tiff));
      }
      const voxelsieve::ConcatSource labels(std::move(parts), options->join_axis);
      std::vector<std::int64_t> starts;
      for (std::size_t p = 0; p < labels.partCount(); ++p) {
        starts.push_back(labels.partStart(p));
      }
      const auto volume = voxelsieve::MaterialVolume::open(options->out);
      const auto score = voxelsieve::scoreMaterials(volume, labels, dataset, starts,
                                                    options->join_axis, options->region);
      std::cout << "components         " << score.components;
      for (std::size_t m = 1; m < score.components_per_material.size(); ++m) {
        std::cout << (m == 1 ? " (" : ", ") << score.components_per_material[m] << " material "
                  << m;
      }
      std::cout << ")\n" << std::setprecision(3) << "dice material/air  " << score.dice[0] << "\n";
      for (std::size_t m = 1; m < score.dice.size(); ++m) {
        std::cout << "dice material " << m << "    " << score.dice[m] << "\n";
      }
      json["score"] = {{"dice", score.dice},
                       {"confusion", score.confusion},
                       {"components", score.components},
                       {"components_per_material", score.components_per_material}};
      std::cout
          << std::setprecision(2) << "scoring time       "
          << std::chrono::duration<double>(std::chrono::steady_clock::now() - segmented).count()
          << " s\n";
    }
    if (!options->json.empty()) {
      voxelsieve::writeJson(options->json, json);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "vs-segment: " << error.what() << "\n\n" << kUsage;
    return 1;
  }
}
