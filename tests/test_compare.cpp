#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <random>
#include <string>
#include <vector>

#include "voxelsieve/compare.hpp"
#include "voxelsieve/dataset.hpp"
#include "voxelsieve/mesh.hpp"
#include "voxelsieve/parts.hpp"
#include "voxelsieve/surface.hpp"
#include "voxelsieve/synthetic.hpp"

namespace voxelsieve {
namespace {

using Vec = std::array<double, 3>;

double distance(const Vec& a, const Vec& b) {
  return std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
}

Mesh transformed(const Mesh& mesh, const RigidTransform& t) {
  Mesh out;
  for (const auto& tri : mesh.triangles) {
    auto& o = out.triangles.emplace_back();
    for (std::size_t k = 0; k < 3; ++k) {
      const Vec p = t.apply({tri[k][0], tri[k][1], tri[k][2]});
      o[k] = {static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2])};
    }
  }
  return out;
}

TEST(RigidTransformTest, InverseAndCompositionAreConsistent) {
  const auto a = RigidTransform::fromAxisAngle({1.0, 2.0, 3.0}, 40.0, {30.0, -12.0, 7.0});
  const auto b = RigidTransform::fromAxisAngle({0.0, 0.0, 1.0}, -75.0, {1.0, 2.0, 3.0});
  const Vec p{3.0, -4.0, 5.0};
  const Vec back = a.inverse().apply(a.apply(p));
  EXPECT_LT(distance(back, p), 1e-12);
  EXPECT_LT(distance(b.after(a).apply(p), b.apply(a.apply(p))), 1e-12);
  EXPECT_NEAR(a.angleDegrees(), 40.0, 1e-9);
  const auto m = a.matrix();
  const auto c = RigidTransform::fromMatrix(m);
  EXPECT_LT(distance(c.apply(p), a.apply(p)), 1e-12);
  EXPECT_THROW((void)RigidTransform::fromMatrix(std::vector<double>(12, 0.0)),
               std::invalid_argument);
  const std::array<double, 12> mirror{-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
  EXPECT_THROW((void)RigidTransform::fromMatrix(mirror), std::invalid_argument);
  EXPECT_THROW((void)RigidTransform::fromMatrix(std::vector<double>(5, 0.0)),
               std::invalid_argument);
}

/// Signed distance to an axis-aligned box centred on the origin with half sizes `h`.
double boxDistance(const Vec& p, const Vec& h) {
  Vec q{};
  double outside = 0.0;
  double inside = -1e300;
  for (std::size_t k = 0; k < 3; ++k) {
    q[k] = std::abs(p[k]) - h[k];
    outside += std::max(q[k], 0.0) * std::max(q[k], 0.0);
    inside = std::max(inside, q[k]);
  }
  return std::sqrt(outside) + std::min(inside, 0.0);
}

TEST(MeshDistanceTest, IsExactForABoxInsideAndOutside) {
  const Vec h{10.0, 5.0, 3.0};
  const Mesh box = boxMesh({2 * h[0], 2 * h[1], 2 * h[2]});
  Mesh inverted = box;
  for (auto& t : inverted.triangles) {
    std::swap(t[1], t[2]);
  }
  const MeshDistance distance_to(box);
  const MeshDistance distance_to_inverted(inverted);
  std::mt19937 random(7);
  std::uniform_real_distribution<double> coordinate(-14.0, 14.0);
  for (int i = 0; i < 20000; ++i) {
    const Vec p{coordinate(random), coordinate(random), coordinate(random)};
    const double truth = boxDistance(p, h);
    const auto hit = distance_to.query(p);
    ASSERT_NEAR(hit.distance, truth, 1e-9) << p[0] << ' ' << p[1] << ' ' << p[2];
    ASSERT_NEAR(distance_to_inverted.query(p).distance, truth, 1e-9);
    // The closest point lies on the box and the normal points from it to p.
    EXPECT_NEAR(boxDistance(hit.closest, h), 0.0, 1e-9);
    const Vec expected{hit.closest[0] + hit.distance * hit.normal[0],
                       hit.closest[1] + hit.distance * hit.normal[1],
                       hit.closest[2] + hit.distance * hit.normal[2]};
    EXPECT_LT(distance(expected, p), 1e-9);
  }
  EXPECT_THROW(MeshDistance(Mesh{}), std::invalid_argument);
}

TEST(MeshDistanceTest, FollowsTheSamplePartSurfaces) {
  for (const auto& part : samplePartInfos()) {
    SamplePartOptions options;
    options.scale = 0.3;
    options.resolution_mm = 0.1;
    const MeshDistance distance_to(samplePartMesh(part.name, options));
    std::mt19937 random(3);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    int near = 0;
    for (int i = 0; i < 8000; ++i) {
      Vec p{};
      for (std::size_t k = 0; k < 3; ++k) {
        p[k] =
            0.3 * (part.bounds.min[k] + unit(random) * (part.bounds.max[k] - part.bounds.min[k]));
      }
      const double truth = samplePartDistance(part.name, p, 0.3);
      // The analytic distance of composed shapes is exact near the surface and a lower bound
      // of the magnitude elsewhere; the mesh follows the surface to a fraction of its resolution.
      const double measured = distance_to.query(p).distance;
      if (std::abs(truth) < 0.01) {
        ++near;
        ASSERT_LT(std::abs(measured), options.resolution_mm) << part.name;
      } else if (std::abs(truth) > 0.1) {
        ASSERT_EQ(measured > 0.0, truth > 0.0) << part.name;
        ASSERT_GT(std::abs(measured), std::abs(truth) - 0.05) << part.name;
      }
    }
    EXPECT_GT(near, 10) << part.name;
  }
}

class CompareTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("voxelsieve_compare_" +
            std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  static SyntheticSpec spec(double voxel_size) {
    SyntheticSpec spec;
    spec.voxel_size = voxel_size;
    spec.padding_mm = 4.0 * voxel_size;
    spec.noise_sigma = 400.0;
    spec.cupping = 0.1;
    spec.blur_sigma_mm = 0.5 * voxel_size;
    return spec;
  }

  /// Scans `mesh`, sieves the scan and extracts its surface.
  SurfaceMask surfaceOf(const SyntheticScan& scan) {
    std::filesystem::remove_all(dir_ / "scan.vsieve");
    DatasetOptions options;
    options.brick_size = 64;
    (void)writeDataset(scan, dir_ / "scan.vsieve", options);
    (void)writeSurface(Dataset::open(dir_ / "scan.vsieve"), dir_ / "surface.vss");
    return SurfaceMask::open(dir_ / "surface.vss");
  }

  /// CAD to scan for a scan of the untransformed mesh: scan coordinates are mesh coordinates
  /// minus the origin.
  static RigidTransform meshToScan(const SyntheticScan& scan) {
    const auto origin = scan.originMm();
    RigidTransform t;
    t.translation = {-origin[0], -origin[1], -origin[2]};
    return t;
  }

  std::filesystem::path dir_;
};

TEST_F(CompareTest, AlignsARotatedAndShiftedCadModel) {
  // The bracket is shaped much like the housing. The hub is symmetric but for the keyway in its
  // bore, which has to decide between five positions of the bolt circle.
  for (const std::string part : {"housing", "hub"}) {  // NOLINT(performance-for-range-copy)
    SamplePartOptions options;
    options.scale = 0.3;
    options.resolution_mm = 0.15;
    const SyntheticScan scan(samplePartMesh(part, options), spec(0.15));
    const SurfaceMask mask = surfaceOf(scan);
    options.resolution_mm = 0.05;
    const Mesh nominal = samplePartMesh(part, options);
    const auto moved = RigidTransform::fromAxisAngle({1.0, 2.0, 3.0}, 40.0, {30.0, -12.0, 7.0});
    const Mesh cad = transformed(nominal, moved);

    CompareOptions compare;
    compare.fit_points = 8000;
    const CompareResult result = compareToCad(mask, cad, compare);
    // Where the CAD model lands in the scan: the nominal mesh, placed as it was scanned.
    const RigidTransform truth = meshToScan(scan).after(moved.inverse());
    const double v = 0.15;
    double worst = 0.0;
    for (const auto& t : cad.triangles) {
      const Vec p{t[0][0], t[0][1], t[0][2]};
      worst = std::max(worst, distance(result.cad_to_scan.apply(p), truth.apply(p)));
    }
    EXPECT_LT(worst, 0.15 * v) << part;
    const DeviationStats& s = result.stats;
    // Most of the remaining deviation is real: the unsharpness rounds edges and fillets of this
    // small part, which is only a few voxels thick in places.
    EXPECT_LT(std::abs(s.mean_mm), 0.05 * v) << part;
    EXPECT_LT(s.rms_mm, 0.25 * v) << part;
    EXPECT_GT(s.within_tolerance, 0.97) << part;  // default tolerance 0.1 mm = 0.67 voxels
    EXPECT_LT(result.fit_rms_mm, 0.25 * v) << part;
    EXPECT_GT(result.fit_inliers, 0.9) << part;
    EXPECT_EQ(result.dropped_components, 0U) << part;
    EXPECT_EQ(result.deviation_mm.size(), result.mesh.points.size());
  }
}

TEST_F(CompareTest, MeasuresMaterialThatIsNotInTheCadModel) {
  // The scanned block is 0.4 mm higher than nominal at the top.
  const Mesh actual = boxMesh({20.0, 14.0, 10.4}, {0.0, 0.0, 0.2});
  const Mesh cad = boxMesh({20.0, 14.0, 10.0});
  const SyntheticScan scan(actual, spec(0.2));
  const SurfaceMask mask = surfaceOf(scan);
  CompareOptions options;
  options.alignment = CompareOptions::Alignment::kNone;
  options.initial = meshToScan(scan);
  const CompareResult result = compareToCad(mask, cad, options);

  const RigidTransform to_cad = result.cad_to_scan.inverse();
  double top = 0.0;
  double sides = 0.0;
  int top_count = 0;
  int side_count = 0;
  for (std::size_t i = 0; i < result.mesh.points.size(); ++i) {
    const auto& p = result.mesh.points[i];
    const Vec c = to_cad.apply({p[0], p[1], p[2]});
    if (c[2] > 5.3 && std::abs(c[0]) < 9.0 && std::abs(c[1]) < 6.0) {
      top += result.deviation_mm[i];
      ++top_count;
    } else if (std::abs(c[2]) < 4.0 && std::abs(std::abs(c[0]) - 10.0) < 0.2) {
      sides += result.deviation_mm[i];
      ++side_count;
    }
  }
  ASSERT_GT(top_count, 100);
  ASSERT_GT(side_count, 100);
  EXPECT_NEAR(top / top_count, 0.4, 0.02);
  EXPECT_NEAR(sides / side_count, 0.0, 0.02);
  // The top face is about 280 of 1267 mm^2; its edges are rounded by the unsharpness.
  EXPECT_NEAR(result.stats.above_tolerance, 280.0 / 1267.0, 0.03);
  EXPECT_NEAR(result.stats.max_mm, 0.4, 0.05);
  EXPECT_NEAR(result.stats.percentiles_mm[4], 0.4, 0.05);
  EXPECT_EQ(result.fit_iterations, 0);

  // Refinement from a slightly wrong start converges to a better fit than the start.
  options.alignment = CompareOptions::Alignment::kRefine;
  options.initial =
      meshToScan(scan).after(RigidTransform::fromAxisAngle({0.0, 1.0, 0.0}, 1.0, {0.3, 0.0, 0.0}));
  const CompareResult refined = compareToCad(mask, cad, options);
  EXPECT_GT(refined.fit_iterations, 1);
  EXPECT_LT(refined.fit_rms_mm, 0.05);
}

TEST_F(CompareTest, LeavesInternalPoresOutOfTheComparison) {
  const Mesh block = boxMesh({16.0, 12.0, 10.0});
  SyntheticSpec with_pores = spec(0.2);
  with_pores.lunker_count = 3;
  with_pores.lunker_radius_mm = 1.2;
  const SyntheticScan scan(block, with_pores);
  const SurfaceMask mask = surfaceOf(scan);
  CompareOptions options;
  options.alignment = CompareOptions::Alignment::kNone;
  options.initial = meshToScan(scan);
  const CompareResult outer = compareToCad(mask, block, options);
  EXPECT_GE(outer.components, 2U);
  EXPECT_EQ(outer.dropped_components, outer.components - 1);
  EXPECT_GT(outer.dropped_area_mm2, 1.0);
  EXPECT_GT(outer.stats.min_mm, -0.5);  // corners are rounded by the unsharpness

  options.outer_surface_only = false;
  const CompareResult all = compareToCad(mask, block, options);
  EXPECT_EQ(all.dropped_components, 0U);
  EXPECT_LT(all.stats.min_mm, -1.0);  // pore walls lie deep inside the nominal material
}

TEST_F(CompareTest, WritesAndReadsItsFiles) {
  const Mesh block = boxMesh({8.0, 6.0, 5.0});
  const SyntheticScan scan(block, spec(0.2));
  const SurfaceMask mask = surfaceOf(scan);
  const CompareResult result = compareToCad(mask, block);
  writeComparison(result, dir_ / "out");
  writeAlignedCad(result, block, dir_ / "out" / "cad_aligned.stl");
  for (const char* name : {"compare.json", "deviation.ply", "cad_aligned.stl",
                           "deviation_view_1.png", "deviation_view_2.png"}) {
    EXPECT_TRUE(std::filesystem::exists(dir_ / "out" / name)) << name;
  }
  const DeviationMesh read = readDeviationPly(dir_ / "out" / "deviation.ply");
  EXPECT_EQ(read.mesh.points, result.mesh.points);
  EXPECT_EQ(read.mesh.triangles, result.mesh.triangles);
  EXPECT_EQ(read.deviation_mm, result.deviation_mm);
  std::ifstream in(dir_ / "out" / "compare.json");
  const auto json = nlohmann::json::parse(in);
  EXPECT_EQ(json.at("alignment"), "auto");
  EXPECT_EQ(json.at("cad_to_scan").size(), 16U);
  EXPECT_EQ(json.at("deviation").at("histogram").size(), 40U);
  EXPECT_NEAR(json.at("deviation").at("mean_mm").get<double>(), result.stats.mean_mm, 1e-12);
  const Mesh aligned = readStl(dir_ / "out" / "cad_aligned.stl");
  EXPECT_EQ(aligned.triangles.size(), block.triangles.size());
  EXPECT_NEAR(meshVolumeMm3(aligned), meshVolumeMm3(block), 1e-3);

  std::ofstream(dir_ / "bad.ply") << "ply\nformat ascii 1.0\nend_header\n";
  EXPECT_THROW((void)readDeviationPly(dir_ / "bad.ply"), std::runtime_error);
}

TEST_F(CompareTest, RejectsInvalidOptions) {
  const Mesh block = boxMesh({8.0, 6.0, 5.0});
  const SyntheticScan scan(block, spec(0.2));
  const SurfaceMask mask = surfaceOf(scan);
  CompareOptions options;
  options.tolerance_mm = 0.0;
  EXPECT_THROW((void)compareToCad(mask, block, options), std::invalid_argument);
  options = {};
  options.fit_points = 10;
  EXPECT_THROW((void)compareToCad(mask, block, options), std::invalid_argument);
  EXPECT_THROW((void)compareToCad(mask, Mesh{}), std::invalid_argument);
  EXPECT_THROW((void)alignmentFromString("magic"), std::invalid_argument);
  EXPECT_EQ(alignmentFromString("refine"), CompareOptions::Alignment::kRefine);
}

TEST(DeviationColorTest, GreenInToleranceAndSaturatedBeyondTheRange) {
  const auto green = deviationColor(0.05, 0.1, 0.5);
  EXPECT_GT(green[1], green[0]);
  EXPECT_GT(green[1], green[2]);
  EXPECT_EQ(deviationColor(-0.1, 0.1, 0.5), green);
  EXPECT_EQ(deviationColor(0.5, 0.1, 0.5), deviationColor(3.0, 0.1, 0.5));
  const auto red = deviationColor(0.5, 0.1, 0.5);
  EXPECT_GT(red[0], 200);
  const auto blue = deviationColor(-0.5, 0.1, 0.5);
  EXPECT_GT(blue[2], 200);
}

}  // namespace
}  // namespace voxelsieve
