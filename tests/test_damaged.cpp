// Damaged files of the formats VoxelSieve reads with its own code: every reader must answer with
// a result or an exception (damaged_files.hpp). The readers of models, surface masks and gzipped
// XML projects are tested next to their formats.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "damaged_files.hpp"
#include "voxelsieve/dataset.hpp"
#include "voxelsieve/io.hpp"
#include "voxelsieve/mesh.hpp"
#include "voxelsieve/operation.hpp"
#include "voxelsieve/phantom.hpp"
#include "voxelsieve/project.hpp"
#include "voxelsieve/source.hpp"
#include "voxelsieve/volume.hpp"

namespace voxelsieve {
namespace {

class DamagedFilesTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("voxelsieve_damaged_" +
            std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  static PhantomSpec smallPhantom() {
    PhantomSpec spec = defaultPhantomSpec();
    spec.dims = {24, 20, 16};
    spec.voxel_size = VoxelSize(0.5);
    return spec;
  }

  std::filesystem::path dir_;
};

TEST_F(DamagedFilesTest, StlFilesAreRefused) {
  const Mesh box = boxMesh({4.0, 3.0, 2.0});
  writeStl(dir_ / "box.stl", box);
  readDamagedCopies(dir_ / "box.stl", 600, [](const std::filesystem::path& file) {
    const Mesh mesh = readStl(file);
    (void)meshBounds(mesh);
  });

  std::ofstream text(dir_ / "text.stl");
  text << "solid box\n";
  for (const auto& triangle : box.triangles) {
    text << " facet normal 0 0 0\n  outer loop\n";
    for (const auto& vertex : triangle) {
      text << "   vertex " << vertex[0] << ' ' << vertex[1] << ' ' << vertex[2] << '\n';
    }
    text << "  endloop\n endfacet\n";
  }
  text << "endsolid box\n";
  text.close();
  ASSERT_EQ(readStl(dir_ / "text.stl").triangles.size(), box.triangles.size());
  readDamagedCopies(dir_ / "text.stl", 600, [](const std::filesystem::path& file) {
    const Mesh mesh = readStl(file);
    (void)meshBounds(mesh);
  });
}

TEST_F(DamagedFilesTest, DatasetIndexesAreRefused) {
  DatasetOptions options;
  options.brick_size = 16;
  (void)writeDataset(PhantomSource(smallPhantom()), dir_ / "scan.vsieve", options);
  const auto index = dir_ / "scan.vsieve" / "index.json";
  std::filesystem::copy_file(index, dir_ / "index.json");
  readDamagedCopies(dir_ / "index.json", 400, [&](const std::filesystem::path& file) {
    std::filesystem::copy_file(file, index, std::filesystem::copy_options::overwrite_existing);
    (void)readDatasetInfo(dir_ / "scan.vsieve");
    const Dataset dataset = Dataset::open(dir_ / "scan.vsieve");
    (void)dataset.sample(0, {1, 1, 1});
  });
}

TEST_F(DamagedFilesTest, SidecarsOfRawVolumesAreRefused) {
  const PhantomSpec spec = smallPhantom();
  const Volume16 volume = generatePhantom(spec);
  {
    std::ofstream raw(dir_ / "scan.raw", std::ios::binary);
    raw.write(reinterpret_cast<const char*>(volume.data.data()),  // NOLINT: bytes of the voxels
              static_cast<std::streamsize>(volume.data.size() * sizeof(std::uint16_t)));
  }
  writeJson(dir_ / "intact.json", {{"dims", spec.dims},
                                   {"voxel_size_mm", 0.5},
                                   {"sample_type", "uint16"},
                                   {"header_bytes", 0}});
  OperationRegistry registry;
  registerBuiltinOperations(registry);
  auto project = Project::create(dir_ / "p", "Damaged");
  readDamagedCopies(dir_ / "intact.json", 150, [&](const std::filesystem::path& file) {
    std::filesystem::copy_file(file, dir_ / "scan.json",
                               std::filesystem::copy_options::overwrite_existing);
    (void)project.run(registry, "import_raw",
                      {{"path", (dir_ / "scan.raw").string()}, {"brick_size", 16}});
  });
}

TEST_F(DamagedFilesTest, ProjectFilesAreRefused) {
  OperationRegistry registry;
  registerBuiltinOperations(registry);
  DatasetOptions options;
  options.brick_size = 16;
  (void)writeDataset(PhantomSource(smallPhantom()), dir_ / "scan.vsieve", options);
  {
    auto project = Project::create(dir_ / "p", "Damaged");
    (void)project.run(registry, "open_dataset", {{"path", (dir_ / "scan.vsieve").string()}});
    (void)project.run(registry, "porosity");
  }
  const auto file = dir_ / "p" / "project.json";
  std::filesystem::copy_file(file, dir_ / "project.json");
  readDamagedCopies(dir_ / "project.json", 400, [&](const std::filesystem::path& damaged) {
    std::filesystem::copy_file(damaged, file, std::filesystem::copy_options::overwrite_existing);
    const Project project = Project::open(dir_ / "p");
    (void)project.results();
  });
}

}  // namespace
}  // namespace voxelsieve
