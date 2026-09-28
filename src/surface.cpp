#include "voxelsieve/surface.hpp"

#include <openvdb/tools/Dense.h>
#include <openvdb/tools/LevelSetRebuild.h>
#include <openvdb/tools/SignedFloodFill.h>
#include <openvdb/tools/VolumeToMesh.h>
#include <tbb/blocked_range.h>
#include <tbb/combinable.h>
#include <tbb/parallel_for.h>
#include <tbb/parallel_pipeline.h>

#include <algorithm>
#include <bit>
#include <boost/iostreams/device/back_inserter.hpp>
#include <boost/iostreams/device/mapped_file.hpp>
#include <boost/iostreams/filter/zstd.hpp>
#include <boost/iostreams/filtering_stream.hpp>
#include <cmath>
#include <cstring>
#include <fstream>
#include <list>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include "detail/png.hpp"

namespace voxelsieve {
namespace {

using Json = nlohmann::json;
using Index3 = std::array<std::int64_t, 3>;

constexpr std::int64_t kBlock = 8;
constexpr std::int64_t kBlockVoxels = kBlock * kBlock * kBlock;
constexpr std::string_view kMagic = "VSSURF1\n";
constexpr std::string_view kEndMagic = "VSSEND1\n";
constexpr std::size_t kTableEntryBytes = 17;  // kind, offset, size
constexpr std::size_t kTrailerBytes = 4 * sizeof(std::uint64_t) + kEndMagic.size();
/// Chunks processed at the same time by the writer; bounds its memory.
constexpr std::size_t kChunksInFlight = 4;

enum class BlockKind : std::uint8_t { kOutside = 0, kInside = 1, kSurface = 2 };

// ---------------------------------------------------------------------------------------------
// Little-endian helpers and compression.

void appendU64(std::vector<char>& out, std::uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<char>((value >> (8U * static_cast<unsigned>(i))) & 0xFFU));
  }
}

std::uint64_t readU64(const char* data) {
  std::uint64_t value = 0;
  for (int i = 7; i >= 0; --i) {
    value = (value << 8U) | static_cast<std::uint8_t>(data[i]);
  }
  return value;
}

constexpr int kDefaultLevel = 19;  // zstd's strongest regular level; see ADR 0009
constexpr std::string_view kCompression = "zstd";

std::vector<char> compress(const std::vector<char>& data, int level) {
  namespace io = boost::iostreams;
  std::vector<char> out;
  {
    io::filtering_ostream stream;
    stream.push(io::zstd_compressor(io::zstd_params(static_cast<std::uint32_t>(level))));
    stream.push(io::back_inserter(out));
    stream.write(data.data(), static_cast<std::streamsize>(data.size()));
  }
  return out;
}

std::vector<char> decompress(const char* data, std::size_t size, std::size_t expected) {
  namespace io = boost::iostreams;
  std::vector<char> out;
  out.reserve(expected);
  {
    io::filtering_ostream stream;
    stream.push(io::zstd_decompressor());
    stream.push(io::back_inserter(out));
    stream.write(data, static_cast<std::streamsize>(size));
  }
  return out;
}

// ---------------------------------------------------------------------------------------------
// Chunk layout shared by writer and reader.

/// Voxel box of chunk `chunk`, clipped to the volume, and its number of blocks per axis.
struct ChunkGeometry {
  Box box;
  Index3 blocks{};
  [[nodiscard]] std::int64_t blockCount() const { return blocks[0] * blocks[1] * blocks[2]; }
};

ChunkGeometry chunkGeometry(const Index3& dims, std::int64_t chunk_size, const Index3& chunk) {
  ChunkGeometry g;
  for (std::size_t i = 0; i < 3; ++i) {
    g.box.min[i] = chunk[i] * chunk_size;
    g.box.max[i] = std::min(dims[i], g.box.min[i] + chunk_size);
    g.blocks[i] = (g.box.size(i) + kBlock - 1) / kBlock;
  }
  return g;
}

Index3 chunkGrid(const SurfaceInfo& info) {
  Index3 grid{};
  for (std::size_t i = 0; i < 3; ++i) {
    grid[i] = (info.dims[i] + info.chunk_size - 1) / info.chunk_size;
  }
  return grid;
}

/// Payload of a chunk before compression: two bits of block kind per block (four blocks per byte,
/// lowest bits first), then the codes of every surface block with `bits` bits each, x fastest,
/// packed lowest bits first. A block's codes fill exactly 64 * bits bytes.
std::vector<char> packChunk(const std::vector<BlockKind>& kinds,
                            const std::vector<std::uint8_t>& codes, int bits) {
  std::vector<char> out((kinds.size() + 3) / 4, 0);
  for (std::size_t i = 0; i < kinds.size(); ++i) {
    out[i / 4] = static_cast<char>(static_cast<std::uint8_t>(out[i / 4]) |
                                   (static_cast<unsigned>(kinds[i]) << (2U * (i % 4))));
  }
  const auto width = static_cast<unsigned>(bits);
  std::uint32_t buffer = 0;
  unsigned used = 0;
  for (const std::uint8_t code : codes) {
    buffer |= static_cast<std::uint32_t>(code) << used;
    used += width;
    while (used >= 8) {
      out.push_back(static_cast<char>(buffer & 0xFFU));
      buffer >>= 8U;
      used -= 8;
    }
  }
  return out;
}

