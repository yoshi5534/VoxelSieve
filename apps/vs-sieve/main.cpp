// vs-sieve: converts a raw CT volume into a sparse OpenVDB grid that keeps the part, its internal
// voids and an air margin, and drops the air connected to the volume boundary.

#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
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
#include "voxelsieve/sieve.hpp"
#include "voxelsieve/tiff.hpp"
#include "voxelsieve/vdb.hpp"

namespace {

constexpr std::string_view kUsage = R"(Usage: vs-sieve <input.raw> --out <output> [options]
       vs-sieve <slices/ | stack.tif | slices.zip> --out <output> [options]
       vs-sieve <part1> <part2> ... --join <x|y|z> --out <output> [options]
       vs-sieve --phantom <n> --out <output> [options]

Reads a raw volume (x fastest). Dimensions and voxel size come from <input>.json (as written by
vs-phantom) unless given on the command line. A vendor header before the voxel data is detected
from the file size and skipped; use --header when the file also has a footer.

Also reads TIFF stacks: a directory of slices, a multi-page TIFF or a ZIP archive of either,
without extracting it. Slices are sorted by name, numbers by value. The voxel size comes from
--voxel-size, else from the files, else 1 mm. Float slices are mapped linearly onto 16-bit grey
values over --value-range (default: estimated from a few slices); the mapping is recorded in the
dataset (docs/adr/0015).

Several inputs are joined one after another along --join (default z) into one volume, for scans
reconstructed in parts. Their other dimensions and voxel sizes must match.

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
  --type <uint16|uint8>   Sample type of the raw file (default uint16)
  --big-endian            16-bit samples are big endian (default little endian)
  --header <bytes>        Header size; default: file size minus voxel data
  --folder <name>         TIFF stacks: folder of the slices when there are several
  --value-range <lo,hi>   Float TIFF stacks: values mapped to grey 0 and 65535; values outside
                          are clipped and counted
  --join <x|y|z>          Axis along which several inputs are joined (default z)
  --phantom <n>           Use a computed n^3 phantom instead of an input file (benchmarks)
  --threshold <value>     Air/material grey value (default: Otsu estimate)
  --margin <voxels>       Air margin kept around the part (default 3)
  --brick-size <voxels>   Brick edge length for datasets, multiple of 8 (default 256)
  --min-material <n>      Voxels above threshold for an 8^3 block to count as material
                          (default 1); raise it for noisy scans
  --dense                 .vdb only: write every voxel without sieving (baseline)
  -h, --help              Show this help
)";

struct Options {
  std::filesystem::path input;  // the input being opened; the first of `inputs` while parsing
  std::vector<std::filesystem::path> inputs;
  int join_axis = 2;
  std::filesystem::path out;
  std::optional<std::array<std::int64_t, 3>> dims;
  std::optional<voxelsieve::VoxelSize> voxel_size;
  std::optional<double> slice_thickness_mm;
  voxelsieve::SampleType sample_type = voxelsieve::SampleType::kUInt16;
  std::endian byte_order = std::endian::little;
  std::optional<std::uint64_t> header_bytes;
  std::string folder;
  std::optional<std::array<double, 2>> value_range;
  voxelsieve::SieveOptions sieve;
  voxelsieve::DatasetOptions dataset;
  std::optional<std::int64_t> phantom;
  bool dense = false;

