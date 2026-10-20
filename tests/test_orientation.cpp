#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <numbers>
#include <random>
#include <string>

#include "voxelsieve/dataset.hpp"
#include "voxelsieve/materials.hpp"
#include "voxelsieve/orientation.hpp"
#include "voxelsieve/report.hpp"
#include "voxelsieve/source.hpp"
#include "voxelsieve/volume.hpp"

namespace voxelsieve {
namespace {

class OrientationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("voxelsieve_orientation_" +
            std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
    std::filesystem::remove_all(dir_);
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  std::filesystem::path dir_;
};

// Angle in degrees between a direction (sign arbitrary) and an axis.
double angleToAxis(const std::array<double, 3>& direction, std::size_t axis) {
  return std::acos(std::clamp(std::abs(direction[axis]), 0.0, 1.0)) * 180.0 / std::numbers::pi;
}

// Two plies of fibres in a resin, in noise: along x in the lower half, along y in the upper half.
void writePlies(const std::filesystem::path& file) {
  constexpr std::int64_t kN = 96;
  constexpr double kResin = 8000.0;
  constexpr double kFibre = 5000.0;  // above the resin, at the centre of a fibre
  constexpr double kRadius = 0.8;    // voxels, Gaussian profile
  Volume16 grey({kN, kN, kN}, VoxelSize{0.1});
  std::mt19937 rng(3);
  std::normal_distribution<double> noise(0.0, 600.0);
  // Distance of coordinate v to the nearest fibre axis, fibres every 4 voxels.
  const auto offset = [](std::int64_t v) {
    const double r = static_cast<double>(v % 4) - 1.5;
    return r;
  };
  for (std::int64_t z = 0; z < kN; ++z) {
    for (std::int64_t y = 0; y < kN; ++y) {
      for (std::int64_t x = 0; x < kN; ++x) {
        double value = 1000.0;
        if (x >= 8 && x < 88 && y >= 8 && y < 88 && z >= 8 && z < 88) {
          // Lower ply: fibres along x; upper ply: along y.
          const double across = z < 48 ? offset(y) : offset(x);
          const double d2 = across * across + offset(z) * offset(z);
          value = kResin + kFibre * std::exp(-0.5 * d2 / (kRadius * kRadius));
        }
        grey.data[grey.index(x, y, z)] =
            static_cast<std::uint16_t>(std::clamp(value + noise(rng), 0.0, 65535.0));
      }
    }
  }
  DatasetOptions sieve;
  sieve.threshold = 4500.0F;
  sieve.brick_size = 32;
  (void)writeDataset(MemorySource(grey), file, sieve);
}

TEST_F(OrientationTest, FindsTheFibreDirectionsOfTwoPlies) {
  writePlies(dir_ / "scan.vsieve");
  const Dataset dataset = Dataset::open(dir_ / "scan.vsieve");

  const OrientationResult result = analyzeOrientation(dataset);
  ASSERT_EQ(result.families.size(), 2U);
  // One family along x, one along y, each about half of the part.
  const auto along_x =
      std::find_if(result.families.begin(), result.families.end(),
                   [](const FibreFamily& f) { return std::abs(f.direction[0]) > 0.9; });
  const auto along_y =
      std::find_if(result.families.begin(), result.families.end(),
                   [](const FibreFamily& f) { return std::abs(f.direction[1]) > 0.9; });
  ASSERT_NE(along_x, result.families.end());
  ASSERT_NE(along_y, result.families.end());
  EXPECT_LT(angleToAxis(along_x->direction, 0), 3.0);
  EXPECT_LT(angleToAxis(along_y->direction, 1), 3.0);
  EXPECT_NEAR(along_x->fraction, 0.5, 0.1);
  EXPECT_NEAR(along_y->fraction, 0.5, 0.1);
  EXPECT_LT(along_x->spread_deg, 5.0);
  EXPECT_GT(result.mean_coherence, 0.3);
  // In the plane of the plies: no fibres along z.
  EXPECT_LT(result.tensor[8], 0.05);
  EXPECT_NEAR(result.tensor[0] + result.tensor[4], 1.0, 0.05);

  writeOrientationResult(dataset, result, dir_ / "out");
  for (const char* file : {"orientation.json", "orientation.vdb", "orientation_x.png",
                           "orientation_y.png", "orientation_z.png"}) {
    EXPECT_TRUE(std::filesystem::exists(dir_ / "out" / file)) << file;
  }
}

// The report shows the materials and the fibre families and evaluates the required ones.
TEST_F(OrientationTest, ReportEvaluatesTheRequiredFibreFamilies) {
  writePlies(dir_ / "scan.vsieve");
  const Dataset dataset = Dataset::open(dir_ / "scan.vsieve");
  SegmentationOptions segmentation;
  segmentation.material_thresholds = {8500.0F};  // below the mean of the plies
  segmentation.names = {"Resin", "Fibres"};
  (void)segmentMaterials(dataset, dir_ / "materials", segmentation);
  const MaterialVolume materials = MaterialVolume::open(dir_ / "materials");
  OrientationOptions options;
  options.materials = &materials;
  writeOrientationResult(dataset, analyzeOrientation(dataset, options), dir_ / "out");

  // Plies at 0° and 90°: met within 3°. A family required at 10° from x misses 5°.
  const auto evaluate = [&](double degrees) {
    const double angle = degrees * std::numbers::pi / 180.0;
    const nlohmann::json order = {
        {"fibres",
         {{"families",
           {{{"name", "0°"},
             {"direction", {std::cos(angle), std::sin(angle), 0.0}},
             {"tolerance_deg", 5.0},
             {"min_share_percent", 40.0},
             {"max_share_percent", 60.0}},
            {{"name", "90°"}, {"direction", {0.0, 1.0, 0.0}}, {"tolerance_deg", 5.0}}}}}}};
    nlohmann::json data = nlohmann::json::object();
    addMaterials(data, dir_ / "materials", &dataset);
    addFibreOrientation(data, dir_ / "out", order);
    return data;
  };
  const nlohmann::json met = evaluate(0.0);
  ASSERT_EQ(met.at("materials").at("classes").size(), 2U);
  EXPECT_EQ(met["materials"]["classes"][1]["name"], "Fibres");
  EXPECT_EQ(met.at("fibres").at("material"), "Fibres");
  EXPECT_EQ(met.at("fibres").at("families").size(), 2U);
  EXPECT_EQ(met["section"]["materials"], 7);
  EXPECT_EQ(met["section"]["fibres"], 8);
  EXPECT_EQ(met["section"]["assessment"], 9);
  for (const char* image : {"materials_x", "materials_z", "orientation_y"}) {
    EXPECT_TRUE(met["images"][image].get<std::string>().starts_with("data:image/png")) << image;
  }
  const nlohmann::json& block = met.at("evaluation").at("zones").at(0);
  EXPECT_EQ(block.at("criteria").size(), 4U);
  EXPECT_TRUE(met["evaluation"]["passed"].get<bool>());
  EXPECT_FALSE(evaluate(10.0)["evaluation"]["passed"].get<bool>());

  const std::string html = renderTemplate(defaultReportTemplate(), met);
  EXPECT_NE(html.find("8 Fibre orientation"), std::string::npos);
  EXPECT_NE(html.find("9 Assessment"), std::string::npos);
}

TEST_F(OrientationTest, RejectsBadOptions) {
  Volume16 grey({16, 16, 16}, VoxelSize{0.1});
  std::fill(grey.data.begin(), grey.data.end(), std::uint16_t{9000});
  (void)writeDataset(MemorySource(grey), dir_ / "scan.vsieve", {});
  const Dataset dataset = Dataset::open(dir_ / "scan.vsieve");
  OrientationOptions options;
  options.block_size = 2;
  EXPECT_THROW((void)analyzeOrientation(dataset, options), std::invalid_argument);
  options = {};
  options.families = 0;
  EXPECT_THROW((void)analyzeOrientation(dataset, options), std::invalid_argument);
}

}  // namespace
}  // namespace voxelsieve
