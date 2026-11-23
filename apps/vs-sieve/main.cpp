// vs-sieve: converts a raw CT volume into a sparse OpenVDB grid that keeps the part, its internal
// voids and an air margin, and drops the air connected to the volume boundary.

#ifdef _WIN32
#include <io.h>  // _isatty
#else
#include <unistd.h>  // isatty
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "voxelsieve/cli.hpp"
#include "voxelsieve/dataset.hpp"
#include "voxelsieve/dicom.hpp"
#include "voxelsieve/gzipped_xml.hpp"
#include "voxelsieve/hdf5.hpp"
#include "voxelsieve/header_volume.hpp"
#include "voxelsieve/operation.hpp"
#include "voxelsieve/png_stack.hpp"
#include "voxelsieve/sieve.hpp"
#include "voxelsieve/tiff.hpp"
#include "voxelsieve/vdb.hpp"

namespace {

constexpr std::string_view kUsageHead = R"(Usage: vs-sieve <input.raw> --out <output> [options]
       vs-sieve <slices/ | stack.tif | slices.zip> --out <output> [options]
       vs-sieve <dicom-slices/ | project.xml.gz> --out <output> [options]
       vs-sieve <part1> <part2> ... --join <x|y|z> --out <output> [options]
       vs-sieve --phantom <n> --out <output> [options]

Reads a raw volume (x fastest). Dimensions and voxel size come from <input>.json (as written by
vs-phantom) unless given on the command line. A vendor header before the voxel data is detected
from the file size and skipped; use --header when the file also has a footer. A gzip-compressed
raw file (<name>.raw.gz with <name>.json) is decompressed once, in slice order, into the staging
file; give --header when it has a header. Signed, 32-bit and float samples (--type) are read:
signed ones shifted into the unsigned range, 32-bit and float ones mapped onto 16-bit grey values
over --value-range (docs/adr/0040).

Also reads MetaImage (.mhd, .mha) and NRRD (.nhdr, .nrrd) files, whose header gives the layout,
the spacing and the data files, PNG stacks (a directory or ZIP archive of slices), and volumes
in HDF5 files (.h5, .nxs; the largest three-dimensional dataset, or --dataset; docs/adr/0041).

Also reads TIFF stacks: a directory of slices, a multi-page TIFF or a ZIP archive of either,
without extracting it. Slices are sorted by name, numbers by value. The voxel size comes from
--voxel-size, else from the files, else 1 mm. Float slices are mapped linearly onto 16-bit grey
values over --value-range (default: estimated from a few slices); the mapping is recorded in the
dataset (docs/adr/0015).

Also reads DICOM stacks, a directory of slice files sorted by their position, and the first
volume of a gzipped XML project from the DICOM files it refers to (docs/adr/0019). Signed
samples and the rescale slope and intercept are recorded as the value mapping.

Grey values are stored in the input's width, 8 or 16 bit, unchanged (docs/adr/0021). Use
--value-type float for viewers such as Blender and Houdini, which cannot read these grids.

Several inputs are joined one after another along --join (default z) into one volume, for scans
reconstructed in parts. Their other dimensions and voxel sizes must match.

An interrupted import into a dataset directory resumes when it is run again with the same input
and options: the first pass and the bricks already written are skipped.

Output:
  <dir>                   Bricked multi-resolution dataset (streaming, any volume size;
                          see docs/adr/0004). Used for every --out not ending in .vdb.
  <file.vdb>              Single grid, built in memory (small volumes only)

Options:
  --out <path>            Output dataset directory or .vdb file (required)
  --dims <x> <y> <z>      Volume dimensions in voxels
  --voxel-size <mm>       Voxel edge length in mm, or x,y,z when the voxels are not cubes
                          (e.g. 0.33,0.33,0.6 for a coarser slice spacing)
  --slice-thickness <mm>  Slices thinner than their spacing (a gap between slices); recorded,
                          measurements use the spacing
  --type <type>           Sample type of the raw file: uint8, uint16 (default), int8, int16,
                          uint32, int32, float32 or float64
  --big-endian            Samples of several bytes are big endian (default little endian)
  --header <bytes>        Header size; default: file size minus voxel data
  --folder <name>         TIFF and PNG stacks: folder of the slices when there are several
  --dataset <path>        HDF5 files: the volume inside the file (default: the largest)
  --value-range <lo,hi>   Float TIFF stacks, 32-bit and float raw data: values mapped to grey 0
                          and 65535; values outside are clipped and counted
  --join <x|y|z>          Axis along which several inputs are joined (default z)
  --phantom <n>           Use a computed n^3 phantom instead of an input file (benchmarks)
)";