double fractionOf(float distance) {
  return std::clamp(0.5 - static_cast<double>(distance), 0.0, 1.0);
}

// ---------------------------------------------------------------------------------------------
// Writer.

struct ChunkOutput {
  std::size_t index = 0;
  BlockKind kind = BlockKind::kOutside;
  std::vector<char> compressed;
  std::uint64_t raw_bytes = 0;
  std::int64_t surface_blocks = 0;
  std::int64_t band_voxels = 0;
  double material_voxels = 0.0;
};

/// Material grey value: median of level-0 voxels above the dataset threshold in a sample of
/// bricks spread over the part. The voxels just below the surface dominate thin walls, so cupping
/// deep inside thick parts shifts it only a little.
float estimateMaterialLevel(const Dataset& dataset) {
  const auto& bricks = dataset.level(0).bricks;
  const float threshold = dataset.info().threshold;
  constexpr std::size_t kSampleBricks = 16;
  constexpr std::size_t kMaxValues = std::size_t{1} << 20U;
  const std::size_t step = std::max<std::size_t>(1, bricks.size() / kSampleBricks);
  std::vector<float> values;
  for (std::size_t i = 0; i < bricks.size(); i += step) {
    const auto brick = dataset.brick(0, bricks[i]);
    if (!brick) {
      continue;
    }
    for (auto it = brick->cbeginValueOn(); it; ++it) {
      if (*it > threshold) {
        values.push_back(*it);
      }
    }
    if (values.size() > kMaxValues) {
      break;
    }
  }
  if (values.empty()) {
    return threshold;
  }
  auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
  std::nth_element(values.begin(), middle, values.end());
  return *middle;
}

class SurfaceWriter {
 public:
  SurfaceWriter(const Dataset& dataset, const SurfaceInfo& info, const SurfaceOptions& options)
      : dataset_(dataset), info_(info), level_(options.compression_level) {
    // The triangulation must reach one band width plus a cell beyond the chunk.
    halo_ = static_cast<std::int64_t>(std::ceil(info_.band_voxels)) + 3;
  }

