#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <string>

#include "voxelsieve/dataset.hpp"
#include "voxelsieve/io.hpp"
#include "voxelsieve/phantom.hpp"
#include "voxelsieve/porosity.hpp"
#include "voxelsieve/source.hpp"
#include "voxelsieve/studio.hpp"
#include "voxelsieve/surface.hpp"
#include "voxelsieve/voxel_size.hpp"

namespace voxelsieve {
namespace {

using Json = nlohmann::json;

/// The default phantom (8 mm box, 1.5 mm walls, three pores) sampled twice as coarsely along z,
/// as CT systems often do between slices.
PhantomSpec anisotropicPhantom() {
  PhantomSpec spec = defaultPhantomSpec();
  spec.dims = {128, 128, 64};
  spec.voxel_size = VoxelSize(0.1, 0.1, 0.2);
  return spec;
}

/// Voxel coordinates of a phantom position in mm (voxel centres at integers).
std::array<double, 3> phantomVoxel(const PhantomSpec& spec, const std::array<double, 3>& mm) {
  std::array<double, 3> voxel{};
  for (std::size_t a = 0; a < 3; ++a) {
    voxel[a] = mm[a] / spec.voxel_size[a] + static_cast<double>(spec.dims[a]) / 2.0 - 0.5;
  }
  return voxel;
}

class VoxelSizeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("voxelsieve_voxel_size_" +
            std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  std::filesystem::path dir_;
};

TEST(VoxelSize, JsonKeepsCubesAsNumbersAndAddsTheThickness) {
  Json cubic;
  writeVoxelSize(cubic, VoxelSize(0.1));
  EXPECT_TRUE(cubic.at("voxel_size_mm").is_number());
  EXPECT_FALSE(cubic.contains("slice_thickness_mm"));
  EXPECT_EQ(readVoxelSize(cubic), VoxelSize(0.1));

  VoxelSize size(0.33, 0.33, 0.6);
  size.slice_thickness_mm = 0.4;
  Json json;
  writeVoxelSize(json, size);
  EXPECT_EQ(json.at("voxel_size_mm"), Json::array({0.33, 0.33, 0.6}));
  EXPECT_EQ(json.at("slice_thickness_mm"), 0.4);
  EXPECT_EQ(readVoxelSize(json), size);
  EXPECT_NEAR(size.sliceGapMm(), 0.2, 1e-12);
  EXPECT_NEAR(size.volumeMm3(), 0.33 * 0.33 * 0.6, 1e-15);
  EXPECT_EQ(describe(size), "0.33 x 0.33 x 0.6 mm, slices 0.4 mm thick");

  EXPECT_EQ(parseVoxelSize("0.1"), VoxelSize(0.1));
  EXPECT_EQ(parseVoxelSize("0.33,0.33,0.6"), VoxelSize(0.33, 0.33, 0.6));
  EXPECT_THROW((void)parseVoxelSize("0.1,0.2"), std::invalid_argument);
  EXPECT_THROW((void)voxelSizeFromJson(Json::array({1, 2})), std::invalid_argument);
  EXPECT_THROW(VoxelSize(0.1, 0.0, 0.1).validate(), std::invalid_argument);
  VoxelSize too_thick(0.1, 0.1, 0.2);
  too_thick.slice_thickness_mm = 0.3;
  EXPECT_THROW(too_thick.validate(), std::invalid_argument);
}

TEST(VoxelSize, AnisotropicPhantomHasTheAnalyticVolume) {
  const PhantomSpec spec = anisotropicPhantom();
  const Volume16 volume = generatePhantom(spec);
  EXPECT_EQ(volume.voxel_size, spec.voxel_size);
  const auto contrast = static_cast<double>(spec.material_value - spec.air_value);
  double fraction_sum = 0.0;
  for (const std::uint16_t value : volume.data) {
    fraction_sum += (static_cast<double>(value) - spec.air_value) / contrast;
  }
  EXPECT_NEAR(fraction_sum * spec.voxel_size.volumeMm3(), phantomMaterialVolumeMm3(spec),
              0.01 * phantomMaterialVolumeMm3(spec));
}

TEST_F(VoxelSizeTest, DatasetKeepsThePitchPerAxis) {
  PhantomSpec spec = anisotropicPhantom();
  spec.voxel_size.slice_thickness_mm = 0.15;
  DatasetOptions options;
  options.brick_size = 32;
  const DatasetInfo written = writeDataset(PhantomSource(spec), dir_ / "d.vsieve", options);
  EXPECT_EQ(written.voxel_size, spec.voxel_size);
  EXPECT_EQ(written.levels[1].voxel_size.pitch_mm, (std::array<double, 3>{0.2, 0.2, 0.4}));

  const DatasetInfo read = readDatasetInfo(dir_ / "d.vsieve");
  EXPECT_EQ(read.voxel_size, spec.voxel_size);
  EXPECT_EQ(read.levels.back().voxel_size.pitch_mm, written.levels.back().voxel_size.pitch_mm);

  // Bricks and overview carry the scale per axis, so VDB tools (Blender, Houdini) show the part
  // in its true proportions: the outer wall is material at +-3.25 mm along every axis.
  const auto overview = readBrick(dir_ / "d.vsieve" / "overview.vdb", false);
  const int top = written.levels.back().level;
  const double scale = std::pow(2.0, top);
  EXPECT_NEAR(overview->voxelSize()[0], 0.1 * scale, 1e-12);
  EXPECT_NEAR(overview->voxelSize()[2], 0.2 * scale, 1e-12);
  const auto accessor = overview->getConstAccessor();
  const auto world = [&](const std::array<double, 3>& mm) {
    const auto voxel = phantomVoxel(spec, mm);
    return openvdb::Vec3d(voxel[0] * 0.1, voxel[1] * 0.1, voxel[2] * 0.2);
  };
  for (const std::array<double, 3>& wall :
       {std::array<double, 3>{0.0, 3.25, 0.0}, std::array<double, 3>{0.0, 0.0, 3.25},
        std::array<double, 3>{0.0, 0.0, -3.25}}) {
    const auto index = overview->transform().worldToIndexCellCentered(world(wall));
    EXPECT_GT(accessor.getValue(index), 10000.0F) << wall[0] << "," << wall[1] << "," << wall[2];
  }
}

TEST_F(VoxelSizeTest, PoresAndPartVolumeAreMeasuredInMillimetres) {
  const PhantomSpec spec = anisotropicPhantom();
  DatasetOptions options;
  options.brick_size = 64;
  (void)writeDataset(PhantomSource(spec), dir_ / "d.vsieve", options);
  const Dataset dataset = Dataset::open(dir_ / "d.vsieve");
  const PorosityResult result = analyzePorosity(dataset);
  EXPECT_EQ(result.voxel_size, spec.voxel_size);
  // The closed inner cavity (5 mm cube) is a void too, besides the three pores.
  ASSERT_EQ(result.pores.size(), spec.pores.size() + 1);
  EXPECT_NEAR(result.pores.front().volume_mm3, 125.0, 0.5);
  for (const Pore& truth : spec.pores) {
    const auto expected = phantomVoxel(spec, truth.center_mm);
    const auto found = std::min_element(
        result.pores.begin(), result.pores.end(), [&](const auto& a, const auto& b) {
          const auto d = [&](const DetectedPore& p) {
            double sum = 0.0;
            for (std::size_t k = 0; k < 3; ++k) {
              const double mm = (p.center_voxels[k] - expected[k]) * spec.voxel_size[k];
              sum += mm * mm;
            }
            return sum;
          };
          return d(a) < d(b);
        });
    const auto mm = spec.voxel_size.toMm(found->center_voxels);
    const auto truth_mm = spec.voxel_size.toMm(expected);
    for (std::size_t k = 0; k < 3; ++k) {
      EXPECT_NEAR(mm[k], truth_mm[k], 0.5 * spec.voxel_size[k]) << "pore r=" << truth.radius_mm;
    }
    // The smallest pore is only one voxel in radius along z.
    const double volume = 4.0 / 3.0 * std::numbers::pi * std::pow(truth.radius_mm, 3);
    const double tolerance = truth.radius_mm < 0.25 ? 0.3 : 0.15;
    EXPECT_NEAR(found->volume_mm3, volume, tolerance * volume) << "pore r=" << truth.radius_mm;
  }
  // The part volume includes its voids: the whole 8 mm box.
  EXPECT_NEAR(result.part_volume_mm3, 512.0, 0.01 * 512.0);
  // The JSON gives positions in mm per axis.
  const Json json = toJson(result);
  EXPECT_EQ(json.at("voxel_size_mm"), Json::array({0.1, 0.1, 0.2}));
}

TEST_F(VoxelSizeTest, SurfaceMeshHasTheTrueShape) {
  const PhantomSpec spec = anisotropicPhantom();
  DatasetOptions options;
  options.brick_size = 64;
  (void)writeDataset(PhantomSource(spec), dir_ / "d.vsieve", options);
  const Dataset dataset = Dataset::open(dir_ / "d.vsieve");
  const SurfaceInfo info = writeSurface(dataset, dir_ / "s.vss");
  EXPECT_EQ(info.voxel_size, spec.voxel_size);
  EXPECT_NEAR(info.volume_mm3, phantomMaterialVolumeMm3(spec),
              0.02 * phantomMaterialVolumeMm3(spec));
  const SurfaceMask mask = SurfaceMask::open(dir_ / "s.vss");
  EXPECT_EQ(mask.info().voxel_size, spec.voxel_size);

  // The outer box is 8 mm along every axis, although it spans 80 voxels in x and y but 40 in z.
  const Mesh mesh = mask.toMesh();
  ASSERT_FALSE(mesh.triangles.empty());
  std::array<double, 3> lo{1e9, 1e9, 1e9};
  std::array<double, 3> hi{-1e9, -1e9, -1e9};
  for (const auto& triangle : mesh.triangles) {
    for (const auto& p : triangle) {
      for (std::size_t k = 0; k < 3; ++k) {
        lo[k] = std::min(lo[k], static_cast<double>(p[k]));
        hi[k] = std::max(hi[k], static_cast<double>(p[k]));
      }
    }
  }
  for (std::size_t k = 0; k < 3; ++k) {
    EXPECT_NEAR(hi[k] - lo[k], spec.outer_size_mm[k], 0.25 * spec.voxel_size[k]) << "axis " << k;
  }
  EXPECT_NEAR(meshVolumeMm3(mesh), phantomMaterialVolumeMm3(spec),
              0.02 * phantomMaterialVolumeMm3(spec));
}

TEST_F(VoxelSizeTest, StudioImportsVoxelSizesPerAxis) {
  PhantomSpec spec = anisotropicPhantom();
  spec.dims = {64, 64, 32};
  spec.outer_size_mm = {4.0, 4.0, 4.0};
  spec.wall_thickness_mm = 0.8;
  spec.pores = {};
  const Volume16 volume = generatePhantom(spec);
  writeRaw(dir_ / "scan.raw", volume);
  // No sidecar: the voxel size comes from the parameters, with the slice thickness.
  Studio studio({});
  studio.call("project_create", {{"path", (dir_ / "p").string()}});
  const Json imported = studio.call("run_import_raw", {{"path", (dir_ / "scan.raw").string()},
                                                       {"dims", spec.dims},
                                                       {"voxel_size_mm", {0.1, 0.1, 0.2}},
                                                       {"slice_thickness_mm", 0.12},
                                                       {"brick_size", 32}});
  EXPECT_EQ(imported.at("status"), "done");
  EXPECT_EQ(imported.at("summary").at("voxel_size_mm"), Json::array({0.1, 0.1, 0.2}));
  const Json info = studio.call("dataset_info", {});
  EXPECT_EQ(info.at("voxel_size_mm"), Json::array({0.1, 0.1, 0.2}));
  EXPECT_EQ(info.at("slice_thickness_mm"), 0.12);
  const Json slice = studio.call("view_slice", {{"axis", "x"}});
  EXPECT_EQ(slice.at("pixel_size_mm"), Json::array({0.1, 0.2}));
  EXPECT_THROW(studio.call("run_import_raw", {{"path", (dir_ / "scan.raw").string()},
                                              {"dims", spec.dims},
                                              {"voxel_size_mm", {0.1, 0.2}}}),
               std::invalid_argument);
}

}  // namespace
}  // namespace voxelsieve