/// The rest of the options, after those of the sieve.
constexpr std::string_view kUsageTail =
    R"(  --dense                 .vdb only: write every voxel without sieving (baseline)
  --preview <file.png>    Datasets: write a first look at the input as soon as it is read: a
                          few slices spread over the volume, its sections and histogram with
                          the estimated threshold. A staged input rewrites it sharper as more
                          slices are copied. The import reads every slice only once
)";

/// The options of the sieve: parameters of the import operations (sieveParameters), so that
/// their checks, defaults and descriptions are the same here.
struct SieveFlag {
  std::string_view flag;
  std::string_view value;
  std::string_view parameter;
};

constexpr std::array<SieveFlag, 7> kSieveFlags{{
    {"--threshold", "<value>", "threshold"},
    {"--margin", "<voxels>", "margin_voxels"},
    {"--brick-size", "<voxels>", "brick_size"},
    {"--air-from", "<axes>", "outside_air_axes"},
    {"--min-material", "<n>", "min_material_voxels"},
    {"--value-type", "<type>", "value_type"},
    {"--staging-dir", "<dir>", "staging_dir"},
}};

/// One option of the usage text: its name in a column of 26, the description wrapped at 100.
std::string usageEntry(std::string name, const std::string& description) {
  constexpr std::size_t kColumn = 26;
  constexpr std::size_t kWidth = 100;
  std::string out = "  " + std::move(name);
  std::string line =
      out.size() < kColumn ? out + std::string(kColumn - out.size(), ' ') : out + ' ';
  out.clear();
  std::istringstream words(description);
  std::string word;
  bool first = true;
  while (words >> word) {
    if (!first && line.size() + 1 + word.size() > kWidth) {
      out += line + '\n';
      line = std::string(kColumn, ' ');
      first = true;
    }
    line += (first ? "" : " ") + word;
    first = false;
  }
  return out + line + '\n';
}

/// The usage text, with the options of the sieve from their schema.
std::string usage() {
  const nlohmann::json parameters = voxelsieve::sieveParameters();
  std::string text(kUsageHead);
  for (const SieveFlag& flag : kSieveFlags) {
    const nlohmann::json& schema = parameters.at(std::string(flag.parameter));
    std::string description = schema.value("description", std::string());
    std::string note;
    // The description with what it may be and its default in brackets.
    if (const auto values = schema.find("enum"); values != schema.end()) {
      for (std::size_t i = 0; i < values->size(); ++i) {
        note += (i == 0                    ? ""
                 : i + 1 == values->size() ? " or "
                                           : ", ") +
                (*values)[i].get<std::string>();
      }
    }
    if (const auto value = schema.find("default"); value != schema.end()) {
      note += (note.empty() ? "default " : "; default ") +
              (value->is_string() ? value->get<std::string>() : value->dump());
    }
    if (!note.empty()) {
      description.append(" (").append(note).append(")");
    }
    text += usageEntry(std::string(flag.flag).append(" ").append(flag.value), description);
  }
  text += usageEntry("--no-staging",
                     "Read slow inputs directly instead of copying them to a temporary raw file "
                     "first: no disk space, but more decoding (parameter staging)");
  text += kUsageTail;
  return text;
}

struct Options {
  std::filesystem::path input;  // the input being opened; the first of `inputs` while parsing
  std::vector<std::filesystem::path> inputs;
  int join_axis = 2;
  std::filesystem::path out;
  std::optional<std::array<std::int64_t, 3>> dims;
  std::optional<voxelsieve::VoxelSize> voxel_size;
  std::optional<double> slice_thickness_mm;
  voxelsieve::RawSampleType sample_type = voxelsieve::RawSampleType::kUInt16;
  std::endian byte_order = std::endian::little;
  std::optional<std::uint64_t> header_bytes;
  std::string folder;
  std::string dataset_path;
  std::optional<std::array<double, 2>> value_range;
  voxelsieve::SieveOptions sieve;
  nlohmann::json sieve_parameters = nlohmann::json::object();
  voxelsieve::DatasetOptions dataset;
  std::optional<std::int64_t> phantom;
  bool dense = false;
  std::optional<voxelsieve::ValueType> value_type;  // unset: the input's
  std::filesystem::path preview;