  [[nodiscard]] ChunkOutput process(std::size_t index, const Index3& chunk) const {
    ChunkOutput out;
    out.index = index;
    const ChunkGeometry geometry = chunkGeometry(info_.dims, info_.chunk_size, chunk);
    if (!dataset_.hasBrick(0, chunk)) {
      return out;  // removed air, farther from the part than the margin
    }
    Box region;
    for (std::size_t i = 0; i < 3; ++i) {
      region.min[i] = std::max<std::int64_t>(0, geometry.box.min[i] - halo_);
      region.max[i] = std::min(info_.dims[i], geometry.box.max[i] + halo_);
    }
    const Index3 size{region.size(0), region.size(1), region.size(2)};
    std::vector<float> grey(static_cast<std::size_t>(region.voxelCount()));
    dataset_.readRegion(0, region, grey, info_.air_level);
    const float iso = info_.iso_value;
    const auto [lowest, highest] = std::minmax_element(grey.begin(), grey.end());
    if (*lowest > iso || *highest <= iso) {
      // No surface within the halo, so every voxel of the chunk is farther than the band.
      out.kind = *lowest > iso ? BlockKind::kInside : BlockKind::kOutside;
      if (out.kind == BlockKind::kInside) {
        out.material_voxels = static_cast<double>(geometry.box.voxelCount());
      }
      return out;
    }

    // Distances to the triangulated iso-surface. Material is inside, so mesh -grey at -iso.
    openvdb::FloatGrid::Ptr distance;
    {
      std::vector<float> negated(grey.size());
      std::transform(grey.begin(), grey.end(), negated.begin(), [](float g) { return -g; });
      const openvdb::CoordBBox bbox(
          openvdb::Coord(static_cast<int>(region.min[0]), static_cast<int>(region.min[1]),
                         static_cast<int>(region.min[2])),
          openvdb::Coord(static_cast<int>(region.max[0] - 1), static_cast<int>(region.max[1] - 1),
                         static_cast<int>(region.max[2] - 1)));
      const openvdb::tools::Dense<float, openvdb::tools::LayoutXYZ> dense(bbox, negated.data());
      openvdb::FloatGrid field(-info_.air_level);
      openvdb::tools::copyFromDense(dense, field, -1.0F);  // negative tolerance: all active
      const auto width = static_cast<float>(info_.band_voxels + 1.5);
      distance = openvdb::tools::levelSetRebuild(field, -iso, width, width);
    }

    const std::int64_t block_count = geometry.blockCount();
    std::vector<BlockKind> kinds(static_cast<std::size_t>(block_count));
    std::vector<std::uint8_t> codes(static_cast<std::size_t>(block_count * kBlockVoxels));
    const auto max_code = static_cast<std::uint8_t>(info_.maxCode());
    struct Counts {
      std::int64_t band = 0;
      double material = 0.0;
    };
    tbb::combinable<Counts> counts;
    tbb::parallel_for(tbb::blocked_range<std::int64_t>(0, block_count), [&](const auto& range) {
      auto accessor = distance->getConstAccessor();
      Counts& local = counts.local();
      for (std::int64_t b = range.begin(); b < range.end(); ++b) {
        const Index3 block{b % geometry.blocks[0], (b / geometry.blocks[0]) % geometry.blocks[1],
                           b / (geometry.blocks[0] * geometry.blocks[1])};
        std::uint8_t* block_codes = codes.data() + b * kBlockVoxels;
        bool all_inside = true;
        bool all_outside = true;
        for (std::int64_t v = 0; v < kBlockVoxels; ++v) {
          const Index3 voxel{geometry.box.min[0] + block[0] * kBlock + v % kBlock,
                             geometry.box.min[1] + block[1] * kBlock + (v / kBlock) % kBlock,
                             geometry.box.min[2] + block[2] * kBlock + v / (kBlock * kBlock)};
          std::uint8_t code = max_code;
          if (voxel[0] < info_.dims[0] && voxel[1] < info_.dims[1] && voxel[2] < info_.dims[2]) {
            const auto offset = static_cast<std::size_t>(
                (voxel[0] - region.min[0]) +
                size[0] * ((voxel[1] - region.min[1]) + size[1] * (voxel[2] - region.min[2])));
            const bool inside = grey[offset] > iso;
            const float d = accessor.getValue(openvdb::Coord(static_cast<int>(voxel[0]),
                                                             static_cast<int>(voxel[1]),
                                                             static_cast<int>(voxel[2])));
            code = encodeSurfaceDistance(std::abs(d), inside, info_.bits, info_.band_voxels);
            local.material +=
                fractionOf(decodeSurfaceDistance(code, info_.bits, info_.band_voxels));
            if (code != 0 && code != max_code) {
              ++local.band;
            }
          }
          block_codes[v] = code;
          all_inside = all_inside && code == 0;
          all_outside = all_outside && code == max_code;
        }
        kinds[static_cast<std::size_t>(b)] = all_inside    ? BlockKind::kInside
                                             : all_outside ? BlockKind::kOutside
                                                           : BlockKind::kSurface;
      }
    });
    counts.combine_each([&](const Counts& c) {
      out.band_voxels += c.band;
      out.material_voxels += c.material;
    });

    std::vector<std::uint8_t> surface_codes;
    for (std::int64_t b = 0; b < block_count; ++b) {
      if (kinds[static_cast<std::size_t>(b)] == BlockKind::kSurface) {
        ++out.surface_blocks;
        const auto first = codes.begin() + b * kBlockVoxels;
        surface_codes.insert(surface_codes.end(), first, first + kBlockVoxels);
      }
    }
    if (out.surface_blocks == 0) {
      const bool inside = std::all_of(kinds.begin(), kinds.end(),
                                      [](BlockKind k) { return k == BlockKind::kInside; });
      const bool outside = std::all_of(kinds.begin(), kinds.end(),
                                       [](BlockKind k) { return k == BlockKind::kOutside; });
      if (inside || outside) {
        out.kind = inside ? BlockKind::kInside : BlockKind::kOutside;
        return out;
      }
    }
    out.kind = BlockKind::kSurface;
    const std::vector<char> payload = packChunk(kinds, surface_codes, info_.bits);
    out.raw_bytes = payload.size();
    out.compressed = compress(payload, level_);
    return out;
  }

 private:
  const Dataset& dataset_;
  SurfaceInfo info_;
  int level_ = 0;
  std::int64_t halo_ = 0;
};

Json headerJson(const SurfaceInfo& info) {
  Json json = toJson(info);
  json.erase("file_bytes");  // known from the file itself
  json["format"] = "voxelsieve-surface";
  json["version"] = 1;
  json["sign"] = "negative in material";
  json["codes"] = {{"inside", 0},
                   {"outside", info.maxCode()},
                   {"distance_voxels", "-band + (code - 0.5) * step for 0 < code < outside"}};
  return json;
}

SurfaceInfo infoFromJson(const Json& json) {
  SurfaceInfo info;
  info.dims = json.at("dims").get<Index3>();
  info.voxel_size_mm = json.at("voxel_size_mm").get<double>();
  info.bits = json.at("bits").get<int>();
  info.band_voxels = json.at("band_voxels").get<double>();
  info.block_size = json.at("block_size").get<std::int64_t>();
  info.chunk_size = json.at("chunk_size").get<std::int64_t>();
  info.iso_value = json.at("iso_value").get<float>();
  info.air_level = json.at("air_level").get<float>();
  info.material_level = json.at("material_level").get<float>();
  if (json.at("compression").get<std::string>() != kCompression) {
    throw std::runtime_error("Unsupported surface compression");
  }
  info.chunks = json.at("chunks").get<std::int64_t>();
  info.surface_chunks = json.at("surface_chunks").get<std::int64_t>();
  info.surface_blocks = json.at("surface_blocks").get<std::int64_t>();
  info.band_voxel_count = json.at("band_voxel_count").get<std::int64_t>();
  info.volume_mm3 = json.at("volume_mm3").get<double>();
  info.raw_bytes = json.at("raw_bytes").get<std::uint64_t>();
  if (info.bits < 2 || info.bits > 8 || info.block_size != kBlock || info.chunk_size <= 0 ||
      info.chunk_size % kBlock != 0) {
    throw std::runtime_error("Unsupported surface file layout");
  }
  return info;
}

