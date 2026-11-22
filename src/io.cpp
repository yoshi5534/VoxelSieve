#include "voxelsieve/io.hpp"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "voxelsieve/resources.hpp"

namespace voxelsieve {

static_assert(std::endian::native == std::endian::little,
              "Raw I/O assumes a little-endian host; add byte swapping for big-endian targets.");

void writeRaw(const std::filesystem::path& path, const Volume16& volume) {
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    throw std::runtime_error("Cannot open for writing: " + path.string());
  }
  out.write(reinterpret_cast<const char*>(volume.data.data()),
            static_cast<std::streamsize>(volume.data.size() * sizeof(std::uint16_t)));
  out.close();
  if (!out) {
    throwWriteError(path);
  }
}

void writeRaw(const std::filesystem::path& path, const VolumeSource& source) {
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    throw std::runtime_error("Cannot open for writing: " + path.string());
  }
  const auto dims = source.dims();
  const std::int64_t slice = dims[0] * dims[1];
  // About 64 MB per slab, at least one slice.
  const std::int64_t slab = std::max<std::int64_t>(1, (std::int64_t{32} << 20) / slice);
  std::vector<std::uint16_t> buffer;
  for (std::int64_t z0 = 0; z0 < dims[2]; z0 += slab) {
    const std::int64_t z1 = std::min(z0 + slab, dims[2]);
    buffer.resize(static_cast<std::size_t>(slice * (z1 - z0)));
    tbb::parallel_for(z0, z1, [&](std::int64_t z) {
      const auto offset = static_cast<std::size_t>(slice * (z - z0));
      source.readRegion({{0, 0, z}, {dims[0], dims[1], z + 1}},
                        std::span(buffer).subspan(offset, static_cast<std::size_t>(slice)));
    });
    out.write(reinterpret_cast<const char*>(buffer.data()),
              static_cast<std::streamsize>(buffer.size() * sizeof(std::uint16_t)));
    if (!out) {
      throwWriteError(path);  // a full disk: stop now rather than read the rest of the volume
    }
  }
  out.close();
  if (!out) {
    throwWriteError(path);
  }
}

Volume16 readRaw(const std::filesystem::path& path, const std::array<std::int64_t, 3>& dims,
                 const VoxelSize& voxel_size) {
  Volume16 volume(dims, voxel_size);
  const auto expected_bytes = volume.data.size() * sizeof(std::uint16_t);
  if (std::filesystem::file_size(path) != expected_bytes) {
    throw std::runtime_error("File size of " + path.string() + " does not match dimensions (" +
                             std::to_string(expected_bytes) + " bytes expected)");
  }
  std::ifstream in(path, std::ios::binary);
  in.read(reinterpret_cast<char*>(volume.data.data()),
          static_cast<std::streamsize>(expected_bytes));
  if (!in) {
    throw std::runtime_error("Read failed: " + path.string());
  }
  return volume;
}

nlohmann::json phantomToJson(const PhantomSpec& spec) {
  nlohmann::json pores = nlohmann::json::array();
  for (const Pore& pore : spec.pores) {
    pores.push_back({{"center_mm", pore.center_mm}, {"radius_mm", pore.radius_mm}});
  }
  return {
      {"format",
       {{"type", "raw"}, {"dtype", "uint16"}, {"endianness", "little"}, {"order", "xyz"}}},
      {"dims", spec.dims},
      {"voxel_size_mm", spec.voxel_size},
      {"phantom",
       {{"air_value", spec.air_value},
        {"material_value", spec.material_value},
        {"noise_sigma", spec.noise_sigma},
        {"seed", spec.seed},
        {"outer_size_mm", spec.outer_size_mm},
        {"wall_thickness_mm", spec.wall_thickness_mm},
        {"pores", pores},
        {"material_volume_mm3", phantomMaterialVolumeMm3(spec)}}},
  };
}

void requireFreeSpace(const std::filesystem::path& dir, std::uint64_t bytes,
                      std::string_view what) {
  std::error_code error;
  const auto space = std::filesystem::space(dir, error);
  if (error || space.available >= bytes) {
    return;  // unknown: the writers report a full disk
  }
  constexpr double kGb = 1024.0 * 1024.0 * 1024.0;
  const auto gb = [](double value) {
    std::ostringstream text;
    text.precision(value < 10.0 ? 2 : 3);
    text << value;
    return text.str();
  };
  throw std::runtime_error(std::string(what) + " needs about " +
                           gb(static_cast<double>(bytes) / kGb) + " GB in " + dir.string() +
                           ", but only " + gb(static_cast<double>(space.available) / kGb) +
                           " GB are free");
}

void throwWriteError(const std::filesystem::path& path) {
  // A full disk is the usual cause of a failed write; say so rather than leave a file that only
  // shows later as truncated.
  std::error_code error;
  const auto space =
      std::filesystem::space(path.parent_path().empty() ? "." : path.parent_path(), error);
  if (!error && space.available < (std::uintmax_t{1} << 20U)) {
    throw std::runtime_error("No space left on the device while writing " + path.string());
  }
  throw std::runtime_error("Cannot write " + path.string());
}

void writeFile(const std::filesystem::path& path, const std::function<void(std::ostream&)>& write) {
  // A name of its own for each write: two threads writing the same file (the GUI saves the
  // project while a step runs) must not write into, or move away, each other's partial file.
  static std::atomic<std::uint64_t> writes{0};
  const std::filesystem::path partial = path.string() + ".partial-" + std::to_string(processId()) +
                                        "-" + std::to_string(writes.fetch_add(1));
  const auto discard = [&partial] {
    std::error_code ignored;
    std::filesystem::remove(partial, ignored);
  };
  try {
    std::ofstream out(partial, std::ios::binary | std::ios::trunc);
    if (!out) {
      throwWriteError(path);
    }
    write(out);
    // Errors of the last buffered writes show only when the file is closed.
    out.close();
    if (!out) {
      throwWriteError(path);
    }
    // Windows cannot replace a file that another program holds open for a moment, such as a
    // virus scanner looking at a new file or a reader of the previous version: try again shortly.
    constexpr int kAttempts = 20;
    for (int attempt = 1;; ++attempt) {
      std::error_code error;
      std::filesystem::rename(partial, path, error);
      if (!error) {
        break;
      }
      if (attempt == kAttempts) {
        throw std::filesystem::filesystem_error("Cannot replace " + path.string(), partial, path,
                                                error);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
  } catch (...) {
    discard();
    throw;
  }
}

void writeFile(const std::filesystem::path& path, std::string_view bytes) {
  writeFile(path, [bytes](std::ostream& out) {
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  });
}

void writeJson(const std::filesystem::path& path, const nlohmann::json& json) {
  // Text from input files (names, descriptions, tags) need not be UTF-8: a byte that is not is
  // replaced rather than failing the write of a result that took hours.
  writeFile(path, json.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + '\n');
}

}  // namespace voxelsieve