  [[nodiscard]] bool singleGrid() const { return out.extension() == ".vdb"; }
};

Options parse(voxelsieve::cli::Arguments& args) {
  Options options;
  while (args.next()) {
    if (args.is("--out")) {
      options.out = args.value();
    } else if (args.is("--dims")) {
      std::array<std::int64_t, 3> dims{};
      for (auto& d : dims) {
        d = args.integer();
      }
      options.dims = dims;
    } else if (args.is("--type")) {
      const std::string type = args.value();
      const auto parsed = voxelsieve::rawSampleTypeFromName(type);
      if (!parsed) {
        throw voxelsieve::cli::UsageError(
            "--type must be uint8, uint16, int8, int16, uint32, int32, float32 or float64");
      }
      options.sample_type = *parsed;
    } else if (args.is("--big-endian")) {
      options.byte_order = std::endian::big;
    } else if (args.is("--folder")) {
      options.folder = args.value();
    } else if (args.is("--dataset")) {
      options.dataset_path = args.value();
    } else if (args.is("--value-range")) {
      const std::string text = args.value();
      const auto comma = text.find(',');
      if (comma == std::string::npos) {
        throw voxelsieve::cli::UsageError("--value-range needs two numbers: <lo>,<hi>");
      }
      options.value_range = {std::stod(text.substr(0, comma)), std::stod(text.substr(comma + 1))};
    } else if (args.is("--header")) {
      options.header_bytes = std::stoull(args.value());
    } else if (args.is("--voxel-size")) {
      options.voxel_size = voxelsieve::parseVoxelSize(args.value());
    } else if (args.is("--slice-thickness")) {
      options.slice_thickness_mm = args.number();
    } else if (args.is("--phantom")) {
      options.phantom = args.integer();
    } else if (args.is("--no-staging")) {
      options.sieve_parameters["staging"] = false;
    } else if (const auto flag = std::ranges::find(kSieveFlags, args.arg(), &SieveFlag::flag);
               flag != kSieveFlags.end()) {
      const std::string key(flag->parameter);
      const std::string type = voxelsieve::sieveParameters().at(key).at("type");
      options.sieve_parameters[key] = type == "number"    ? nlohmann::json(args.number())
                                      : type == "integer" ? nlohmann::json(args.integer())
                                                          : nlohmann::json(args.value());
    } else if (args.is("--preview")) {
      options.preview = args.value();
    } else if (args.is("--dense")) {
      options.dense = true;
    } else if (args.is("--join")) {
      const std::string axis = args.value();
      if (axis != "x" && axis != "y" && axis != "z") {
        throw voxelsieve::cli::UsageError("--join must be x, y or z");
      }
      options.join_axis = axis[0] - 'x';
    } else if (args.positional()) {
      options.inputs.emplace_back(args.arg());
      options.input = options.inputs.front();
    } else {
      args.unknown();
    }
  }
  try {
    const nlohmann::json parameters = voxelsieve::validateParameters(
        {{"type", "object"}, {"properties", voxelsieve::sieveParameters()}},
        options.sieve_parameters);
    options.dataset = voxelsieve::sieveOptions(parameters);
  } catch (const std::invalid_argument& error) {
    // The message names the parameter; name the option instead.
    std::string message = error.what();
    for (const SieveFlag& flag : kSieveFlags) {
      const std::string name = "Parameter '" + std::string(flag.parameter) + "'";
      if (const auto at = message.find(name); at != std::string::npos) {
        message.replace(at, name.size(), flag.flag);
      }
    }
    throw voxelsieve::cli::UsageError(message);
  }
  options.value_type = options.dataset.value_type;
  options.sieve.threshold = options.dataset.threshold;
  options.sieve.margin_voxels = options.dataset.margin_voxels;
  options.sieve.min_material_voxels = options.dataset.min_material_voxels;
  options.sieve.outside_air_axes = options.dataset.outside_air_axes;
  if (options.out.empty() || (options.input.empty() == !options.phantom)) {
    throw voxelsieve::cli::UsageError("--out and either an input file or --phantom are required");
  }
  if (options.phantom && options.singleGrid()) {
    throw voxelsieve::cli::UsageError("--phantom writes datasets only; use a directory for --out");
  }
  return options;
}

struct Geometry {
  std::array<std::int64_t, 3> dims{};
  voxelsieve::VoxelSize voxel_size;
};