struct FileSections {
  SurfaceInfo info;
  std::uint64_t table_offset = 0;
  std::uint64_t table_bytes = 0;
};

FileSections readSections(const char* data, std::size_t size) {
  if (size < kMagic.size() + kTrailerBytes || std::string_view(data, kMagic.size()) != kMagic ||
      std::string_view(data + size - kEndMagic.size(), kEndMagic.size()) != kEndMagic) {
    throw std::runtime_error("Not a VoxelSieve surface file");
  }
  const char* trailer = data + size - kTrailerBytes;
  FileSections sections;
  sections.table_offset = readU64(trailer);
  sections.table_bytes = readU64(trailer + 8);
  const std::uint64_t header_offset = readU64(trailer + 16);
  const std::uint64_t header_bytes = readU64(trailer + 24);
  if (header_offset + header_bytes > size || sections.table_offset + sections.table_bytes > size) {
    throw std::runtime_error("Corrupt surface file trailer");
  }
  sections.info = infoFromJson(
      Json::parse(std::string_view(data + header_offset, static_cast<std::size_t>(header_bytes))));
  sections.info.file_bytes = size;
  return sections;
}

}  // namespace

// ---------------------------------------------------------------------------------------------

Json toJson(const SurfaceInfo& info) {
  return {{"dims", info.dims},
          {"voxel_size_mm", info.voxel_size_mm},
          {"bits", info.bits},
          {"band_voxels", info.band_voxels},
          {"step_voxels", info.stepVoxels()},
          {"block_size", info.block_size},
          {"chunk_size", info.chunk_size},
          {"iso_value", info.iso_value},
          {"air_level", info.air_level},
          {"material_level", info.material_level},
          {"compression", kCompression},
          {"chunks", info.chunks},
          {"surface_chunks", info.surface_chunks},
          {"surface_blocks", info.surface_blocks},
          {"band_voxel_count", info.band_voxel_count},
          {"volume_mm3", info.volume_mm3},
          {"raw_bytes", info.raw_bytes},
          {"file_bytes", info.file_bytes}};
}

std::uint8_t encodeSurfaceDistance(double distance_voxels, bool inside, int bits,
                                   double band_voxels) {
  const int max_code = (1 << bits) - 1;
  const int half = (max_code - 1) / 2;
  const double magnitude = std::abs(distance_voxels);
  if (magnitude >= band_voxels) {
    return static_cast<std::uint8_t>(inside ? 0 : max_code);
  }
  const double step = 2.0 * band_voxels / (max_code - 1);
  const double signed_distance = inside ? -magnitude : magnitude;
  const auto code = 1 + static_cast<int>(std::floor((signed_distance + band_voxels) / step));
  return static_cast<std::uint8_t>(inside ? std::clamp(code, 1, half)
                                          : std::clamp(code, half + 1, max_code - 1));
}

float decodeSurfaceDistance(std::uint8_t code, int bits, double band_voxels) {
  const int max_code = (1 << bits) - 1;
  if (code == 0) {
    return static_cast<float>(-band_voxels);
  }
  if (code >= max_code) {
    return static_cast<float>(band_voxels);
  }
  const double step = 2.0 * band_voxels / (max_code - 1);
  return static_cast<float>(-band_voxels + (code - 0.5) * step);
}

