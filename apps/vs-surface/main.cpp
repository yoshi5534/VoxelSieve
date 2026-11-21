// vs-surface: locates the surface of the part in a sieved dataset and stores it as a voxel mask
// with a few bits per voxel that encode the distance to the surface (docs/adr/0009).

#include <openvdb/io/File.h>

#include <chrono>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#include "voxelsieve/cli.hpp"
#include "voxelsieve/dataset.hpp"
#include "voxelsieve/io.hpp"
#include "voxelsieve/mesh.hpp"
#include "voxelsieve/surface.hpp"

namespace {

constexpr std::string_view kUsage = R"(Usage: vs-surface <dataset> --out <file.vss> [options]
       vs-surface --info <file.vss> [--stl <file>] [--vdb <file>]

Writes the surface of the part in a dataset written by vs-sieve as a distance mask: every voxel
gets a few bits that encode its signed distance to the surface (negative in material). Only 8^3
blocks near the surface are stored; the rest costs nearly nothing.

Options:
  --out <file>            Surface file to write (required unless --info)
  --bits <n>              Bits per voxel, 2 to 8 (default 4)
  --band <voxels>         Half width of the distance band (default 1)
  --iso <value>           Grey value of the surface (default half way between air and material)
  --level <n>             zstd level 1 to 22 (default 19)
  --info <file>           Print the header of an existing surface file
  --stl <file>            Also write the surface as a triangle mesh (mm, dataset coordinates)
  --vdb <file>            Also write it as a narrow-band level set "surface" for Blender/Houdini
  --json <file>           Also write the header as JSON
)";

struct Options {
  std::filesystem::path dataset;
  std::filesystem::path out;
  std::filesystem::path info;
  std::filesystem::path stl;
  std::filesystem::path vdb;
  std::filesystem::path json;
  voxelsieve::SurfaceOptions surface;
};

Options parse(voxelsieve::cli::Arguments& args) {
  Options options;
  while (args.next()) {
    if (args.is("--out")) {
      options.out = args.value();
    } else if (args.is("--info")) {
      options.info = args.value();
    } else if (args.is("--stl")) {
      options.stl = args.value();
    } else if (args.is("--vdb")) {
      options.vdb = args.value();
    } else if (args.is("--json")) {
      options.json = args.value();
    } else if (args.is("--bits")) {
      options.surface.bits = static_cast<int>(args.integer());
    } else if (args.is("--band")) {
      options.surface.band_voxels = args.number();
    } else if (args.is("--iso")) {
      options.surface.iso_value = static_cast<float>(args.number());
    } else if (args.is("--level")) {
      options.surface.compression_level = static_cast<int>(args.integer());
    } else if (args.positional() && options.dataset.empty()) {
      options.dataset = args.arg();
    } else {
      args.unknown();
    }
  }
  if (options.info.empty() == (options.dataset.empty() || options.out.empty())) {
    throw voxelsieve::cli::UsageError("Either a dataset and --out, or --info, are required");
  }
  return options;
}

void printInfo(const voxelsieve::SurfaceInfo& info) {
  const auto voxels = static_cast<double>(info.dims[0] * info.dims[1] * info.dims[2]);
  const auto bits = 8.0 * static_cast<double>(info.file_bytes);
  std::cout << std::fixed << std::setprecision(3) << "dims               " << info.dims[0] << " x "
            << info.dims[1] << " x " << info.dims[2] << ", "
            << voxelsieve::describe(info.voxel_size) << "\n"
            << "code               " << info.bits << " bit, band +-" << info.band_voxels
            << " voxels, step " << info.stepVoxels() << " voxels\n"
            << std::setprecision(1) << "surface            iso " << info.iso_value << " (air "
            << info.air_level << ", material " << info.material_level << ")\n"
            << "stored             " << info.surface_chunks << " of " << info.chunks << " chunks, "
            << info.surface_blocks << " blocks of 8^3, " << info.band_voxel_count
            << " band voxels\n"
            << std::setprecision(3) << "volume             " << info.volume_mm3 << " mm^3\n"
            << "file               " << info.file_bytes << " bytes, " << bits / voxels
            << " bit/voxel, "
            << (info.band_voxel_count > 0 ? bits / static_cast<double>(info.band_voxel_count) : 0.0)
            << " bit/band voxel, " << std::setprecision(0)
            << 2.0 * voxels / static_cast<double>(info.file_bytes) << ":1 against 16-bit raw\n";
}

int run(const Options& options) {
  const auto start = std::chrono::steady_clock::now();
  std::filesystem::path file = options.info;
  if (file.empty()) {
    const auto dataset = voxelsieve::Dataset::open(options.dataset);
    (void)voxelsieve::writeSurface(dataset, options.out, options.surface);
    file = options.out;
  }
  const auto written = std::chrono::steady_clock::now();
  const auto mask = voxelsieve::SurfaceMask::open(file);
  printInfo(mask.info());
  if (!options.json.empty()) {
    voxelsieve::writeJson(options.json, voxelsieve::toJson(mask.info()));
  }
  if (!options.stl.empty()) {
    mask.writeStl(options.stl);  // streamed chunk by chunk
    std::cout << "mesh               " << (std::filesystem::file_size(options.stl) - 84) / 50
              << " triangles, " << options.stl.string() << '\n';
  }
  if (!options.vdb.empty()) {
    openvdb::io::File out(options.vdb.string());
    out.write({mask.toLevelSet()});
    out.close();
    std::cout << "level set          " << options.vdb.string() << '\n';
  }
  if (options.info.empty()) {
    std::cout << std::setprecision(2) << "time               "
              << std::chrono::duration<double>(written - start).count() << " s\n";
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  return voxelsieve::cli::runTool("vs-surface", kUsage, argc, argv, parse, run);
}
