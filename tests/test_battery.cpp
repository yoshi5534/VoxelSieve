#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <numbers>
#include <span>
#include <string>
#include <vector>

#include "detail/png.hpp"
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

// Anodes as dark as the separator between the layers, as hard carbon or graphite often are: only
// the cathodes stand out in the winding, the anodes only beyond the cathode ends, where the cell
// holds less electrolyte. The third cathode turn reaches 0.1 mm further at the top.
TEST_F(BatteryTest, FindsTheOverhangOfAnodesHiddenInTheWinding) {
  CellSpec spec = defaultCylindricalCell();
  spec.noise_sigma = 500.0;
  spec.anode_value = 7000;
  spec.separator_value = 7000;
  spec.electrolyte_value = 3000;
  spec.extensions.push_back({2, 1, 0.0, 360.0, 0.1});
  const Dataset dataset = sieve(spec);

  const BatteryResult result = analyzeBattery(dataset);
  EXPECT_EQ(result.type, CellType::kCylindrical);
  EXPECT_FALSE(result.levels.electrodes_distinct);
  EXPECT_EQ(result.cathode_layers, spec.turns);
  ASSERT_EQ(result.edges.size(), 2U);
  const BatteryEdge& bottom = edgeOf(result, 2, 0);
  const BatteryEdge& top = edgeOf(result, 2, 1);
  EXPECT_GT(bottom.samples, 100);
  EXPECT_NEAR(bottom.mean_mm, 0.3, 0.04);
  EXPECT_NEAR(top.min_mm, 0.2, 0.05);
  EXPECT_EQ(result.overhang[static_cast<std::size_t>(top.worst)].layer, 2);
  EXPECT_TRUE(result.particles.empty());
  EXPECT_TRUE(result.voids.empty());
}

/// The synthetic cell as section images: cross-sections every fourth slice (and the last), named
/// by their position from `offset_mm`, and longitudinal sections every 5 degrees through the
/// axis, sampled bilinearly.
CellImages cellImages(const CellSpec& spec, double offset_mm) {
  const CellSource source(spec);
  const auto [nx, ny, nz] = spec.dims;
  std::vector<std::uint16_t> volume(static_cast<std::size_t>(nx * ny * nz));
  source.readRegion(Box{{0, 0, 0}, spec.dims}, volume);
  const auto voxel = [&](std::int64_t x, std::int64_t y, std::int64_t z) {
    x = std::clamp<std::int64_t>(x, 0, nx - 1);
    y = std::clamp<std::int64_t>(y, 0, ny - 1);
    return static_cast<double>(volume[static_cast<std::size_t>(x + nx * (y + ny * z))]);
  };
  CellImages images;
  images.cell = "synthetic";
  for (std::int64_t z = 0; z < nz; z = z + 4 < nz || z == nz - 1 ? z + 4 : nz - 1) {
    CellImage image{offset_mm + static_cast<double>(z) * spec.voxel_size[2], nx, ny, {}};
    image.values.assign(volume.begin() + nx * ny * z, volume.begin() + nx * ny * (z + 1));
    images.cross.push_back(std::move(image));
  }
  const double cx = 0.5 * static_cast<double>(nx - 1);
  const double cy = 0.5 * static_cast<double>(ny - 1);
  for (int degrees = 0; degrees < 180; degrees += 5) {
    const double t = degrees * std::numbers::pi / 180.0;
    CellImage image{static_cast<double>(degrees), nx, nz, {}};
    image.values.resize(static_cast<std::size_t>(nx * nz));
    for (std::int64_t row = 0; row < nz; ++row) {
      const std::int64_t z = nz - 1 - row;
      for (std::int64_t col = 0; col < nx; ++col) {
        const double d = static_cast<double>(col) - cx;
        const double x = cx + d * std::cos(t);
        const double y = cy + d * std::sin(t);
        const double fx = std::floor(x);
        const double fy = std::floor(y);
        const auto ix = static_cast<std::int64_t>(fx);
        const auto iy = static_cast<std::int64_t>(fy);
        const double u = x - fx;
        const double v = y - fy;
        const double value = (1.0 - v) * ((1.0 - u) * voxel(ix, iy, z) + u * voxel(ix + 1, iy, z)) +
                             v * ((1.0 - u) * voxel(ix, iy + 1, z) + u * voxel(ix + 1, iy + 1, z));
        image.values[static_cast<std::size_t>(col + nx * row)] =
            static_cast<std::uint16_t>(std::lround(value));
      }
    }
    images.longitudinal.push_back(std::move(image));
  }
  return images;
}