SurfaceInfo writeSurface(const Dataset& dataset, const std::filesystem::path& file,
                         const SurfaceOptions& options) {
  const DatasetInfo& data = dataset.info();
  if (options.bits < 2 || options.bits > 8) {
    throw std::invalid_argument("bits must be between 2 and 8");
  }
  if (options.band_voxels <= 0.0 || options.band_voxels > data.margin_voxels) {
    throw std::invalid_argument("band must be > 0 and at most the dataset margin of " +
                                std::to_string(data.margin_voxels) + " voxels");
  }
  if (options.compression_level < 0 || options.compression_level > 22) {
    throw std::invalid_argument("compression level must be between 0 (default) and 22");
  }
  SurfaceInfo info;
  info.dims = data.dims;
  info.voxel_size_mm = data.voxel_size_mm;
  info.bits = options.bits;
  info.band_voxels = options.band_voxels;
  info.chunk_size = data.brick_size;
  info.air_level = data.air_level;
  info.material_level = estimateMaterialLevel(dataset);
  info.iso_value = options.iso_value.value_or(0.5F * (info.air_level + info.material_level));
  SurfaceOptions resolved = options;
  if (resolved.compression_level <= 0) {
    resolved.compression_level = kDefaultLevel;
  }
  const Index3 grid = chunkGrid(info);
  const auto chunk_count = static_cast<std::size_t>(grid[0] * grid[1] * grid[2]);
  info.chunks = static_cast<std::int64_t>(chunk_count);

  const std::filesystem::path temporary = file.string() + ".partial";
  std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
  if (!out) {
    throw std::runtime_error("Cannot write " + temporary.string());
  }
  out.write(kMagic.data(), static_cast<std::streamsize>(kMagic.size()));
  std::uint64_t position = kMagic.size();
  std::vector<char> table;
  table.reserve(chunk_count * kTableEntryBytes);
  double material_voxels = 0.0;

  const SurfaceWriter writer(dataset, info, resolved);
  std::size_t next = 0;
  tbb::parallel_pipeline(
      kChunksInFlight,
      tbb::make_filter<void, std::size_t>(tbb::filter_mode::serial_in_order,
                                          [&](tbb::flow_control& control) -> std::size_t {
                                            if (next >= chunk_count) {
                                              control.stop();
                                              return 0;
                                            }
                                            return next++;
                                          }) &
          tbb::make_filter<std::size_t, ChunkOutput>(
              tbb::filter_mode::parallel,
              [&](std::size_t index) {
                const auto i = static_cast<std::int64_t>(index);
                const Index3 chunk{i % grid[0], (i / grid[0]) % grid[1], i / (grid[0] * grid[1])};
                return writer.process(index, chunk);
              }) &
          tbb::make_filter<ChunkOutput, void>(
              tbb::filter_mode::serial_in_order, [&](const ChunkOutput& chunk) {
                table.push_back(static_cast<char>(chunk.kind));
                appendU64(table, chunk.compressed.empty() ? 0 : position);
                appendU64(table, chunk.compressed.size());
                out.write(chunk.compressed.data(),
                          static_cast<std::streamsize>(chunk.compressed.size()));
                position += chunk.compressed.size();
                info.raw_bytes += chunk.raw_bytes;
                info.surface_chunks += chunk.surface_blocks > 0 ? 1 : 0;
                info.surface_blocks += chunk.surface_blocks;
                info.band_voxel_count += chunk.band_voxels;
                material_voxels += chunk.material_voxels;
              }));
  info.volume_mm3 = material_voxels * std::pow(info.voxel_size_mm, 3);

  const std::vector<char> packed_table = compress(table, resolved.compression_level);
  const std::uint64_t table_offset = position;
  out.write(packed_table.data(), static_cast<std::streamsize>(packed_table.size()));
  position += packed_table.size();
  info.raw_bytes += table.size();
  const std::string header = headerJson(info).dump(2);
  const std::uint64_t header_offset = position;
  out.write(header.data(), static_cast<std::streamsize>(header.size()));
  std::vector<char> trailer;
  appendU64(trailer, table_offset);
  appendU64(trailer, packed_table.size());
  appendU64(trailer, header_offset);
  appendU64(trailer, header.size());
  trailer.insert(trailer.end(), kEndMagic.begin(), kEndMagic.end());
  out.write(trailer.data(), static_cast<std::streamsize>(trailer.size()));
  out.close();
  if (!out) {
    throw std::runtime_error("Cannot write " + temporary.string());
  }
  std::filesystem::rename(temporary, file);
  info.file_bytes = std::filesystem::file_size(file);
  return info;
}

SurfaceInfo readSurfaceInfo(const std::filesystem::path& file) {
  const boost::iostreams::mapped_file_source mapped(file.string());
  return readSections(mapped.data(), mapped.size()).info;
}

// ---------------------------------------------------------------------------------------------
// Reader.

namespace {

struct DecodedChunk {
  ChunkGeometry geometry;
  BlockKind kind = BlockKind::kOutside;
  /// Per block: -1 for uniform blocks (kind in `kinds`), else the index of its codes.
  std::vector<BlockKind> kinds;
  std::vector<std::int32_t> slots;
  std::vector<std::uint8_t> codes;  // 512 per surface block, one byte per code

  [[nodiscard]] std::size_t bytes() const {
    return kinds.size() + slots.size() * sizeof(std::int32_t) + codes.size() + sizeof(*this);
  }
};

}  // namespace

struct SurfaceMask::Impl {
  boost::iostreams::mapped_file_source mapped;
  SurfaceInfo info;
  Index3 grid{};
  struct Entry {
    BlockKind kind = BlockKind::kOutside;
    std::uint64_t offset = 0;
    std::uint64_t bytes = 0;
  };
  std::vector<Entry> table;
  std::size_t cache_bytes = 0;

  mutable std::mutex mutex;
  mutable std::list<std::size_t> lru;  // most recent first
  struct Cached {
    std::shared_ptr<const DecodedChunk> chunk;
    std::list<std::size_t>::iterator position;
  };
  mutable std::unordered_map<std::size_t, Cached> cache;
  mutable std::size_t used_bytes = 0;

  [[nodiscard]] std::size_t chunkIndex(const Index3& chunk) const {
    return static_cast<std::size_t>(chunk[0] + grid[0] * (chunk[1] + grid[1] * chunk[2]));
  }