/// The voxel size with the slice thickness from the command line applied.
voxelsieve::VoxelSize withThickness(voxelsieve::VoxelSize size, const Options& options) {
  if (options.slice_thickness_mm) {
    size.slice_thickness_mm = *options.slice_thickness_mm;
  }
  size.validate();
  return size;
}

/// Takes dims and voxel size from the command line, falling back to the JSON sidecar.
Geometry resolveGeometry(const Options& options) {
  if (options.dims && options.voxel_size) {
    return {*options.dims, withThickness(*options.voxel_size, options)};
  }
  const auto sidecar = voxelsieve::rawSidecarPath(options.input);
  if (!std::filesystem::exists(sidecar)) {
    throw std::invalid_argument("No --dims/--voxel-size given and no sidecar " + sidecar.string());
  }
  std::ifstream in(sidecar);
  const auto json = nlohmann::json::parse(in);
  return {options.dims ? *options.dims : json.at("dims").get<std::array<std::int64_t, 3>>(),
          withThickness(options.voxel_size ? *options.voxel_size : voxelsieve::readVoxelSize(json),
                        options)};
}

double megabytes(std::uintmax_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); }

double seconds(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
  return std::chrono::duration<double>(b - a).count();
}

std::uintmax_t directorySize(const std::filesystem::path& dir) {
  std::uintmax_t bytes = 0;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
    if (entry.is_regular_file()) {
      bytes += entry.file_size();
    }
  }
  return bytes;
}

bool isTiffInput(const Options& options) {
  return options.input.extension() != ".raw" && voxelsieve::isTiffStackPath(options.input);
}

/// Float TIFF inputs, to report their clipped values after the run.
using FloatInputs = std::vector<const voxelsieve::VolumeSource*>;

/// A DICOM stack: a directory or ZIP archive of slices, or the first volume of a gzipped XML
/// project.
std::unique_ptr<voxelsieve::VolumeSource> openDicom(const Options& options) {
  voxelsieve::DicomStackOptions dicom;
  dicom.voxel_size = options.voxel_size;
  std::unique_ptr<voxelsieve::DicomStackSource> source;
  if (voxelsieve::isGzippedXmlFile(options.input)) {
    const voxelsieve::GzippedXmlProject project = voxelsieve::readGzippedXmlProject(options.input);
    if (project.volumes.empty() || project.volumes.front().format != "dicom") {
      throw std::runtime_error("The project's first volume is not a DICOM stack");
    }
    const voxelsieve::GzippedXmlVolume& volume = project.volumes.front();
    std::vector<std::filesystem::path> files;
    for (const voxelsieve::GzippedXmlFile& file : volume.files) {
      if (!file.path) {
        throw std::runtime_error("Not found: " + file.reference);
      }
      files.push_back(*file.path);
    }
    std::cout << "gzipped xml project   volume '" << volume.name << "' of "
              << project.volumes.size() << "\n";
    dicom.voxel_size = dicom.voxel_size.value_or(volume.voxel_size);
    source = std::make_unique<voxelsieve::DicomStackSource>(files, dicom);
  } else {
    source = std::make_unique<voxelsieve::DicomStackSource>(options.input, dicom);
  }
  if (options.slice_thickness_mm) {
    dicom.voxel_size = withThickness(source->voxelSize(), options);
    source = source->archive().empty()
                 ? std::make_unique<voxelsieve::DicomStackSource>(source->files(), dicom)
                 : std::make_unique<voxelsieve::DicomStackSource>(options.input, dicom);
  }
  const auto dims = source->dims();
  std::cout << "dicom stack        " << dims[2] << " slices of " << dims[0] << "x" << dims[1]
            << ", " << source->bitsStored() << " bit "
            << (source->isSigned() ? "signed" : "unsigned") << "\n";
  for (const auto& [uid, slices] : source->otherSeries()) {
    std::cout << "also in input      series " << uid << " (" << slices << " slices)\n";
  }
  if (!source->valueMapping().isIdentity()) {
    std::cout << "value              " << source->valueMapping().offset << " + "
              << source->valueMapping().scale << " * grey\n";
  }
  std::cout << "voxel size         " << voxelsieve::describe(source->voxelSize()) << "\n";
  return source;
}

bool isDicomInput(const Options& options) {
  return voxelsieve::isGzippedXmlFile(options.input) || voxelsieve::isDicomFile(options.input) ||
         voxelsieve::isDicomArchive(options.input) ||
         (std::filesystem::is_directory(options.input) && voxelsieve::containsDicom(options.input));
}

