#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <optional>
#include <string>
#include <utility>

#include "voxelsieve/dataset.hpp"
#include "voxelsieve/phantom.hpp"
#include "voxelsieve/report.hpp"
#include "voxelsieve/source.hpp"
#include "voxelsieve/surface.hpp"
#include "voxelsieve/thickness.hpp"
#include "voxelsieve/volume.hpp"

namespace voxelsieve {
namespace {

using Method = WallThicknessOptions::Method;
using Index3 = std::array<std::int64_t, 3>;

class ThicknessTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Named after suite and test, since ThicknessMapTest shares test names with ThicknessTest.
    const ::testing::TestInfo* info = ::testing::UnitTest::GetInstance()->current_test_info();
    dir_ = std::filesystem::temp_directory_path() /
           ("voxelsieve_" + std::string(info->test_suite_name()) + "_" + info->name());
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  /// Sieves `source` and extracts its surface.
  void scan(const VolumeSource& source) {
    DatasetOptions options;
    options.brick_size = 64;
    (void)writeDataset(source, dir_ / "scan.vsieve", options);
    (void)writeSurface(Dataset::open(dir_ / "scan.vsieve"), dir_ / "surface.vss");
  }

  [[nodiscard]] WallThicknessResult measure(const WallThicknessOptions& options) const {
    return analyzeWallThickness(Dataset::open(dir_ / "scan.vsieve"),
                                SurfaceMask::open(dir_ / "surface.vss"), options);
  }

  /// The hollow box of the phantom, 8 mm with walls of 1.5 mm, without pores.
  static PhantomSpec hollowBox(const VoxelSize& voxel_size) {
    PhantomSpec spec;
    spec.voxel_size = voxel_size;
    for (std::size_t k = 0; k < 3; ++k) {
      spec.dims[k] = static_cast<std::int64_t>(std::ceil(10.0 / voxel_size[k]));
    }
    spec.noise_sigma = 600.0;
    return spec;
  }