  [[nodiscard]] std::shared_ptr<const DecodedChunk> decode(std::size_t index) const {
    const auto i = static_cast<std::int64_t>(index);
    const Index3 chunk{i % grid[0], (i / grid[0]) % grid[1], i / (grid[0] * grid[1])};
    auto decoded = std::make_shared<DecodedChunk>();
    decoded->geometry = chunkGeometry(info.dims, info.chunk_size, chunk);
    const Entry& entry = table[index];
    decoded->kind = entry.kind;
    if (entry.kind != BlockKind::kSurface) {
      return decoded;
    }
    const auto block_count = static_cast<std::size_t>(decoded->geometry.blockCount());
    const std::vector<char> payload =
        decompress(mapped.data() + entry.offset, static_cast<std::size_t>(entry.bytes),
                   block_count * kBlockVoxels);
    const std::size_t kind_bytes = (block_count + 3) / 4;
    if (payload.size() < kind_bytes) {
      throw std::runtime_error("Corrupt surface chunk");
    }
    decoded->kinds.resize(block_count);
    decoded->slots.assign(block_count, -1);
    std::int32_t surface = 0;
    for (std::size_t b = 0; b < block_count; ++b) {
      const auto kind = static_cast<BlockKind>(
          (static_cast<std::uint8_t>(payload[b / 4]) >> (2U * (b % 4))) & 3U);
      decoded->kinds[b] = kind;
      if (kind == BlockKind::kSurface) {
        decoded->slots[b] = surface++;
      }
    }
    const auto width = static_cast<unsigned>(info.bits);
    const std::size_t code_count = static_cast<std::size_t>(surface) * kBlockVoxels;
    if (payload.size() != kind_bytes + code_count * width / 8) {
      throw std::runtime_error("Corrupt surface chunk");
    }
    decoded->codes.resize(code_count);
    std::uint32_t buffer = 0;
    unsigned available = 0;
    std::size_t byte = kind_bytes;
    const std::uint32_t mask = (1U << width) - 1U;
    for (std::size_t c = 0; c < code_count; ++c) {
      while (available < width) {
        buffer |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(payload[byte++]))
                  << available;
        available += 8;
      }
      decoded->codes[c] = static_cast<std::uint8_t>(buffer & mask);
      buffer >>= width;
      available -= width;
    }
    return decoded;
  }

  [[nodiscard]] std::shared_ptr<const DecodedChunk> chunk(std::size_t index) const {
    {
      const std::lock_guard lock(mutex);
      if (auto it = cache.find(index); it != cache.end()) {
        lru.splice(lru.begin(), lru, it->second.position);
        return it->second.chunk;
      }
    }
    auto decoded = decode(index);
    const std::lock_guard lock(mutex);
    if (auto it = cache.find(index); it != cache.end()) {
      return it->second.chunk;  // decoded by another thread meanwhile
    }
    lru.push_front(index);
    cache.emplace(index, Cached{decoded, lru.begin()});
    used_bytes += decoded->bytes();
    while (used_bytes > cache_bytes && lru.size() > 1) {
      const std::size_t oldest = lru.back();
      lru.pop_back();
      used_bytes -= cache.at(oldest).chunk->bytes();
      cache.erase(oldest);
    }
    return decoded;
  }

  [[nodiscard]] std::uint8_t codeIn(const DecodedChunk& chunk, const Index3& voxel) const {
    const auto max_code = static_cast<std::uint8_t>(info.maxCode());
    if (chunk.kind != BlockKind::kSurface) {
      return chunk.kind == BlockKind::kInside ? 0 : max_code;
    }
    Index3 local{};
    Index3 block{};
    for (std::size_t i = 0; i < 3; ++i) {
      local[i] = voxel[i] - chunk.geometry.box.min[i];
      block[i] = local[i] / kBlock;
    }
    const auto b = static_cast<std::size_t>(
        block[0] + chunk.geometry.blocks[0] * (block[1] + chunk.geometry.blocks[1] * block[2]));
    const std::int32_t slot = chunk.slots[b];
    if (slot < 0) {
      return chunk.kinds[b] == BlockKind::kInside ? 0 : max_code;
    }
    const std::int64_t v =
        local[0] % kBlock + kBlock * (local[1] % kBlock + kBlock * (local[2] % kBlock));
    return chunk.codes[static_cast<std::size_t>(slot) * kBlockVoxels + static_cast<std::size_t>(v)];
  }

  [[nodiscard]] bool contains(const Index3& voxel) const {
    for (std::size_t i = 0; i < 3; ++i) {
      if (voxel[i] < 0 || voxel[i] >= info.dims[i]) {
        return false;
      }
    }
    return true;
  }
};