/// Prints how the samples of a raw source became grey values and keeps it for reportClipped.
void reportMapping(const voxelsieve::VolumeSource& source, const Options& options,
                   FloatInputs& floats) {
  if (const auto clipped = voxelsieve::rawClippedValues(source)) {
    std::cout << "values             " << clipped->range[0] << " .. " << clipped->range[1]
              << " -> grey 0 .. 65535"
              << (options.value_range ? "" : " (estimated; set --value-range)") << "\n";
    floats.push_back(&source);
  }
  if (const auto mapping = source.valueMapping(); !mapping.isIdentity()) {
    std::cout << "value              " << mapping.offset << " + " << mapping.scale << " * grey\n";
  }
}

std::unique_ptr<voxelsieve::VolumeSource> openPart(const Options& options, FloatInputs& floats) {
  if (isDicomInput(options)) {
    return openDicom(options);
  }
  if (voxelsieve::isHdf5File(options.input)) {
    voxelsieve::Hdf5Options hdf5;
    hdf5.dataset = options.dataset_path;
    hdf5.value_range = options.value_range;
    hdf5.voxel_size = options.voxel_size;
    if (options.slice_thickness_mm) {
      hdf5.voxel_size = withThickness(
          hdf5.voxel_size.value_or(voxelsieve::Hdf5Source(options.input, hdf5).voxelSize()),
          options);
    }
    auto source = std::make_unique<voxelsieve::Hdf5Source>(options.input, hdf5);
    const auto dims = source->dims();
    std::cout << "hdf5               " << source->volume().path << ", " << dims[0] << "x" << dims[1]
              << "x" << dims[2] << " "
              << voxelsieve::rawSampleTypeName(source->volume().sample_type) << "\n";
    for (const auto& other : source->otherVolumes()) {
      std::cout << "also in input      " << other.path << " (choose with --dataset)\n";
    }
    if (const auto range = source->valueRange()) {
      std::cout << "values             " << (*range)[0] << " .. " << (*range)[1]
                << " -> grey 0 .. 65535"
                << (options.value_range ? "" : " (estimated; set --value-range)") << "\n";
      floats.push_back(source.get());
    }
    if (const auto mapping = source->valueMapping(); !mapping.isIdentity()) {
      std::cout << "value              " << mapping.offset << " + " << mapping.scale << " * grey\n";
    }
    std::cout << "voxel size         " << voxelsieve::describe(source->voxelSize())
              << (source->voxelSizeFrom() == "assumed" ? " (assumed; set --voxel-size)" : "")
              << "\n";
    return source;
  }
  if (voxelsieve::isHeaderVolumeFile(options.input)) {
    const voxelsieve::HeaderVolume header = voxelsieve::readHeaderVolume(options.input);
    std::optional<voxelsieve::VoxelSize> size = options.voxel_size;
    if (options.slice_thickness_mm) {
      size = withThickness(size.value_or(header.layout.voxel_size), options);
    }
    auto source = voxelsieve::openHeaderVolume(header, size, options.value_range);
    const auto dims = source->dims();
    std::cout << header.format << (header.format == "nrrd" ? "               " : "          ")
              << dims[0] << "x" << dims[1] << "x" << dims[2] << " "
              << voxelsieve::rawSampleTypeName(voxelsieve::storedSampleType(header.layout))
              << (header.compressed ? ", compressed" : "") << ", " << header.files.size()
              << " data file(s)\n";
    for (const std::string& note : header.notes) {
      std::cout << "note               " << note << "\n";
    }
    reportMapping(*source, options, floats);
    if (!header.voxel_size_known && !options.voxel_size) {
      std::cout << "voxel size         not in the header, 1 mm assumed (set --voxel-size)\n";
    } else {
      std::cout << "voxel size         " << voxelsieve::describe(source->voxelSize()) << "\n";
    }
    return source;
  }
  if ((std::filesystem::is_directory(options.input) && !voxelsieve::containsTiff(options.input)) ||
      options.input.extension() == ".zip") {
    if (voxelsieve::containsPngSlices(options.input)) {
      voxelsieve::PngStackOptions png;
      png.folder = options.folder;
      png.voxel_size = options.voxel_size;
      if (options.slice_thickness_mm) {
        png.voxel_size = withThickness(
            png.voxel_size.value_or(voxelsieve::PngStackSource(options.input, png).voxelSize()),
            options);
      }
      auto source = std::make_unique<voxelsieve::PngStackSource>(options.input, png);
      const auto dims = source->dims();
      std::cout << "png stack          " << dims[2] << " slices of " << dims[0] << "x" << dims[1]
                << ", " << source->bitDepth() << " bit"
                << (source->folder().empty() ? "" : ", folder " + source->folder()) << "\n";
      for (const std::string& other : source->otherFolders()) {
        std::cout << "also in input      " << other << " (choose with --folder)\n";
      }
      std::cout << "voxel size         " << voxelsieve::describe(source->voxelSize())
                << (source->voxelSizeKnown() ? "" : " (assumed; set --voxel-size)") << "\n";
      return source;
    }
  }
  if (isTiffInput(options)) {
    voxelsieve::TiffStackOptions tiff;
    tiff.folder = options.folder;
    tiff.value_range = options.value_range;
    tiff.voxel_size = options.voxel_size;
    if (options.slice_thickness_mm) {
      tiff.voxel_size = withThickness(
          tiff.voxel_size.value_or(voxelsieve::TiffStackSource(options.input, tiff).voxelSize()),
          options);
    }
    auto source = std::make_unique<voxelsieve::TiffStackSource>(options.input, tiff);
    const auto dims = source->dims();
    std::cout << "tiff stack         " << dims[2] << " slices of " << dims[0] << "x" << dims[1]
              << ", " << source->bitsPerSample() << " bit"
              << (source->folder().empty() ? "" : ", folder " + source->folder()) << "\n";
    for (const std::string& other : source->otherFolders()) {
      std::cout << "also in input      " << other << " (choose with --folder)\n";
    }
    if (source->isFloat()) {
      const auto range = source->valueRange();
      std::cout << "float values       " << range[0] << " .. " << range[1] << " -> grey 0 .. 65535"
                << (options.value_range ? "" : " (estimated; set --value-range)") << "\n"
                << "value              " << source->valueMapping().offset << " + "
                << source->valueMapping().scale << " * grey\n";
      floats.push_back(source.get());
    }
    if (!tiff.voxel_size && !source->fileVoxelSize()) {
      std::cout << "voxel size         unknown in the files, 1 mm assumed (set --voxel-size)\n";
    } else {
      std::cout << "voxel size         " << voxelsieve::describe(source->voxelSize()) << "\n";
    }
    return source;
  }
  const Geometry geometry = resolveGeometry(options);
  std::uint64_t header_bytes = 0;
  voxelsieve::RawLayout layout{geometry.dims, geometry.voxel_size,
                               options.sample_type == voxelsieve::RawSampleType::kUInt8
                                   ? voxelsieve::SampleType::kUInt8
                                   : voxelsieve::SampleType::kUInt16,
                               options.byte_order, options.header_bytes};
  if (options.sample_type != voxelsieve::RawSampleType::kUInt8 &&
      options.sample_type != voxelsieve::RawSampleType::kUInt16) {
    layout.stored_type = options.sample_type;
  }
  layout.value_range = options.value_range;
  auto source = voxelsieve::openRawVolume(options.input, layout, &header_bytes);
  reportMapping(*source, options, floats);
  if (source->sequentialAccess()) {
    std::cout << "gzip               decompressed once, in slice order\n";
  }
  if (header_bytes > 0) {
    std::cout << "header             " << header_bytes << " bytes skipped\n";
  }
  std::cout << "voxel size         " << voxelsieve::describe(source->voxelSize()) << "\n";
  return source;
}