  std::filesystem::path dir_;
};

/// Thickness of the hollow box at a surface point `p` (dataset mm) along the inward normal of its
/// face: 1.5 mm where the cavity lies behind the wall, 8 mm where the ray runs along a side wall
/// to the opposite face; nullopt near the edges and corners of the box and of the cavity. With
/// `sphere`, only the points in front of the cavity, where the largest ball is the wall.
std::optional<double> hollowBoxThickness(const PhantomSpec& spec, const std::array<float, 3>& p,
                                         bool sphere) {
  std::array<double, 3> rel{};
  for (std::size_t k = 0; k < 3; ++k) {
    rel[k] = p[k] - (static_cast<double>(spec.dims[k]) / 2.0 - 0.5) * spec.voxel_size[k];
  }
  int face = -1;
  for (int k = 0; k < 3; ++k) {
    if (std::abs(std::abs(rel[static_cast<std::size_t>(k)]) - 4.0) < 0.15) {
      face = face < 0 ? k : 3;
    }
  }
  if (face < 0 || face == 3) {
    return std::nullopt;
  }
  bool behind_cavity = true;
  for (std::size_t k = 0; k < 3; ++k) {
    if (static_cast<int>(k) == face) {
      continue;
    }
    const double a = std::abs(rel[k]);
    if (a > 3.6 || (a > 2.3 && a < 2.7) || (sphere && a >= 2.3)) {
      return std::nullopt;
    }
    behind_cavity = behind_cavity && a < 2.5;
  }
  return behind_cavity ? 1.5 : 8.0;
}

/// Share of the vertices with an analytic thickness that are measured within `tolerance` of it,
/// and how many have one.
std::pair<double, std::size_t> matchesHollowBox(const PhantomSpec& spec,
                                                const WallThicknessResult& result, bool sphere,
                                                double tolerance) {
  std::size_t known = 0;
  std::size_t matching = 0;
  for (std::size_t i = 0; i < result.mesh.points.size(); ++i) {
    const auto expected = hollowBoxThickness(spec, result.mesh.points[i], sphere);
    if (!expected) {
      continue;
    }
    ++known;
    const float t = result.thickness_mm[i];
    matching += std::isfinite(t) && std::abs(t - *expected) <= tolerance ? 1 : 0;
  }
  return {known > 0 ? static_cast<double>(matching) / static_cast<double>(known) : 0.0, known};
}

TEST_F(ThicknessTest, RaysMeasureTheWallsOfTheHollowBox) {
  const PhantomSpec spec = hollowBox(VoxelSize{0.1});
  scan(PhantomSource(spec));
  WallThicknessOptions options;
  options.method = Method::kRay;
  options.max_thickness_mm = 10.0;
  const WallThicknessResult result = measure(options);
  // The outer skin is measured; the closed cavity is a surface of its own.
  EXPECT_EQ(result.components, 2U);
  EXPECT_EQ(result.dropped_components, 1U);
  EXPECT_EQ(result.thickness_mm.size(), result.mesh.points.size());
  const ThicknessStats& s = result.stats;
  EXPECT_NEAR(s.area_mm2, 6.0 * 8.0 * 8.0, 6.0);
  // Only the rays through the edges and corners leave the wall at a steep angle.
  EXPECT_GT(s.measured_area_mm2 / s.area_mm2, 0.9);
  // In front of the cavity (25 of the 64 mm² of a face) the wall, elsewhere along a side wall.
  EXPECT_NEAR(s.percentiles_mm[0], 1.5, 0.05);
  EXPECT_GT(s.percentiles_mm[4], 7.9);  // rays tilted near the edges are a little longer
  EXPECT_LT(s.percentiles_mm[4], 8.5);
  const auto [share, known] = matchesHollowBox(spec, result, false, 0.05);
  EXPECT_GT(known, 10000U);
  EXPECT_GT(share, 0.97);
}

TEST_F(ThicknessTest, RaysFollowThePitchOfLongVoxels) {
  // Slices half again as far apart as the pixels.
  const PhantomSpec spec = hollowBox(VoxelSize{0.1, 0.1, 0.15});
  scan(PhantomSource(spec));
  WallThicknessOptions options;
  options.max_thickness_mm = 10.0;
  const WallThicknessResult result = measure(options);
  const auto [share, known] = matchesHollowBox(spec, result, false, 0.075);
  EXPECT_GT(known, 5000U);
  EXPECT_GT(share, 0.95);
}

TEST_F(ThicknessTest, SpheresMeasureTheWallsOfTheHollowBox) {
  const PhantomSpec spec = hollowBox(VoxelSize{0.1});
  scan(PhantomSource(spec));
  WallThicknessOptions options;
  options.method = Method::kSphere;
  const WallThicknessResult result = measure(options);
  const ThicknessStats& s = result.stats;
  EXPECT_GT(s.measured_area_mm2 / s.area_mm2, 0.99);
  // A ball that touches a face close to an edge is as small as its distance to the edge allows;
  // over the side walls it reaches into the thicker corner of the box.
  EXPECT_LT(s.percentiles_mm[0], 1.0);
  EXPECT_LT(s.percentiles_mm[4], 2.5);
  const auto [share, known] = matchesHollowBox(spec, result, true, 0.06);
  EXPECT_GT(known, 5000U);
  EXPECT_GT(share, 0.95);
}

TEST_F(ThicknessTest, PoresThinTheWall) {
  PhantomSpec spec = hollowBox(VoxelSize{0.1});
  // A pore of 0.8 mm in the middle of the top wall (z from 2.5 to 4 mm): 0.35 mm of wall remain
  // above it.
  spec.pores = {{{0.0, 0.0, 3.25}, 0.4}};
  scan(PhantomSource(spec));
  WallThicknessOptions options;
  options.min_wall_mm = 1.0;
  const WallThicknessResult result = measure(options);
  ASSERT_FALSE(result.thin_regions.empty());
  const ThicknessRegion& thinnest = result.thin_regions.front();
  EXPECT_NEAR(thinnest.extreme_mm, 0.35, 0.05);
  // Above the pore, on the outer face of the top wall (z = 9 mm in dataset coordinates).
  EXPECT_NEAR(thinnest.location_mm[0], 5.0, 0.2);
  EXPECT_NEAR(thinnest.location_mm[1], 5.0, 0.2);
  EXPECT_NEAR(thinnest.location_mm[2], 9.0, 0.1);
  EXPECT_LT(thinnest.area_mm2, 1.0);
  EXPECT_GT(result.stats.below_min, 0.0);
  EXPECT_LT(result.stats.below_min, 0.01);
}

TEST_F(ThicknessTest, FindsTheThinStepOfAPlate) {
  // A plate of 6 x 6 mm, 1 mm thick for x < 3 mm and 2 mm thick from there on, in 0.1 mm voxels
  // with 1 mm of air around it.
  const VoxelSize voxel_size{0.1};
  Volume16 volume({80, 80, 40}, voxel_size);
  const auto box_distance = [](const std::array<double, 3>& p, const std::array<double, 3>& low,
                               const std::array<double, 3>& high) {
    double outside = 0.0;
    double inside = -1e9;
    for (std::size_t k = 0; k < 3; ++k) {
      const double d = std::max(low[k] - p[k], p[k] - high[k]);
      outside += std::pow(std::max(d, 0.0), 2);
      inside = std::max(inside, std::min(d, 0.0));
    }
    return std::sqrt(outside) + inside;
  };
  for (std::int64_t z = 0; z < 40; ++z) {
    for (std::int64_t y = 0; y < 80; ++y) {
      for (std::int64_t x = 0; x < 80; ++x) {
        const std::array<double, 3> p = voxel_size.toMm(
            {static_cast<double>(x), static_cast<double>(y), static_cast<double>(z)});
        // The boxes overlap, so that the minimum of their distances is negative where they meet.
        const double d = std::min(box_distance(p, {1.0, 1.0, 1.0}, {4.5, 7.0, 2.0}),
                                  box_distance(p, {4.0, 1.0, 1.0}, {7.0, 7.0, 3.0}));
        const double fraction = std::clamp(0.5 - d / 0.1, 0.0, 1.0);
        volume.at(x, y, z) = static_cast<std::uint16_t>(std::lround(1000.0 + 19000.0 * fraction));
      }
    }
  }
  scan(MemorySource(volume));
  WallThicknessOptions options;
  options.min_wall_mm = 1.5;
  options.max_wall_mm = 2.5;
  options.max_thickness_mm = 8.0;
  const WallThicknessResult result = measure(options);
  ASSERT_FALSE(result.thin_regions.empty());
  // The top and bottom faces of the thin half, each about 3 x 6 mm.
  double thin_area = 0.0;
  for (const ThicknessRegion& region : result.thin_regions) {
    thin_area += region.area_mm2;
    EXPECT_LT(region.bounds_mm[1][0], 4.2);
  }
  EXPECT_NEAR(result.thin_regions.front().extreme_mm, 1.0, 0.05);
  EXPECT_NEAR(thin_area, 2.0 * 3.0 * 6.0, 6.0);
  // Across the plate, the rays from its sides find 6 mm: thicker than the maximum. Rays from
  // close to an edge run up to the search angle askew and a little longer.
  ASSERT_FALSE(result.thick_regions.empty());
  EXPECT_GT(result.thick_regions.front().extreme_mm, 5.9);
  EXPECT_LT(result.thick_regions.front().extreme_mm, 6.0 / std::cos(std::numbers::pi / 6.0));
  for (const float t : result.thickness_mm) {
    if (std::isfinite(t)) {
      EXPECT_GT(t, 0.9);
    }
  }
}

TEST_F(ThicknessTest, WritesAndReadsItsFiles) {
  scan(PhantomSource(hollowBox(VoxelSize{0.1})));
  WallThicknessOptions options;
  options.min_wall_mm = 1.0;
  const WallThicknessResult result = measure(options);
  writeWallThickness(result, dir_ / "out");
  for (const char* file :
       {"wall_thickness.json", "thickness.ply", "thickness_view_1.png", "thickness_view_2.png"}) {
    EXPECT_TRUE(std::filesystem::exists(dir_ / "out" / file)) << file;
  }
  const ThicknessMesh read = readThicknessPly(dir_ / "out" / "thickness.ply");
  ASSERT_EQ(read.mesh.points.size(), result.mesh.points.size());
  ASSERT_EQ(read.mesh.triangles.size(), result.mesh.triangles.size());
  for (std::size_t i = 0; i < read.thickness_mm.size(); ++i) {
    const float a = read.thickness_mm[i];
    const float b = result.thickness_mm[i];
    EXPECT_TRUE((std::isnan(a) && std::isnan(b)) || a == b);
  }
  std::ifstream in(dir_ / "out" / "wall_thickness.json");
  const nlohmann::json json = nlohmann::json::parse(in);
  EXPECT_EQ(json.at("method"), "ray");
  EXPECT_NEAR(json.at("thickness").at("percentiles_mm").at("1").get<double>(), 1.5, 0.05);
  EXPECT_EQ(json.at("thin_region_count"), 0);
  EXPECT_DOUBLE_EQ(json.at("min_wall_mm").get<double>(), 1.0);
}

TEST_F(ThicknessTest, TheReportEvaluatesTheLimitsOfTheOrder) {
  PhantomSpec spec = hollowBox(VoxelSize{0.1});
  spec.pores = {{{0.0, 0.0, 3.25}, 0.4}};  // 0.35 mm of wall left above it
  scan(PhantomSource(spec));
  writeWallThickness(measure({}), dir_ / "out");

  // The limit of the order counts, not that of the analysis (none).
  nlohmann::json data = nlohmann::json::object();
  addWallThickness(data, dir_ / "out", {{"wall_thickness", {{"min_mm", 1.0}}}});
  const nlohmann::json& section = data.at("wall_thickness");
  EXPECT_EQ(section.at("min_wall").at("count"), 1);
  EXPECT_EQ(section.at("min_wall").at("regions").size(), 1U);
  EXPECT_FALSE(data.at("images").at("thickness_1").get<std::string>().empty());
  EXPECT_NE(data.at("images").at("thickness_histogram").get<std::string>().find("<svg"),
            std::string::npos);
  const nlohmann::json& block = data.at("evaluation").at("zones").at(0);
  EXPECT_EQ(block.at("name"), "Wall thickness");
  EXPECT_EQ(block.at("criteria").at(0).at("label"), "Thinnest wall");
  EXPECT_FALSE(data.at("evaluation").at("passed"));

  // A small thin spot may be tolerated by its area.
  data = nlohmann::json::object();
  addWallThickness(data, dir_ / "out",
                   {{"wall_thickness", {{"min_mm", 1.0}, {"max_area_below_min_mm2", 1.0}}}});
  EXPECT_TRUE(data.at("evaluation").at("passed"));
  data = nlohmann::json::object();
  addWallThickness(data, dir_ / "out",
                   {{"wall_thickness", {{"min_mm", 1.0}, {"max_area_below_min_mm2", 0.01}}}});
  EXPECT_FALSE(data.at("evaluation").at("passed"));

  // Without limits, the findings only.
  data = nlohmann::json::object();
  addWallThickness(data, dir_ / "out", nlohmann::json::object());
  EXPECT_FALSE(data.contains("evaluation"));
  EXPECT_FALSE(data.at("wall_thickness").contains("min_wall"));
}

TEST_F(ThicknessTest, RejectsInvalidOptions) {
  scan(PhantomSource(hollowBox(VoxelSize{0.2})));
  WallThicknessOptions options;
  options.min_wall_mm = 2.0;
  options.max_wall_mm = 1.0;
  EXPECT_THROW((void)measure(options), std::invalid_argument);
  options = {};
  options.search_angle_deg = 0.0;
  EXPECT_THROW((void)measure(options), std::invalid_argument);
  EXPECT_THROW((void)thicknessMethodFromString("cube"), std::invalid_argument);
  EXPECT_EQ(thicknessMethodFromString("sphere"), Method::kSphere);
}

/// Local thickness of the hollow box at voxel `p` (the wall), or nullopt within 0.5 mm of the
/// edges and corners of the cavity or of the box, where it grows towards the diagonals.
std::optional<double> hollowBoxLocalThickness(const PhantomSpec& spec, const Index3& p) {
  const std::array<double, 3> rel = voxelCenterMm(spec, p[0], p[1], p[2]);
  int in_wall = 0;
  for (const double r : rel) {
    const double a = std::abs(r);
    if (a > 3.9) {
      return std::nullopt;  // at the outer face: partial volume
    }
    if (a > 2.6) {
      ++in_wall;
    } else if (a > 2.0) {
      return std::nullopt;
    }
  }
  return in_wall == 1 ? std::optional<double>(1.5) : std::nullopt;
}

class ThicknessMapTest : public ThicknessTest {
 protected:
  [[nodiscard]] ThicknessMapResult map(const ThicknessMapOptions& options) const {
    return writeThicknessMap(Dataset::open(dir_ / "scan.vsieve"),
                             SurfaceMask::open(dir_ / "surface.vss"), dir_ / "out", options);
  }