// The cylindrical cell of the first test given as section images instead of a volume.
TEST_F(BatteryTest, InspectsACylindricalCellFromSectionImages) {
  CellSpec spec = defaultCylindricalCell();
  spec.noise_sigma = 800.0;
  spec.extensions.push_back({3, 1, 40.0, 100.0, 0.35});
  const double r = spec.core_radius_mm + cellPitchMm(spec) * (4.0 + 200.0 / 360.0) + 0.05;
  const double t = 200.0 * std::numbers::pi / 180.0;
  spec.particles.push_back({{r * std::cos(t), r * std::sin(t), 0.3}, 0.08});
  const CellImages images = cellImages(spec, -0.5);

  const BatteryResult result = analyzeBatteryImages(images);
  EXPECT_TRUE(result.from_images);
  EXPECT_EQ(result.type, CellType::kCylindrical);
  EXPECT_NEAR(result.voxel_size[0], spec.voxel_size[0], 1e-9);
  EXPECT_TRUE(result.levels.electrodes_distinct);
  EXPECT_EQ(result.cathode_layers, spec.turns);
  EXPECT_NEAR(result.pitch_mm, cellPitchMm(spec), 0.01);
  EXPECT_NEAR(result.outer_mean_mm, 2.0 * cellOuterRadiusMm(spec), 0.05);
  EXPECT_NEAR(result.length_mm, cellOuterSizeMm(spec)[2], 0.03);
  ASSERT_EQ(result.edges.size(), 2U);
  const BatteryEdge& bottom = edgeOf(result, 2, 0);
  const BatteryEdge& top = edgeOf(result, 2, 1);
  EXPECT_GT(bottom.samples, 100);
  EXPECT_NEAR(bottom.mean_mm, 0.3, 0.03);
  EXPECT_NEAR(top.min_mm, -0.05, 0.03);
  const OverhangSample& worst = result.overhang[static_cast<std::size_t>(top.worst)];
  EXPECT_EQ(worst.layer, 3);
  EXPECT_GE(worst.section, 40.0);
  EXPECT_LE(worst.section, 100.0);
  // The particle, seen in a cross-section and maybe in longitudinal sections, counts once.
  ASSERT_EQ(result.particles.size(), 1U);
  EXPECT_NEAR(result.particles[0].radius_mm, r, 0.06);
  EXPECT_NEAR(result.particles[0].angle_deg, 200.0, 4.0);
  EXPECT_TRUE(result.voids.empty());

  writeBatteryResult(result, dir_ / "out");
  EXPECT_TRUE(std::filesystem::exists(dir_ / "out" / "overhang_z_max.png"));
  const nlohmann::json order = {{"battery", {{"max_particles", 0}}}};
  const std::string html =
      renderTemplate(defaultBatteryReportTemplate(), batteryReportData(order, dir_ / "out"));
  EXPECT_NE(html.find("Evaluated from section images"), std::string::npos);
  EXPECT_EQ(html.find("{{"), std::string::npos);
}