std::unique_ptr<voxelsieve::VolumeSource> openSource(const Options& options, FloatInputs& floats) {
  if (options.inputs.size() <= 1) {
    return openPart(options, floats);
  }
  std::vector<std::unique_ptr<voxelsieve::VolumeSource>> parts;
  for (const auto& input : options.inputs) {
    std::cout << "part " << parts.size() + 1 << "             " << input.filename().string()
              << "\n";
    Options part = options;
    part.input = input;
    parts.push_back(openPart(part, floats));
  }
  auto joined = std::make_unique<voxelsieve::ConcatSource>(std::move(parts), options.join_axis);
  const auto dims = joined->dims();
  std::cout << "joined along " << static_cast<char>('x' + options.join_axis) << "     " << dims[0]
            << "x" << dims[1] << "x" << dims[2] << " voxels\n";
  return joined;
}

void reportClipped(const FloatInputs& floats) {
  std::uint64_t clipped = 0;
  for (const auto* source : floats) {
    if (const auto* tiff = dynamic_cast<const voxelsieve::TiffStackSource*>(source)) {
      clipped += tiff->clippedValues();
    } else if (const auto* hdf5 = dynamic_cast<const voxelsieve::Hdf5Source*>(source)) {
      clipped += hdf5->clippedValues();
    } else if (const auto raw = voxelsieve::rawClippedValues(*source)) {
      clipped += raw->clipped;
    }
  }
  if (!floats.empty()) {
    std::cout << "clipped values     " << clipped
              << (clipped > 0 ? " outside the value range (widen --value-range)" : "") << "\n";
  }
}