  /// Share of the voxels with an analytic local thickness within `tolerance` of it.
  [[nodiscard]] std::pair<double, std::size_t> matches(const PhantomSpec& spec,
                                                       double tolerance) const {
    const Dataset map = Dataset::open(dir_ / "out" / "thickness_map");
    Dataset::Sampler sample(map, 0);
    std::size_t known = 0;
    std::size_t matching = 0;
    for (std::int64_t z = 0; z < spec.dims[2]; ++z) {
      for (std::int64_t y = 0; y < spec.dims[1]; ++y) {
        for (std::int64_t x = 0; x < spec.dims[0]; ++x) {
          const auto expected = hollowBoxLocalThickness(spec, {x, y, z});
          if (!expected) {
            continue;
          }
          ++known;
          const auto t = sample({x, y, z});
          matching += t && std::abs(*t - *expected) <= tolerance ? 1 : 0;
        }
      }
    }
    return {known > 0 ? static_cast<double>(matching) / static_cast<double>(known) : 0.0, known};
  }
};

TEST_F(ThicknessMapTest, MapsTheWallsOfTheHollowBox) {
  const PhantomSpec spec = hollowBox(VoxelSize{0.1});
  scan(PhantomSource(spec));
  ThicknessMapOptions options;
  options.max_thickness_mm = 4.0;
  const ThicknessMapResult result = map(options);
  // The walls are 15 voxels: 1.5 mm and half a voxel.
  const auto [share, known] = matches(spec, 0.1);
  EXPECT_GT(known, 50000U);
  EXPECT_GT(share, 0.99);
  EXPECT_NEAR(result.material_volume_mm3, phantomMaterialVolumeMm3(spec), 15.0);
  // The edges of the box belong to the walls they join; the largest balls fit into its corners,
  // 1.5 sqrt(3) / (1 + sqrt(3)) mm in radius.
  EXPECT_NEAR(result.percentiles_mm[1], 1.55, 0.1);
  EXPECT_GT(result.min_mm, 1.3);
  EXPECT_NEAR(result.max_mm, 2.0 * 1.5 * std::sqrt(3.0) / (1.0 + std::sqrt(3.0)), 0.15);
  EXPECT_DOUBLE_EQ(result.capped, 0.0);
  double histogram = 0.0;
  for (const double h : result.histogram) {
    histogram += h;
  }
  EXPECT_NEAR(histogram, 1.0, 1e-9);
}

TEST_F(ThicknessMapTest, FollowsThePitchOfLongVoxels) {
  const PhantomSpec spec = hollowBox(VoxelSize{0.1, 0.1, 0.15});
  scan(PhantomSource(spec));
  ThicknessMapOptions options;
  options.max_thickness_mm = 4.0;
  (void)map(options);
  const auto [share, known] = matches(spec, 0.12);
  EXPECT_GT(known, 30000U);
  EXPECT_GT(share, 0.98);
}

TEST_F(ThicknessMapTest, CapsSolidMaterialAndFindsItsRegions) {
  // A solid cube of 8 mm with a pore near one face: thicker than the maximum except at its edges
  // and next to the pore.
  PhantomSpec spec = hollowBox(VoxelSize{0.1});
  spec.wall_thickness_mm = 0.0;
  spec.pores = {{{0.0, 0.0, 3.0}, 0.4}};  // 0.6 mm below the top face
  scan(PhantomSource(spec));
  ThicknessMapOptions options;
  options.max_thickness_mm = 3.0;
  options.min_wall_mm = 1.0;
  options.max_wall_mm = 2.5;
  const ThicknessMapResult result = map(options);
  EXPECT_DOUBLE_EQ(result.max_mm, 3.0);
  EXPECT_GT(result.capped, 0.8);
  EXPECT_GE(result.above_max, result.capped);
  // The material is one region thicker than the maximum.
  ASSERT_EQ(result.thick_regions.size(), 1U);
  EXPECT_NEAR(result.thick_regions.front().volume_mm3,
              result.above_max * result.material_volume_mm3, 1e-6);
  // Thin: the 0.6 mm of material above the pore.
  ASSERT_EQ(result.thin_regions.size(), 1U);
  const ThicknessMapRegion& thin = result.thin_regions.front();
  EXPECT_NEAR(thin.extreme_mm, 0.6, 0.1);
  EXPECT_NEAR(thin.location_mm[0], 5.0, 0.6);
  EXPECT_NEAR(thin.location_mm[1], 5.0, 0.6);
  EXPECT_NEAR(thin.location_mm[2], 8.6, 0.6);
  EXPECT_LT(thin.volume_mm3, 1.0);
}

TEST_F(ThicknessMapTest, WritesItsFiles) {
  scan(PhantomSource(hollowBox(VoxelSize{0.2})));
  ThicknessMapOptions options;
  options.min_wall_mm = 1.7;
  const ThicknessMapResult result = map(options);
  for (const char* file : {"thickness_map.json", "thickness_map_x.png", "thickness_map_y.png",
                           "thickness_map_z.png", "thickness_map/index.json"}) {
    EXPECT_TRUE(std::filesystem::exists(dir_ / "out" / file)) << file;
  }
  // The walls are thinner than the minimum, their edges and corners thicker.
  ASSERT_FALSE(result.thin_regions.empty());
  EXPECT_GT(result.below_min, 0.4);
  EXPECT_LT(result.below_min, 0.8);
  std::ifstream in(dir_ / "out" / "thickness_map.json");
  const nlohmann::json json = nlohmann::json::parse(in);
  EXPECT_EQ(json.at("thin_region_count"), result.thin_regions.size());
  EXPECT_NEAR(json.at("thickness").at("percentiles_mm").at("50").get<double>(), 1.5, 0.2);
  // Default maximum: the smallest extent of the part, from its bricks (here the volume).
  EXPECT_NEAR(result.max_thickness_mm, 10.0, 0.25);
  EXPECT_THROW((void)map(options), std::invalid_argument);  // the map exists already
}

TEST_F(ThicknessMapTest, TheReportShowsTheMapAndEvaluatesItsThickestSpot) {
  scan(PhantomSource(hollowBox(VoxelSize{0.2})));
  writeWallThickness(measure({}), dir_ / "out");
  ThicknessMapOptions options;
  options.min_wall_mm = 1.7;
  const ThicknessMapResult result = map(options);

  nlohmann::json data = nlohmann::json::object();
  addWallThickness(data, dir_ / "out", {{"wall_thickness", {{"min_mm", 1.7}, {"max_mm", 2.5}}}});
  const nlohmann::json& section = data.at("wall_thickness").at("map");
  EXPECT_FALSE(section.at("median").get<std::string>().empty());
  // The order's minimum is that of the analysis, so its regions are listed; the maximum is not.
  EXPECT_TRUE(section.at("thin").at("has_regions"));
  EXPECT_EQ(section.at("thin").at("regions").size(),
            std::min<std::size_t>(result.thin_regions.size(), 10));
  EXPECT_FALSE(section.at("thick").at("has_regions"));
  EXPECT_FALSE(data.at("images").at("thickness_map_z").get<std::string>().empty());
  const nlohmann::json& criteria = data.at("evaluation").at("zones").at(0).at("criteria");
  const auto thickest = std::find_if(criteria.begin(), criteria.end(), [](const auto& c) {
    return c.at("label") == "Thickest spot inside the part";
  });
  ASSERT_NE(thickest, criteria.end());
  EXPECT_TRUE(thickest->at("passed"));  // the corners reach about 1.9 mm

  // A maximum below the thickest spot fails it.
  data = nlohmann::json::object();
  addWallThickness(data, dir_ / "out", {{"wall_thickness", {{"max_mm", result.max_mm - 0.1}}}});
  EXPECT_FALSE(data.at("evaluation").at("zones").at(0).at("criteria").back().at("passed"));
}

TEST_F(ThicknessMapTest, RejectsInvalidOptions) {
  scan(PhantomSource(hollowBox(VoxelSize{0.2})));
  ThicknessMapOptions options;
  options.min_wall_mm = 2.0;
  options.max_wall_mm = 1.0;
  EXPECT_THROW((void)map(options), std::invalid_argument);
  options = {};
  options.max_thickness_mm = -1.0;
  EXPECT_THROW((void)map(options), std::invalid_argument);
}

TEST(ThicknessColorTest, MarksTheLimitsAndUnmeasuredPoints) {
  const auto red = thicknessColor(0.0, 0.0, 0.0, 4.0);
  const auto blue = thicknessColor(4.0, 0.0, 0.0, 4.0);
  EXPECT_GT(red[0], red[2]);
  EXPECT_GT(blue[2], blue[0]);
  EXPECT_EQ(thicknessColor(5.0, 0.0, 0.0, 4.0), blue);
  const auto below = thicknessColor(0.5, 1.0, 0.0, 4.0);
  EXPECT_NE(below, thicknessColor(0.5, 0.0, 0.0, 4.0));
  EXPECT_NE(thicknessColor(3.5, 1.0, 3.0, 4.0), thicknessColor(3.5, 1.0, 0.0, 4.0));
  const auto none = thicknessColor(std::nan(""), 1.0, 3.0, 4.0);
  EXPECT_EQ(none[0], none[1]);
  EXPECT_EQ(none[1], none[2]);
}

}  // namespace
}  // namespace voxelsieve
