#include <gtest/gtest.h>
#include <openvdb/io/File.h>

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

#include "voxelsieve/dataset.hpp"
#include "voxelsieve/mesh.hpp"
#include "voxelsieve/porosity.hpp"
#include "voxelsieve/synthetic.hpp"

namespace voxelsieve {
namespace {

class PorosityTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("voxelsieve_porosity_" +
            std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
    std::filesystem::remove_all(dir_);
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  /// Sieves a synthetic scan of a box into a dataset and analyses it.
  PorosityResult analyze(const SyntheticScan& scan) {
    DatasetOptions options;
    options.brick_size = 32;  // several bricks, so pores and zones cross brick boundaries
    (void)writeDataset(scan, dir_ / "scan.vsieve", options);
    dataset_ = std::make_unique<Dataset>(Dataset::open(dir_ / "scan.vsieve"));
    return analyzePorosity(*dataset_);
  }

  /// Truth position in voxel coordinates.
  static std::array<double, 3> toVoxels(const SyntheticScan& scan,
                                        const std::array<double, 3>& mm) {
    const auto origin = scan.originMm();
    return {(mm[0] - origin[0]) / scan.voxelSizeMm(), (mm[1] - origin[1]) / scan.voxelSizeMm(),
            (mm[2] - origin[2]) / scan.voxelSizeMm()};
  }

  static double distance(const std::array<double, 3>& a, const std::array<double, 3>& b) {
    return std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
  }

  std::filesystem::path dir_;
  std::unique_ptr<Dataset> dataset_;
};

TEST_F(PorosityTest, SoundPartHasNoFindings) {
  SyntheticSpec spec;
  spec.noise_sigma = 500.0;
  spec.cupping = 0.1;
  const SyntheticScan scan(boxMesh({8.0, 6.0, 4.0}), spec);
  const PorosityResult result = analyze(scan);
  EXPECT_TRUE(result.pores.empty());
  EXPECT_TRUE(result.zones.empty());
  // The material level follows the cupping with depth, so the part volume stays right.
  EXPECT_NEAR(result.part_volume_mm3, 192.0, 0.005 * 192.0);
  EXPECT_NEAR(result.noise_sigma, 500.0, 50.0);
}

TEST_F(PorosityTest, LunkersMatchGroundTruth) {
  SyntheticSpec spec;
  spec.noise_sigma = 500.0;
  spec.cupping = 0.1;
  spec.lunker_count = 3;
  spec.lunker_radius_mm = 0.5;
  const SyntheticScan scan(boxMesh({10.0, 6.0, 4.0}), spec);
  ASSERT_EQ(scan.defects().size(), 3U);
  const PorosityResult result = analyze(scan);

  ASSERT_EQ(result.pores.size(), 3U);
  EXPECT_TRUE(result.zones.empty());
  for (const Defect& lunker : scan.defects()) {
    const auto truth = toVoxels(scan, lunker.center_mm);
    const auto match = std::min_element(
        result.pores.begin(), result.pores.end(), [&](const auto& a, const auto& b) {
          return distance(a.center_voxels, truth) < distance(b.center_voxels, truth);
        });
    // The void-weighted centre of the lobes is not the enclosing centre, so allow a few voxels.
    EXPECT_LT(distance(match->center_voxels, truth), 3.0);
    EXPECT_NEAR(match->volume_mm3, lunker.void_volume_mm3, 0.03 * lunker.void_volume_mm3);
  }
  const double part = 10.0 * 6.0 * 4.0;
  EXPECT_NEAR(result.part_volume_mm3, part, 0.005 * part);
}

TEST_F(PorosityTest, LargeLunkerCountsEveryVoxel) {
  // A lunker with completely empty 8^3 leaves, which OpenVDB stores as tiles.
  SyntheticSpec spec;
  spec.noise_sigma = 500.0;
  spec.lunker_count = 1;
  spec.lunker_radius_mm = 2.5;
  const SyntheticScan scan(boxMesh({8.0, 8.0, 8.0}), spec);
  ASSERT_EQ(scan.defects().size(), 1U);
  const PorosityResult result = analyze(scan);

  ASSERT_EQ(result.pores.size(), 1U);
  const double truth = scan.defects().front().void_volume_mm3;
  EXPECT_NEAR(result.pores.front().volume_mm3, truth, 0.03 * truth);
}

TEST_F(PorosityTest, LoosenedZonesMatchGroundTruth) {
  SyntheticSpec spec;
  spec.noise_sigma = 500.0;
  spec.cupping = 0.1;
  spec.cupping_depth_mm = 2.0;
  spec.loosening_count = 1;
  spec.loosening_radius_mm = 1.2;
  spec.loosening_porosity = 0.05;
  spec.seed = 3;
  const SyntheticScan scan(boxMesh({8.0, 8.0, 8.0}), spec);
  ASSERT_EQ(scan.defects().size(), 1U);
  const Defect& truth = scan.defects().front();
  const PorosityResult result = analyze(scan);

  ASSERT_EQ(result.zones.size(), 1U);
  const PorosityZone& zone = result.zones.front();
  EXPECT_LT(distance(zone.center_voxels, toVoxels(scan, truth.center_mm)), 4.0);
  // Clusters of pores that happen to be resolved are reported as pores; together with the zone
  // they carry the void volume.
  double found = zone.void_volume_mm3;
  for (const DetectedPore& pore : result.pores) {
    EXPECT_LT(distance(pore.center_voxels, toVoxels(scan, truth.center_mm)),
              truth.radius_mm / scan.voxelSizeMm() + 2.0);
    found += pore.volume_mm3;
  }
  EXPECT_NEAR(found, truth.void_volume_mm3, 0.15 * truth.void_volume_mm3);
  EXPECT_NEAR(result.porosity(), truth.void_volume_mm3 / 512.0,
              0.15 * truth.void_volume_mm3 / 512.0);
}

TEST_F(PorosityTest, WritesJsonImagesAndVdb) {
  SyntheticSpec spec;
  spec.noise_sigma = 300.0;
  spec.lunker_count = 2;
  spec.lunker_radius_mm = 0.4;
  const SyntheticScan scan(boxMesh({6.0, 5.0, 4.0}), spec);
  const PorosityResult result = analyze(scan);
  ASSERT_EQ(result.pores.size(), 2U);

  const auto json = toJson(result);
  EXPECT_EQ(json.at("pores").size(), 2U);
  EXPECT_NEAR(json.at("porosity").get<double>(), result.porosity(), 1e-12);

  const auto out = dir_ / "out";
  writePorosityImages(*dataset_, result, out);
  for (const char* name : {"projection_x.png", "projection_y.png", "projection_z.png"}) {
    std::ifstream png(out / name, std::ios::binary);
    std::string signature(8, '\0');
    png.read(signature.data(), 8);
    EXPECT_EQ(signature, "\x89PNG\r\n\x1a\n") << name;
  }

  writePorosityVdb(*dataset_, result, out / "porosity.vdb");
  openvdb::io::File file((out / "porosity.vdb").string());
  file.open();
  const auto pores = openvdb::gridPtrCast<openvdb::FloatGrid>(file.readGrid("pores"));
  ASSERT_TRUE(pores);
  std::int64_t voxels = 0;
  for (const DetectedPore& pore : result.pores) {
    voxels += pore.voxel_count;
  }
  EXPECT_EQ(static_cast<std::int64_t>(pores->activeVoxelCount()), voxels);
  EXPECT_TRUE(file.readGrid("zones"));
}

}  // namespace
}  // namespace voxelsieve