void runSingleGrid(const Options& options) {
  const auto start = std::chrono::steady_clock::now();
  FloatInputs floats;
  const auto source = openSource(options, floats);
  voxelsieve::Volume16 volume(source->dims(), source->voxelSize());
  source->readRegion({{0, 0, 0}, volume.dims}, volume.data);
  const auto loaded = std::chrono::steady_clock::now();

  openvdb::FloatGrid::Ptr grid;
  if (options.dense) {
    grid = voxelsieve::toDenseFloatGrid(volume);
  } else {
    const auto result = voxelsieve::sieve(volume, options.sieve);
    grid = result.grid;
    const auto& s = result.stats;
    std::cout << "threshold          " << s.threshold << " (air level " << s.air_level << ")\n"
              << "blocks             " << s.block_count << " total, " << s.material_block_count
              << " material, " << s.outside_air_block_count << " outside air\n";
  }
  if (const auto mapping = source->valueMapping(); !mapping.isIdentity()) {
    // Float scans: value = value_offset + value_scale * grey (docs/adr/0015).
    grid->insertMeta("value_offset", openvdb::DoubleMetadata(mapping.offset));
    grid->insertMeta("value_scale", openvdb::DoubleMetadata(mapping.scale));
  }
  // The input's width unless asked otherwise (docs/adr/0021).
  const voxelsieve::ValueType type = options.value_type.value_or(
      source->sampleType() == voxelsieve::SampleType::kUInt8 ? voxelsieve::ValueType::kUInt8
                                                             : voxelsieve::ValueType::kUInt16);
  const voxelsieve::GreyGrid stored = type == voxelsieve::ValueType::kFloat
                                          ? voxelsieve::GreyGrid(grid)
                                          : voxelsieve::convertGrid(*grid, type);
  std::cout << "values             " << voxelsieve::valueTypeName(type) << "\n";
  const auto converted = std::chrono::steady_clock::now();
  voxelsieve::writeVdb(options.out, {stored.basePtr()});
  const auto written = std::chrono::steady_clock::now();

  const auto active = grid->activeVoxelCount();
  const auto raw_bytes = static_cast<std::uintmax_t>(volume.voxelCount()) * 2U;
  const auto vdb_bytes = std::filesystem::file_size(options.out);
  std::cout << std::fixed << std::setprecision(2) << "active voxels      " << active << " of "
            << volume.voxelCount() << " ("
            << 100.0 * static_cast<double>(active) / static_cast<double>(volume.voxelCount())
            << " %)\n"
            << "file size          " << megabytes(raw_bytes) << " MB raw -> "
            << megabytes(vdb_bytes) << " MB vdb ("
            << 100.0 * static_cast<double>(vdb_bytes) / static_cast<double>(raw_bytes) << " %)\n"
            << "time               read " << seconds(start, loaded) << " s, convert "
            << seconds(loaded, converted) << " s, write " << seconds(converted, written) << " s\n";
  reportClipped(floats);
}

/// Progress of the streaming sieve on stderr: one line per stage that is rewritten in place on a
/// terminal, and a line every 10 % otherwise (logs).
class ProgressPrinter {
 public:
  void operator()(std::string_view stage, double fraction) {
    const auto now = std::chrono::steady_clock::now();
    if (stage != stage_) {
      finishLine();
      stage_ = stage;
      stage_start_ = now;
      last_logged_ = -1;
    }
    const auto percent = static_cast<int>(fraction * 100.0);
    if (!terminal_ && percent / 10 == last_logged_ / 10 && percent < 100) {
      return;
    }
    last_logged_ = percent;
    const double elapsed = seconds(stage_start_, now);
    std::ostringstream line;
    line << std::left << std::setw(19) << (label(stage) + " ") << std::right << std::setw(3)
         << percent << " %, " << std::fixed << std::setprecision(0) << elapsed << " s";
    if (fraction > 0.02 && fraction < 1.0) {
      line << ", about " << elapsed * (1.0 - fraction) / fraction << " s left";
    }
    std::cerr << (terminal_ ? "\r\033[K" : "") << line.str() << (terminal_ ? "" : "\n")
              << std::flush;
    open_line_ = terminal_;
  }