SurfaceMask::SurfaceMask(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
SurfaceMask::SurfaceMask(SurfaceMask&&) noexcept = default;
SurfaceMask& SurfaceMask::operator=(SurfaceMask&&) noexcept = default;
SurfaceMask::~SurfaceMask() = default;

SurfaceMask SurfaceMask::open(const std::filesystem::path& file, std::size_t cache_bytes) {
  auto impl = std::make_unique<Impl>();
  impl->mapped.open(file.string());
  if (!impl->mapped.is_open()) {
    throw std::runtime_error("Cannot open " + file.string());
  }
  const FileSections sections = readSections(impl->mapped.data(), impl->mapped.size());
  impl->info = sections.info;
  impl->grid = chunkGrid(impl->info);
  impl->cache_bytes = cache_bytes;
  const auto chunk_count = static_cast<std::size_t>(impl->grid[0] * impl->grid[1] * impl->grid[2]);
  const std::vector<char> table =
      decompress(impl->mapped.data() + sections.table_offset,
                 static_cast<std::size_t>(sections.table_bytes), chunk_count * kTableEntryBytes);
  if (table.size() != chunk_count * kTableEntryBytes) {
    throw std::runtime_error("Corrupt surface chunk table");
  }
  impl->table.resize(chunk_count);
  for (std::size_t i = 0; i < chunk_count; ++i) {
    const char* entry = table.data() + i * kTableEntryBytes;
    auto& e = impl->table[i];
    const auto kind = static_cast<std::uint8_t>(entry[0]);
    if (kind > 2) {
      throw std::runtime_error("Corrupt surface chunk table");
    }
    e.kind = static_cast<BlockKind>(kind);
    e.offset = readU64(entry + 1);
    e.bytes = readU64(entry + 9);
    if (e.offset + e.bytes > impl->mapped.size()) {
      throw std::runtime_error("Corrupt surface chunk table");
    }
  }
  return SurfaceMask(std::move(impl));
}

const SurfaceInfo& SurfaceMask::info() const { return impl_->info; }

std::uint8_t SurfaceMask::code(const std::array<std::int64_t, 3>& voxel) const {
  if (!impl_->contains(voxel)) {
    return static_cast<std::uint8_t>(impl_->info.maxCode());
  }
  const Index3 chunk{voxel[0] / impl_->info.chunk_size, voxel[1] / impl_->info.chunk_size,
                     voxel[2] / impl_->info.chunk_size};
  return impl_->codeIn(*impl_->chunk(impl_->chunkIndex(chunk)), voxel);
}

float SurfaceMask::distance(const std::array<std::int64_t, 3>& voxel) const {
  return decodeSurfaceDistance(code(voxel), impl_->info.bits, impl_->info.band_voxels);
}

void SurfaceMask::readCodes(const Box& box, std::span<std::uint8_t> out) const {
  if (out.size() != static_cast<std::size_t>(box.voxelCount())) {
    throw std::invalid_argument("Output size does not match the region");
  }
  const SurfaceInfo& info = impl_->info;
  const auto max_code = static_cast<std::uint8_t>(info.maxCode());
  std::fill(out.begin(), out.end(), max_code);
  Index3 first{};
  Index3 last{};
  for (std::size_t i = 0; i < 3; ++i) {
    const std::int64_t lo = std::max<std::int64_t>(box.min[i], 0);
    const std::int64_t hi = std::min(box.max[i], info.dims[i]);
    if (lo >= hi) {
      return;
    }
    first[i] = lo / info.chunk_size;
    last[i] = (hi - 1) / info.chunk_size;
  }
  for (std::int64_t cz = first[2]; cz <= last[2]; ++cz) {
    for (std::int64_t cy = first[1]; cy <= last[1]; ++cy) {
      for (std::int64_t cx = first[0]; cx <= last[0]; ++cx) {
        const auto chunk = impl_->chunk(impl_->chunkIndex({cx, cy, cz}));
        Box part;
        for (std::size_t i = 0; i < 3; ++i) {
          part.min[i] = std::max(box.min[i], chunk->geometry.box.min[i]);
          part.max[i] = std::min(box.max[i], chunk->geometry.box.max[i]);
        }
        for (std::int64_t z = part.min[2]; z < part.max[2]; ++z) {
          for (std::int64_t y = part.min[1]; y < part.max[1]; ++y) {
            for (std::int64_t x = part.min[0]; x < part.max[0]; ++x) {
              const auto offset = static_cast<std::size_t>(
                  (x - box.min[0]) +
                  box.size(0) * ((y - box.min[1]) + box.size(1) * (z - box.min[2])));
              out[offset] = impl_->codeIn(*chunk, {x, y, z});
            }
          }
        }
      }
    }
  }
}

openvdb::FloatGrid::Ptr SurfaceMask::toLevelSet() const {
  const SurfaceInfo& info = impl_->info;
  const double v = info.voxel_size_mm;
  auto grid = openvdb::FloatGrid::create(static_cast<float>(info.band_voxels * v));
  grid->setGridClass(openvdb::GRID_LEVEL_SET);
  grid->setName("surface");
  grid->setTransform(openvdb::math::Transform::createLinearTransform(v));
  auto accessor = grid->getAccessor();
  for (std::size_t index = 0; index < impl_->table.size(); ++index) {
    if (impl_->table[index].kind != BlockKind::kSurface) {
      continue;
    }
    const auto chunk = impl_->chunk(index);
    const ChunkGeometry& g = chunk->geometry;
    for (std::size_t b = 0; b < chunk->slots.size(); ++b) {
      if (chunk->slots[b] < 0) {
        continue;
      }
      const auto block = static_cast<std::int64_t>(b);
      const Index3 origin{g.box.min[0] + (block % g.blocks[0]) * kBlock,
                          g.box.min[1] + ((block / g.blocks[0]) % g.blocks[1]) * kBlock,
                          g.box.min[2] + (block / (g.blocks[0] * g.blocks[1])) * kBlock};
      const std::uint8_t* codes =
          chunk->codes.data() + static_cast<std::size_t>(chunk->slots[b]) * kBlockVoxels;
      for (std::int64_t i = 0; i < kBlockVoxels; ++i) {
        const Index3 voxel{origin[0] + i % kBlock, origin[1] + (i / kBlock) % kBlock,
                           origin[2] + i / (kBlock * kBlock)};
        if (!impl_->contains(voxel)) {
          continue;
        }
        const float d = decodeSurfaceDistance(codes[i], info.bits, info.band_voxels);
        accessor.setValue(openvdb::Coord(static_cast<int>(voxel[0]), static_cast<int>(voxel[1]),
                                         static_cast<int>(voxel[2])),
                          static_cast<float>(d * v));
      }
    }
  }
  openvdb::tools::signedFloodFill(grid->tree());
  return grid;
}

Mesh SurfaceMask::toMesh(double adaptivity) const {
  const auto grid = toLevelSet();
  std::vector<openvdb::Vec3s> points;
  std::vector<openvdb::Vec3I> triangles;
  std::vector<openvdb::Vec4I> quads;
  openvdb::tools::volumeToMesh(*grid, points, triangles, quads, 0.0, adaptivity);
  Mesh mesh;
  mesh.triangles.reserve(triangles.size() + 2 * quads.size());
  const auto vertex = [&](std::uint32_t index) {
    return std::array<float, 3>{points[index].x(), points[index].y(), points[index].z()};
  };
  for (const openvdb::Vec3I& t : triangles) {
    mesh.triangles.push_back({vertex(t[0]), vertex(t[1]), vertex(t[2])});
  }
  for (const openvdb::Vec4I& q : quads) {
    mesh.triangles.push_back({vertex(q[0]), vertex(q[1]), vertex(q[2])});
    mesh.triangles.push_back({vertex(q[0]), vertex(q[2]), vertex(q[3])});
  }
  if (meshVolumeMm3(mesh) < 0.0) {
    for (auto& triangle : mesh.triangles) {
      std::swap(triangle[1], triangle[2]);
    }
  }
  return mesh;
}

void writeSurfaceImages(const SurfaceMask& mask, const std::filesystem::path& dir,
                        std::int64_t max_pixels) {
  const SurfaceInfo& info = mask.info();
  std::filesystem::create_directories(dir);
  const int max_code = info.maxCode();
  for (std::size_t axis = 0; axis < 3; ++axis) {
    // Image axes: the two other volume axes, u to the right and v downwards.
    const std::size_t u = axis == 0 ? 1 : 0;
    const std::size_t v = axis == 2 ? 1 : 2;
    const std::int64_t extent = std::max(info.dims[u], info.dims[v]);
    const std::int64_t step = std::max<std::int64_t>(1, (extent + max_pixels - 1) / max_pixels);
    const std::int64_t width = (info.dims[u] + step - 1) / step;
    const std::int64_t height = (info.dims[v] + step - 1) / step;
    Box slice;
    slice.min[axis] = info.dims[axis] / 2;
    slice.max[axis] = slice.min[axis] + 1;
    slice.max[u] = info.dims[u];
    slice.max[v] = info.dims[v];
    std::vector<std::uint8_t> codes(static_cast<std::size_t>(slice.voxelCount()));
    mask.readCodes(slice, codes);
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(width * height * 3));
    for (std::int64_t y = 0; y < height; ++y) {
      for (std::int64_t x = 0; x < width; ++x) {
        std::array<std::int64_t, 3> index{};
        index[u] = x * step;
        index[v] = (height - 1 - y) * step;  // v up, as in the slice view
        index[v] = std::min(index[v], info.dims[v] - 1);
        const auto offset = static_cast<std::size_t>(index[0] + slice.size(0) * index[1] +
                                                     slice.size(0) * slice.size(1) * index[2]);
        const int code = codes[offset];
        std::array<std::uint8_t, 3> color{0, 0, 0};
        if (code == 0) {
          color = {150, 150, 150};
        } else if (code < max_code) {
          const double t = static_cast<double>(code - 1) / std::max(1, max_code - 2);
          color = {static_cast<std::uint8_t>(40 + 215 * t), 60,
                   static_cast<std::uint8_t>(255 - 215 * t)};
        }
        std::copy(color.begin(), color.end(),
                  rgb.begin() + static_cast<std::ptrdiff_t>((y * width + x) * 3));
      }
    }
    const std::string name = std::string("surface_") + "xyz"[axis] + ".png";
    detail::writeRgbPng(dir / name, static_cast<std::uint32_t>(width),
                        static_cast<std::uint32_t>(height), rgb);
  }
}

}  // namespace voxelsieve
