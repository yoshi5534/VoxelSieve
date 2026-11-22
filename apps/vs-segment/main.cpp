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

#include "voxelsieve/cli.hpp"
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
keeps walls one voxel thin; the classes are separated by thresholds (unless given: the valleys
between the peaks of the histogram when it has one per class, else multi-level Otsu). With
--model, a learned model (.vsm, tools/models) decides instead.

Options:
  --out <dir>             Material volume to write (required)
  --materials <n>         Number of material classes, 1 to 8 (default 2)
  --threshold <value>     Air threshold (default: the dataset's)
  --thresholds <a,b,..>   Thresholds between the classes (default: valleys between the peaks,
                          else multi-level Otsu)
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
  voxelsieve::SegmentationOptions segmentation;
  voxelsieve::ModelSegmentationOptions model_options;
};

Options parse(voxelsieve::cli::Arguments& args) {
  Options options;
  bool truth_list = false;  // plain arguments after --truth are more label inputs
  while (args.next()) {
    if (args.positional()) {
      if (truth_list) {
        options.truth.emplace_back(args.arg());
      } else if (options.dataset.empty()) {
        options.dataset = args.arg();
      } else {
        throw voxelsieve::cli::UsageError("Unexpected argument: " + std::string(args.arg()));
      }
      continue;
    }
    truth_list = false;
    if (args.is("--out")) {
      options.out = args.value();
    } else if (args.is("--json")) {
      options.json = args.value();
    } else if (args.is("--materials")) {
      options.segmentation.materials =
          static_cast<int>(args.integer("segment_materials", "materials"));
    } else if (args.is("--threshold")) {
      options.segmentation.air_threshold = static_cast<float>(args.number());
    } else if (args.is("--thresholds")) {
      std::stringstream list(args.value());
      std::string item;
      while (std::getline(list, item, ',')) {
        options.segmentation.material_thresholds.push_back(std::stof(item));
      }
    } else if (args.is("--min-neighbours")) {
      options.segmentation.min_neighbours =
          static_cast<int>(args.integer("segment_materials", "min_neighbours"));
    } else if (args.is("--grow")) {
      options.segmentation.grow_steps =
          static_cast<int>(args.integer("segment_materials", "grow_steps"));
    } else if (args.is("--grow-fraction")) {
      options.segmentation.grow_fraction =
          static_cast<float>(args.number("segment_materials", "grow_fraction"));
    } else if (args.is("--model")) {
      options.model = args.value();
    } else if (args.is("--tile")) {
      options.model_options.tile = args.integer("segment_model", "tile");
    } else if (args.is("--region")) {
      std::stringstream list(args.value());
      std::vector<std::int64_t> values;
      std::string item;
      while (std::getline(list, item, ',')) {
        values.push_back(std::stoll(item));
      }
      if (values.size() != 6) {
        throw voxelsieve::cli::UsageError("--region needs x0,y0,z0,x1,y1,z1");
      }
      options.region =
          voxelsieve::Box{{values[0], values[1], values[2]}, {values[3], values[4], values[5]}};
    } else if (args.is("--truth")) {
      options.truth.emplace_back(args.value());
      truth_list = true;
    } else if (args.is("--folder")) {
      options.folder = args.value();
    } else if (args.is("--join")) {
      const std::string axis = args.value();
      if (axis != "x" && axis != "y" && axis != "z") {
        throw voxelsieve::cli::UsageError("--join must be x, y or z");
      }
      options.join_axis = axis[0] - 'x';
    } else {
      args.unknown();
    }
  }
  if (options.dataset.empty() || options.out.empty()) {
    throw voxelsieve::cli::UsageError("A dataset and --out are required");
  }
  return options;
}

int run(const Options& options) {
  const auto start = std::chrono::steady_clock::now();
  const auto dataset = voxelsieve::Dataset::open(options.dataset);
  voxelsieve::MaterialVolumeInfo info;
  if (options.model.empty()) {
    info = voxelsieve::segmentMaterials(dataset, options.out, options.segmentation);
  } else {
    const auto model = voxelsieve::Model::load(options.model);
    info =
        voxelsieve::segmentMaterialsWithModel(dataset, model, options.out, options.model_options);
  }
  const auto segmented = std::chrono::steady_clock::now();

  std::cout << std::fixed << std::setprecision(1);
  if (info.model.empty()) {
    std::cout << "air threshold      " << info.air_threshold << " (grows above "
              << info.grow_threshold << ", " << info.grow_steps << " steps, " << info.min_neighbours
              << " of 27 neighbours)\n";
  } else {
    std::cout << "model              " << info.model << "\n";
  }
  nlohmann::json json = nlohmann::json::parse(
      std::ifstream(options.out / "materials.json"));  // written by segmentMaterials
  for (const auto& material : info.materials) {
    std::cout << "material " << material.id << "         " << material.name << " from "
              << std::setprecision(1) << material.lower << ", " << material.voxel_count
              << " voxels, " << std::setprecision(1) << material.volume_mm3 << " mm^3\n";
  }
  std::cout << std::setprecision(2) << "time               "
            << std::chrono::duration<double>(segmented - start).count() << " s\n";

  if (!options.truth.empty()) {
    std::vector<std::unique_ptr<voxelsieve::VolumeSource>> parts;
    voxelsieve::TiffStackOptions tiff;
    tiff.folder = options.folder;
    parts.reserve(options.truth.size());
    for (const auto& path : options.truth) {
      parts.push_back(std::make_unique<voxelsieve::TiffStackSource>(path, tiff));
    }
    const voxelsieve::ConcatSource labels(std::move(parts), options.join_axis);
    std::vector<std::int64_t> starts;
    for (std::size_t p = 0; p < labels.partCount(); ++p) {
      starts.push_back(labels.partStart(p));
    }
    const auto volume = voxelsieve::MaterialVolume::open(options.out);
    const auto score = voxelsieve::scoreMaterials(volume, labels, dataset, starts,
                                                  options.join_axis, options.region);
    std::cout << "components         " << score.components;
    for (std::size_t m = 1; m < score.components_per_material.size(); ++m) {
      std::cout << (m == 1 ? " (" : ", ") << score.components_per_material[m] << " material " << m;
    }
    std::cout << ")\n" << std::setprecision(3) << "dice material/air  " << score.dice[0] << "\n";
    for (std::size_t m = 1; m < score.dice.size(); ++m) {
      std::cout << "dice material " << m << "    " << score.dice[m] << "\n";
    }
    json["score"] = {{"dice", score.dice},
                     {"confusion", score.confusion},
                     {"components", score.components},
                     {"components_per_material", score.components_per_material}};
    std::cout << std::setprecision(2) << "scoring time       "
              << std::chrono::duration<double>(std::chrono::steady_clock::now() - segmented).count()
              << " s\n";
  }
  if (!options.json.empty()) {
    voxelsieve::writeJson(options.json, json);
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  return voxelsieve::cli::runTool("vs-segment", kUsage, argc, argv, parse, run);
}