  void finishLine() {
    if (open_line_) {
      std::cerr << "\n";
      open_line_ = false;
    }
  }

 private:
  static std::string label(std::string_view stage) {
    if (stage == "staging") {
      return "staging (decode)";
    }
    if (stage == "histogram") {
      return "pass 1 (histogram)";
    }
    if (stage == "bricks") {
      return "pass 2 (bricks)";
    }
    return std::string(stage);
  }

#ifdef _WIN32
  bool terminal_ = _isatty(_fileno(stderr)) != 0;
#else
  bool terminal_ = isatty(fileno(stderr)) != 0;
#endif
  std::string stage_;
  std::chrono::steady_clock::time_point stage_start_;
  int last_logged_ = -1;
  bool open_line_ = false;
};

void runDataset(const Options& options) {
  std::unique_ptr<voxelsieve::VolumeSource> source;
  FloatInputs floats;
  if (options.phantom) {
    voxelsieve::PhantomSpec spec = voxelsieve::defaultPhantomSpec();
    const std::int64_t n = *options.phantom;
    spec.dims = {n, n, n};
    spec.voxel_size = 12.8 / static_cast<double>(n);  // same geometry at any resolution
    spec.noise_sigma = 500.0;
    source = std::make_unique<voxelsieve::PhantomSource>(spec);
  } else {
    source = openSource(options, floats);
  }

  const auto start = std::chrono::steady_clock::now();
  ProgressPrinter printer;
  voxelsieve::DatasetOptions dataset = options.dataset;
  dataset.progress = [&printer](std::string_view stage, double fraction) {
    printer(stage, fraction);
  };
  if (!options.preview.empty()) {
    dataset.preview = [&](const voxelsieve::ImportPreview& preview) {
      const std::vector<std::uint8_t> png = voxelsieve::previewPng(preview);
      std::ofstream(options.preview, std::ios::binary)
          .write(reinterpret_cast<const char*>(png.data()),
                 static_cast<std::streamsize>(png.size()));
      printer.finishLine();
      std::cout << std::fixed << std::setprecision(1) << "preview            after "
                << seconds(start, std::chrono::steady_clock::now()) << " s, " << preview.slices_read
                << " of " << preview.source_dims[2] << " slices, threshold estimate "
                << std::setprecision(0) << preview.threshold << " -> " << options.preview.string()
                << '\n'
                << std::flush;
    };
  }
  const auto info = voxelsieve::writeDataset(*source, options.out, dataset);
  printer.finishLine();
  const auto done = std::chrono::steady_clock::now();

  const auto dims = source->dims();
  const double voxels =
      static_cast<double>(dims[0]) * static_cast<double>(dims[1]) * static_cast<double>(dims[2]);
  const double raw_mb = voxels * 2.0 / (1024.0 * 1024.0);
  std::cout << std::fixed << std::setprecision(2) << "threshold          " << info.threshold
            << " (air level " << info.air_level << ")\n"
            << "values             " << voxelsieve::valueTypeName(info.value_type) << "\n";
  for (const auto& level : info.levels) {
    std::cout << "level " << level.level << "            " << level.bricks.size() << " bricks, "
              << level.dims[0] << "x" << level.dims[1] << "x" << level.dims[2] << " voxels\n";
  }
  std::cout << "active voxels      " << info.active_voxel_count << " ("
            << 100.0 * static_cast<double>(info.active_voxel_count) / voxels << " %)\n"
            << "size               " << raw_mb << " MB raw -> "
            << megabytes(directorySize(options.out)) << " MB dataset\n"
            << "time               " << seconds(start, done) << " s\n";
  reportClipped(floats);
}

int run(const Options& options) {
  if (options.singleGrid()) {
    runSingleGrid(options);
  } else {
    runDataset(options);
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  return voxelsieve::cli::runTool("vs-sieve", usage(), argc, argv, parse, run);
}