  [[nodiscard]] bool singleGrid() const { return out.extension() == ".vdb"; }
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
    if (arg == "-h" || arg == "--help") {
      return std::nullopt;
    } else if (arg == "--out") {
      options.out = next();
    } else if (arg == "--dims") {
      std::array<std::int64_t, 3> dims{};
      for (auto& d : dims) {
        d = std::stoll(next());
      }
      options.dims = dims;
    } else if (arg == "--type") {
      const std::string type = next();
      if (type != "uint16" && type != "uint8") {
        throw std::invalid_argument("--type must be uint16 or uint8");
      }
      options.sample_type =
          type == "uint8" ? voxelsieve::SampleType::kUInt8 : voxelsieve::SampleType::kUInt16;
    } else if (arg == "--big-endian") {
      options.byte_order = std::endian::big;
    } else if (arg == "--folder") {
      options.folder = next();
    } else if (arg == "--value-range") {
      const std::string text = next();
      const auto comma = text.find(',');
      if (comma == std::string::npos) {
        throw std::invalid_argument("--value-range needs two numbers: <lo>,<hi>");
      }
      options.value_range = {std::stod(text.substr(0, comma)), std::stod(text.substr(comma + 1))};
    } else if (arg == "--header") {
      options.header_bytes = std::stoull(next());
    } else if (arg == "--voxel-size") {
      options.voxel_size = voxelsieve::parseVoxelSize(next());
    } else if (arg == "--slice-thickness") {
      options.slice_thickness_mm = std::stod(next());
    } else if (arg == "--threshold") {
      options.sieve.threshold = std::stof(next());
      options.dataset.threshold = options.sieve.threshold;
    } else if (arg == "--margin") {
      options.sieve.margin_voxels = std::stoi(next());
      options.dataset.margin_voxels = options.sieve.margin_voxels;
    } else if (arg == "--brick-size") {
      options.dataset.brick_size = std::stoll(next());
    } else if (arg == "--phantom") {
      options.phantom = std::stoll(next());
    } else if (arg == "--min-material") {
      options.sieve.min_material_voxels = std::stoi(next());
      options.dataset.min_material_voxels = options.sieve.min_material_voxels;
    } else if (arg == "--dense") {
      options.dense = true;
    } else if (arg == "--join") {
      const std::string axis = next();
      if (axis != "x" && axis != "y" && axis != "z") {
        throw std::invalid_argument("--join must be x, y or z");
      }
      options.join_axis = axis[0] - 'x';
    } else if (!arg.starts_with("-")) {
      options.inputs.emplace_back(arg);
      options.input = options.inputs.front();
    } else {
      throw std::invalid_argument("Unknown option: " + std::string(arg));
    }
  }
  if (options.out.empty() || (options.input.empty() == !options.phantom)) {
    throw std::invalid_argument("--out and either an input file or --phantom are required");
  }
  if (options.phantom && options.singleGrid()) {
    throw std::invalid_argument("--phantom writes datasets only; use a directory for --out");
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
  auto sidecar = options.input;
  sidecar.replace_extension(".json");
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

/// Peak resident memory in MB from /proc (Linux), or a negative value where unavailable.
double peakMemoryMb() {
  std::ifstream status("/proc/self/status");
  std::string line;
  while (std::getline(status, line)) {
    if (line.starts_with("VmHWM:")) {
      return std::stod(line.substr(6)) / 1024.0;
    }
  }
  return -1.0;
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
using FloatInputs = std::vector<const voxelsieve::TiffStackSource*>;

std::unique_ptr<voxelsieve::VolumeSource> openPart(const Options& options, FloatInputs& floats) {
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
  auto source = std::make_unique<voxelsieve::MappedRawSource>(
      options.input, voxelsieve::RawLayout{geometry.dims, geometry.voxel_size, options.sample_type,
                                           options.byte_order, options.header_bytes});
  if (source->headerBytes() > 0) {
    std::cout << "header             " << source->headerBytes() << " bytes skipped\n";
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
    clipped += source->clippedValues();
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
  const auto converted = std::chrono::steady_clock::now();
  voxelsieve::writeVdb(options.out, {grid});
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
  const auto info = voxelsieve::writeDataset(*source, options.out, options.dataset);
  const auto done = std::chrono::steady_clock::now();

  const auto dims = source->dims();
  const double voxels =
      static_cast<double>(dims[0]) * static_cast<double>(dims[1]) * static_cast<double>(dims[2]);
  const double raw_mb = voxels * 2.0 / (1024.0 * 1024.0);
  std::cout << std::fixed << std::setprecision(2) << "threshold          " << info.threshold
            << " (air level " << info.air_level << ")\n";
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
  if (const double peak = peakMemoryMb(); peak >= 0.0) {
    // VmHWM also counts pages of a memory-mapped input, which the OS can reclaim at any time.
    std::cout << "peak memory        " << peak << " MB (incl. mapped input pages)\n";
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto options = parse(argc, argv);
    if (!options) {
      std::cout << kUsage;
      return 0;
    }
    if (options->singleGrid()) {
      runSingleGrid(*options);
    } else {
      runDataset(*options);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "vs-sieve: " << error.what() << "\n\n" << kUsage;
    return 1;
  }
}
