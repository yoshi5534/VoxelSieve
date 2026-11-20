// vs-diconde: writes a sieved dataset and its results as DICONDE (ASTM E2339) and shows what a
// DICONDE or DICOM series says about the inspected part (docs/adr/0024).

#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

#include "voxelsieve/cli.hpp"
#include "voxelsieve/dataset.hpp"
#include "voxelsieve/dicom.hpp"
#include "voxelsieve/diconde.hpp"
#include "voxelsieve/io.hpp"
#include "voxelsieve/materials.hpp"
#include "voxelsieve/porosity.hpp"
#include "voxelsieve/report.hpp"

namespace {

constexpr std::string_view kUsage = R"(Usage: vs-diconde <dataset> --out <dir> [options]
       vs-diconde --info <dicom-slices/ | slice.dcm> [--json <file>]

Writes a dataset written by vs-sieve as DICONDE (DICOM for nondestructive evaluation): one CT
image per slice in <dir>/volume with the exact grey values, voxel size and position. Pores and
loosened zones of a porosity result become indications of that series, a material volume a
second, derived series of labels in <dir>/materials. The inspection order fills in the
component, the inspection and the scan settings, and with acceptance limits the evaluation.
With --info, prints the component, inspection and indications of a DICONDE or DICOM series.

Options:
  --out <dir>             Directory to write (required, must be empty)
  --porosity <dir>        Porosity result (vs-porosity --out) to write as indications
  --materials <dir>       Material volume (vs-segment --out) to write as labels
  --order <file>          Inspection order (examples/inspection_order.json)
  --description <text>    Series description of the volume
  --info <path>           Show a series instead of writing one
  --json <file>           With --info: also write what was found as JSON
)";

struct Options {
  std::filesystem::path dataset;
  std::filesystem::path out;
  std::filesystem::path porosity;
  std::filesystem::path materials;
  std::filesystem::path order;
  std::filesystem::path info;
  std::filesystem::path json;
  std::string description;
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
    } else if (args.is("--porosity")) {
      options.porosity = args.value();
    } else if (args.is("--materials")) {
      options.materials = args.value();
    } else if (args.is("--order")) {
      options.order = args.value();
    } else if (args.is("--description")) {
      options.description = args.value();
    } else if (args.is("--info")) {
      options.info = args.value();
    } else if (args.is("--json")) {
      options.json = args.value();
    } else {
      args.unknown();
    }
  }
  if (options.info.empty() && (options.dataset.empty() || options.out.empty())) {
    throw voxelsieve::cli::UsageError("A dataset and --out, or --info, are required");
  }
  return options;
}

void printInfo(const Options& options) {
  const voxelsieve::DicomStackSource source(options.info);
  const voxelsieve::DicondeInfo info = voxelsieve::readDicondeInfo(source);
  const auto dims = source.dims();
  std::cout << "series             " << info.series_uid << " (" << info.modality << ", " << dims[0]
            << " x " << dims[1] << " x " << dims[2] << ", "
            << voxelsieve::describe(source.voxelSize()) << ")\n";
  if (!info.series_description.empty()) {
    std::cout << "description        " << info.series_description << "\n";
  }
  if (info.derived) {
    std::cout << "derived            " << info.image_comments << "\n";
  }
  std::cout << "component          " << info.component.name;
  if (!info.component.id.empty()) {
    std::cout << ", " << info.component.id;
  }
  if (!info.component.material.empty()) {
    std::cout << ", " << info.component.material;
  }
  std::cout << "\ninspection         " << info.inspection.study_id;
  if (!info.inspection.company.empty()) {
    std::cout << " by " << info.inspection.company;
  }
  if (!info.inspection.owner.empty()) {
    std::cout << " for " << info.inspection.owner;
  }
  std::cout << "\n";
  if (!info.inspection.notes.empty()) {
    std::cout << "notes              " << info.inspection.notes << "\n";
  }
  std::cout << "indications        " << info.indications.size() << "\n";
  std::cout << std::fixed << std::setprecision(3);
  for (const auto& indication : info.indications) {
    std::cout << "  " << indication.number << " " << indication.type << " " << indication.label
              << " at " << indication.center_mm[0] << ", " << indication.center_mm[1] << ", "
              << indication.center_mm[2] << " mm\n";
  }
  if (!options.json.empty()) {
    voxelsieve::writeJson(options.json, voxelsieve::toJson(info));
  }
}

void write(const Options& options) {
  voxelsieve::DicondeWriteOptions diconde;
  diconde.series_description = options.description;
  nlohmann::json order = nlohmann::json::object();
  if (!options.order.empty()) {
    std::ifstream in(options.order);
    if (!in) {
      throw std::runtime_error("Cannot read " + options.order.string());
    }
    order = nlohmann::json::parse(in);
  }
  std::optional<voxelsieve::Evaluation> evaluation;
  if (!options.porosity.empty()) {
    const auto result = voxelsieve::loadPorosityResult(options.porosity);
    diconde.indications = voxelsieve::porosityIndications(result);
    if (const auto zones = voxelsieve::inspectionZonesFromJson(order, options.order.parent_path());
        !zones.empty()) {
      evaluation = voxelsieve::evaluate(result, zones);
    }
  }
  voxelsieve::applyInspectionOrder(order, diconde, evaluation ? &*evaluation : nullptr);
  const auto dataset = voxelsieve::Dataset::open(options.dataset);
  const auto volume = voxelsieve::writeDicondeDataset(dataset, options.out / "volume", diconde);
  std::cout << "volume             " << volume.files << " slices, " << diconde.indications.size()
            << " indications, series " << volume.series_uid << "\n";
  if (!options.materials.empty()) {
    voxelsieve::DicondeWriteOptions labels = diconde;
    labels.inspection.study_uid = volume.study_uid;
    labels.frame_of_reference_uid = volume.frame_of_reference_uid;
    labels.series_number = 2;
    labels.series_description = "Materials";
    labels.indications.clear();
    const auto materials = voxelsieve::writeDicondeMaterials(
        voxelsieve::MaterialVolume::open(options.materials), options.out / "materials", labels);
    std::cout << "materials          " << materials.files << " slices, series "
              << materials.series_uid << "\n";
  }
  if (evaluation) {
    std::cout << "evaluation         " << (evaluation->passed() ? "passed" : "failed") << "\n";
  }
}

int run(const Options& options) {
  if (!options.info.empty()) {
    printInfo(options);
  } else {
    write(options);
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  return voxelsieve::cli::runTool("vs-diconde", kUsage, argc, argv, parse, run);
}
