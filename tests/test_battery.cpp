#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <numbers>
#include <string>
#include <vector>

#include "voxelsieve/battery.hpp"
#include "voxelsieve/cells.hpp"
#include "voxelsieve/dataset.hpp"
#include "voxelsieve/report.hpp"

namespace voxelsieve {
namespace {

class BatteryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("voxelsieve_battery_" +
            std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
    std::filesystem::remove_all(dir_);
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  Dataset sieve(const CellSpec& spec) {
    DatasetOptions options;
    options.brick_size = 64;
    (void)writeDataset(CellSource(spec), dir_ / "cell.vsieve", options);
    return Dataset::open(dir_ / "cell.vsieve");
  }

  std::filesystem::path dir_;
};

const BatteryEdge& edgeOf(const BatteryResult& result, int axis, int side) {
  const auto it = std::find_if(result.edges.begin(), result.edges.end(), [&](const BatteryEdge& e) {
    return e.axis == axis && e.side == side;
  });
  EXPECT_NE(it, result.edges.end());
  return *it;
}

// A wound cell, 3 % wider along x, with a cathode that sticks out at the top over 60°, a metal
// particle and a gas bubble in the winding.
TEST_F(BatteryTest, InspectsACylindricalCell) {
  CellSpec spec = defaultCylindricalCell();
  spec.noise_sigma = 800.0;
  spec.ovality = 0.03;
  spec.extensions.push_back({3, 1, 40.0, 100.0, 0.35});  // overhang 0.3 - 0.35 = -0.05 mm
  // In turn 4 at 200°: radius core + pitch * (4 + 200/360) + half a cathode.
  const double r = spec.core_radius_mm + cellPitchMm(spec) * (4.0 + 200.0 / 360.0) + 0.05;
  const double t = 200.0 * std::numbers::pi / 180.0;
  spec.particles.push_back({{r * std::cos(t) * 1.03, r * std::sin(t), 0.3}, 0.06});
  spec.voids.push_back({{-0.8, 1.0, -0.5}, 0.08});
  const Dataset dataset = sieve(spec);

  const BatteryResult result = analyzeBattery(dataset);
  EXPECT_EQ(result.type, CellType::kCylindrical);
  EXPECT_EQ(result.axis, 2);
  EXPECT_TRUE(result.levels.electrodes_distinct);
  EXPECT_EQ(result.cathode_layers, spec.turns);
  EXPECT_EQ(result.anode_layers, spec.turns);
  EXPECT_NEAR(result.pitch_mm, cellPitchMm(spec), 0.01);
  // The outer radius runs from R along y to 1.03 R along x.
  const double outer = cellOuterRadiusMm(spec);
  EXPECT_NEAR(result.outer_mean_mm, 2.0 * outer * 1.015, 0.05);
  EXPECT_NEAR(result.max_out_of_roundness_mm, 0.03 * outer, 0.03);
  EXPECT_NEAR(result.outer_size_mm[0], cellOuterSizeMm(spec)[0], 0.03);
  EXPECT_NEAR(result.outer_size_mm[1], cellOuterSizeMm(spec)[1], 0.03);
  EXPECT_NEAR(result.length_mm, cellOuterSizeMm(spec)[2], 0.03);
  EXPECT_NEAR(result.min_wall_mm, spec.housing_wall_mm, 0.04);
  for (const BatterySection& s : result.sections) {
    EXPECT_NEAR(s.core_diameter_mm, 2.0 * spec.core_radius_mm * 1.0, 0.1);
  }

  ASSERT_EQ(result.edges.size(), 2U);
  const BatteryEdge& bottom = edgeOf(result, 2, 0);
  const BatteryEdge& top = edgeOf(result, 2, 1);
  EXPECT_GT(bottom.samples, 100);
  EXPECT_NEAR(bottom.mean_mm, 0.3, 0.03);
  EXPECT_NEAR(bottom.min_mm, 0.3, 0.08);
  EXPECT_NEAR(top.min_mm, -0.05, 0.03);
  const OverhangSample& worst = result.overhang[static_cast<std::size_t>(top.worst)];
  EXPECT_EQ(worst.layer, 3);
  EXPECT_TRUE(result.gaps.empty());
  EXPECT_GE(worst.section, 40.0);
  EXPECT_LE(worst.section, 100.0);
  EXPECT_NEAR(result.min_overhang_mm, -0.05, 0.03);

  ASSERT_EQ(result.particles.size(), 1U);
  EXPECT_NEAR(result.particles[0].volume_mm3, 4.0 / 3.0 * std::numbers::pi * std::pow(0.06, 3),
              0.0004);
  ASSERT_EQ(result.voids.size(), 1U);
  EXPECT_NEAR(result.voids[0].equivalent_diameter_mm, 0.16, 0.03);

  writeBatteryResult(result, dir_ / "out");
  for (const char* file : {"battery.json", "cross_section.png", "longitudinal.png",
                           "overhang_z_min.png", "overhang_z_max.png"}) {
    EXPECT_TRUE(std::filesystem::exists(dir_ / "out" / file)) << file;
  }

  // The report: the cathode that sticks out fails the overhang, the particle the particle count.
  const nlohmann::json order = {{"battery",
                                 {{"min_overhang_mm", 0.1},
                                  {"cathode_layers", {{"min", 6}, {"max", 6}}},
                                  {"min_wall_mm", 0.1},
                                  {"max_gaps", 0},
                                  {"max_particles", 0},
                                  {"max_voids", 1}}}};
  std::vector<std::string> warnings;
  const nlohmann::json data = batteryReportData(order, dir_ / "out", &warnings);
  EXPECT_FALSE(warnings.empty());  // the order has no report number, customer, ...
  const nlohmann::json& evaluation = data.at("evaluation");
  EXPECT_FALSE(evaluation.at("passed").get<bool>());
  std::map<std::string, bool> passed;
  for (const nlohmann::json& c : evaluation.at("criteria")) {
    passed[c.at("label").get<std::string>()] = c.at("passed").get<bool>();
  }
  EXPECT_EQ(passed.size(), 6U);
  EXPECT_FALSE(passed.at("Smallest anode overhang, all edges"));
  EXPECT_TRUE(passed.at("Cathode layers (turns) per radius"));
  EXPECT_TRUE(passed.at("Thinnest housing wall"));
  EXPECT_TRUE(passed.at("Gaps between the layers"));
  EXPECT_FALSE(passed.at("Foreign particles"));
  EXPECT_TRUE(passed.at("Gas voids"));
  const std::string html = renderTemplate(defaultBatteryReportTemplate(), data);
  EXPECT_NE(html.find("Battery cell test report"), std::string::npos);
  EXPECT_NE(html.find("<svg"), std::string::npos);  // the overhang chart
  EXPECT_NE(html.find("data:image/png;base64,"), std::string::npos);
  EXPECT_NE(html.find("The requirements are not met."), std::string::npos);
  EXPECT_EQ(html.find("{{"), std::string::npos);
}

// A stack of five cathode sheets; the third reaches 0.2 mm further at high x.
TEST_F(BatteryTest, InspectsAStackedCell) {
  CellSpec spec = defaultStackedCell();
  spec.noise_sigma = 800.0;
  spec.extensions.push_back({2, 1, 0.0, 0.0, 0.2});
  spec.particles.push_back({{0.3, -0.2, 0.3 - 0.5 * 1.6 + 0.05}, 0.05});
  spec.gap_after = 3;  // the layers come apart by 0.2 mm above the fourth cathode
  spec.gap_mm = 0.2;
  const Dataset dataset = sieve(spec);

  const BatteryResult result = analyzeBattery(dataset);
  EXPECT_EQ(result.type, CellType::kStacked);
  EXPECT_EQ(result.axis, 2);
  EXPECT_EQ(result.cathode_layers, spec.cathode_layers);
  EXPECT_EQ(result.anode_layers, spec.cathode_layers + 1);
  EXPECT_NEAR(result.pitch_mm, cellPitchMm(spec), 0.01);
  EXPECT_NEAR(result.outer_mean_mm, cellOuterSizeMm(spec)[2], 0.05);
  EXPECT_NEAR(result.sections[12].stack_mm,
              cellGroundTruth(spec).at("stack_height_mm").get<double>() - spec.anode_thickness_mm,
              0.03);
  for (std::size_t a = 0; a < 3; ++a) {
    EXPECT_NEAR(result.outer_size_mm[a], cellOuterSizeMm(spec)[a], 0.03) << a;
  }
  EXPECT_NEAR(result.min_wall_mm, spec.housing_wall_mm, 0.04);
  ASSERT_EQ(result.edges.size(), 4U);
  EXPECT_NEAR(edgeOf(result, 0, 0).mean_mm, 0.3, 0.04);
  EXPECT_NEAR(edgeOf(result, 1, 0).mean_mm, 0.3, 0.04);
  EXPECT_NEAR(edgeOf(result, 1, 1).mean_mm, 0.3, 0.04);
  const BatteryEdge& shifted = edgeOf(result, 0, 1);
  EXPECT_NEAR(shifted.min_mm, 0.1, 0.05);
  EXPECT_EQ(result.overhang[static_cast<std::size_t>(shifted.worst)].layer, 2);
  ASSERT_EQ(result.particles.size(), 1U);
  EXPECT_TRUE(result.voids.empty());
  // The gap shows in every profile, between the separator above the fourth cathode and the
  // fifth anode: 0.2 mm on top of the usual distance between neighbouring layers.
  EXPECT_EQ(result.gaps.size(), result.sections.size());
  for (const LayerGap& gap : result.gaps) {
    EXPECT_NEAR(gap.size_mm - gap.expected_mm, 0.2, 0.03);
  }
}

}  // namespace
}  // namespace voxelsieve