// Section images in a folder per cell, 8-bit PNG files named by angle and position.
// A tab, a cathode end far beyond the others, is left out of the overhang; a void in the
// winding is found, one in the innermost turn, where folded separator looks like gas, is not.
TEST_F(BatteryTest, LeavesTabsAndTheInnermostTurnOutOfSectionImages) {
  CellSpec spec = defaultCylindricalCell();
  spec.noise_sigma = 800.0;
  spec.extensions.push_back({2, 1, 200.0, 260.0, 1.2});  // overhang 0.3 - 1.2 = -0.9 mm
  const auto at = [&](double turn, double deg) {
    const double r = spec.core_radius_mm + cellPitchMm(spec) * (turn + deg / 360.0) + 0.05;
    const double t = deg * std::numbers::pi / 180.0;
    return std::array<double, 3>{r * std::cos(t), r * std::sin(t), -0.5};
  };
  spec.voids.push_back({at(3.0, 100.0), 0.1});
  spec.voids.push_back({at(0.0, 300.0), 0.1});
  // The small cell's cathode sticks out at most to the cap, so a tighter limit than on a real
  // cell.
  BatteryOptions options;
  options.max_end_deviation_mm = 0.3;
  const BatteryResult result = analyzeBatteryImages(cellImages(spec, -0.5), options);
  const BatteryEdge& top = edgeOf(result, 2, 1);
  EXPECT_NEAR(top.min_mm, 0.3, 0.05);
  EXPECT_TRUE(std::any_of(result.warnings.begin(), result.warnings.end(), [](const auto& w) {
    return w.find("are left out") != std::string::npos && w.find("tabs") != std::string::npos;
  }));
  // With the default limit the cathode end is left out because its neighbours end lower.
  EXPECT_NEAR(edgeOf(analyzeBatteryImages(cellImages(spec, -0.5)), 2, 1).min_mm, 0.3, 0.05);
  ASSERT_EQ(result.voids.size(), 1U);
  const std::array<double, 3> v = at(3.0, 100.0);
  EXPECT_NEAR(result.voids[0].radius_mm, std::hypot(v[0], v[1]), 0.1);
  EXPECT_NEAR(result.voids[0].angle_deg, 100.0, 5.0);
}

TEST_F(BatteryTest, ReadsCellImagesFromAFolder) {
  const auto write = [&](const std::filesystem::path& path, std::uint8_t value) {
    std::filesystem::create_directories(path.parent_path());
    const std::vector<std::uint8_t> pixels(6, value);
    const std::vector<std::uint8_t> png = detail::encodePng(3, 2, 1, pixels);
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
  };
  for (const char* cell : {"cells/10", "cells/9"}) {
    write(dir_ / cell / "axial_images" / "90.0.png", 90);
    write(dir_ / cell / "axial_images" / "0.0.png", 1);
    write(dir_ / cell / "radial_images" / "12.5.png", 125);
    write(dir_ / cell / "radial_images" / "-0.5.png", 5);
  }
  write(dir_ / "cells" / "only_cross" / "radial_images" / "1.0.png", 10);
  write(dir_ / "cells" / "9" / "notes.png", 0);

  EXPECT_EQ(listCellImages(dir_), (std::vector<std::string>{"9", "10"}));
  const CellImages images = readCellImages(dir_, "10");
  EXPECT_EQ(images.cell, "10");
  ASSERT_EQ(images.longitudinal.size(), 2U);
  ASSERT_EQ(images.cross.size(), 2U);
  EXPECT_EQ(images.longitudinal[0].position, 0.0);
  EXPECT_EQ(images.longitudinal[1].position, 90.0);
  EXPECT_EQ(images.cross[0].position, -0.5);
  EXPECT_EQ(images.cross[1].position, 12.5);
  EXPECT_EQ(images.cross[1].width, 3);
  EXPECT_EQ(images.cross[1].height, 2);
  EXPECT_EQ(images.cross[1].values, std::vector<std::uint16_t>(6, 125));
  EXPECT_THROW((void)readCellImages(dir_, "11"), std::invalid_argument);
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
