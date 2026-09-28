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
#include <string>
#include <string_view>

#include "voxelsieve/dataset.hpp"
#include "voxelsieve/sieve.hpp"
#include "voxelsieve/tiff.hpp"
#include "voxelsieve/vdb.hpp"

namespace {

constexpr std::string_view kUsage = R"(Usage: vs-sieve <input.raw> --out <output> [options]
       vs-sieve <slices/ | stack.tif | slices.zip> --out <output> [options]
       vs-sieve --phantom <n> --out <output> [options]

Reads a raw volume (x fastest). Dimensions and voxel size come from <input>.json (as written by
vs-phantom) unless given on the command line. A vendor header before the voxel data is detected
from the file size and skipped; use --header when the file also has a footer.

Also reads TIFF stacks: a directory of slices, a multi-page TIFF or a ZIP archive of either,
without extracting it. Slices are sorted by name, numbers by value. The voxel size comes from
--voxel-size, else from the files, else 1 mm.

Output:
  <dir>                   Bricked multi-resolution dataset (streaming, any volume size;
                          see docs/adr/0004). Used for every --out not ending in .vdb.
  <file.vdb>              Single grid, built in memory (small volumes only)

Options:
  --out <path>            Output dataset directory or .vdb file (required)
  --dims <x> <y> <z>      Volume dimensions in voxels
  --voxel-size <mm>       Voxel edge length in mm
  --type <uint16|uint8>   Sample type of the raw file (default uint16)
  --big-endian            16-bit samples are big endian (default little endian)
  --header <bytes>        Header size; default: file size minus voxel data
  --folder <name>         TIFF stacks: folder of the slices when there are several
  --phantom <n>           Use a computed n^3 phantom instead of an input file (benchmarks)
  --threshold <value>     Air/material grey value (default: Otsu estimate)
  --margin <voxels>       Air margin kept around the part (default 3)
  --brick-size <voxels>   Brick edge length for datasets, multiple of 8 (default 256)
  --min-material <n>      .vdb only: voxels above threshold for a block to count as material
  --dense                 .vdb only: write every voxel without sieving (baseline)
  -h, --help              Show this help
)";

struct Options {
  std::filesystem::path input;
  std::filesystem::path out;
  std::optional<std::array<std::int64_t, 3>> dims;
  std::optional<double> voxel_size_mm;
  voxelsieve::SampleType sample_type = voxelsieve::SampleType::kUInt16;
  std::endian byte_order = std::endian::little;
  std::optional<std::uint64_t> header_bytes;
  std::string folder;
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
    } else if (arg == "--header") {
      options.header_bytes = std::stoull(next());
    } else if (arg == "--voxel-size") {
      options.voxel_size_mm = std::stod(next());
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
    } else if (arg == "--dense") {
      options.dense = true;
    } else if (!arg.starts_with("-") && options.input.empty()) {
      options.input = arg;
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
  double voxel_size_mm = 0.0;
};

/// Takes dims and voxel size from the command line, falling back to the JSON sidecar.
Geometry resolveGeometry(const Options& options) {
  if (options.dims && options.voxel_size_mm) {
    return {*options.dims, *options.voxel_size_mm};
  }
  auto sidecar = options.input;
  sidecar.replace_extension(".json");
  if (!std::filesystem::exists(sidecar)) {
    throw std::invalid_argument("No --dims/--voxel-size given and no sidecar " + sidecar.string());
  }
  std::ifstream in(sidecar);
  const auto json = nlohmann::json::parse(in);
  return {options.dims ? *options.dims : json.at("dims").get<std::array<std::int64_t, 3>>(),
          options.voxel_size_mm ? *options.voxel_size_mm : json.at("voxel_size_mm").get<double>()};
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

std::unique_ptr<voxelsieve::VolumeSource> openSource(const Options& options) {
  if (isTiffInput(options)) {
    voxelsieve::TiffStackOptions tiff;
    tiff.folder = options.folder;
    tiff.voxel_size_mm = options.voxel_size_mm;
    auto source = std::make_unique<voxelsieve::TiffStackSource>(options.input, tiff);
    const auto dims = source->dims();
    std::cout << "tiff stack         " << dims[2] << " slices of " << dims[0] << "x" << dims[1]
              << ", " << source->bitsPerSample() << " bit"
              << (source->folder().empty() ? "" : ", folder " + source->folder()) << "\n";
    for (const std::string& other : source->otherFolders()) {
      std::cout << "also in input      " << other << " (choose with --folder)\n";
    }
    if (!options.voxel_size_mm && source->fileVoxelSizeMm() <= 0.0) {
      std::cout << "voxel size         unknown in the files, 1 mm assumed (set --voxel-size)\n";
    }
    return source;
  }
  const Geometry geometry = resolveGeometry(options);
  auto source = std::make_unique<voxelsieve::MappedRawSource>(
      options.input,
      voxelsieve::RawLayout{geometry.dims, geometry.voxel_size_mm, options.sample_type,
                            options.byte_order, options.header_bytes});
  if (source->headerBytes() > 0) {
    std::cout << "header             " << source->headerBytes() << " bytes skipped\n";
  }
  return source;
}

void runSingleGrid(const Options& options) {
  const auto start = std::chrono::steady_clock::now();
  const auto source = openSource(options);
  voxelsieve::Volume16 volume(source->dims(), source->voxelSizeMm());
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
}

void runDataset(const Options& options) {
  std::unique_ptr<voxelsieve::VolumeSource> source;
  if (options.phantom) {
    voxelsieve::PhantomSpec spec = voxelsieve::defaultPhantomSpec();
    const std::int64_t n = *options.phantom;
    spec.dims = {n, n, n};
    spec.voxel_size_mm = 12.8 / static_cast<double>(n);  // same geometry at any resolution
    spec.noise_sigma = 500.0;
    source = std::make_unique<voxelsieve::PhantomSource>(spec);
  } else {
    source = openSource(options);
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
