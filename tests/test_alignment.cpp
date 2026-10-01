// Alignment of the objects of a project to each other and their comparison (ADR 0018).

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "voxelsieve/compare.hpp"
#include "voxelsieve/io.hpp"
#include "voxelsieve/mesh.hpp"
#include "voxelsieve/operation.hpp"
#include "voxelsieve/parts.hpp"
#include "voxelsieve/project.hpp"
#include "voxelsieve/synthetic.hpp"

namespace voxelsieve {
namespace {

using Json = nlohmann::json;
using Vec = std::array<double, 3>;

double distance(const Vec& a, const Vec& b) {
  return std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
}

std::vector<double> values(const RigidTransform& transform) {
  const auto matrix = transform.matrix();
  return {matrix.begin(), matrix.end()};
}

TEST(FitRigidTest, RecoversAKnownMotion) {
  const auto truth = RigidTransform::fromAxisAngle({0.3, -1.0, 2.0}, 117.0, {5.0, -3.0, 40.0});
  std::mt19937 random(7);
  std::uniform_real_distribution<double> uniform(-20.0, 20.0);
  std::vector<Vec> from(12);
  std::vector<Vec> to;
  for (Vec& p : from) {
    p = {uniform(random), uniform(random), uniform(random)};
    to.push_back(truth.apply(p));
  }
  const RigidFit fit = fitRigid(from, to);
  for (const Vec& p : from) {
    EXPECT_LT(distance(fit.transform.apply(p), truth.apply(p)), 1e-9);
  }
  EXPECT_LT(fit.rms_mm, 1e-9);
  ASSERT_EQ(fit.residuals_mm.size(), from.size());

  // Three points are enough; noise shows in the residuals.
  to[0][0] += 0.3;
  const RigidFit three = fitRigid(std::span(from).first(3), std::span(to).first(3));
  EXPECT_GT(three.rms_mm, 0.01);

  const std::vector<Vec> line{{0, 0, 0}, {1, 1, 1}, {2, 2, 2}};
  EXPECT_THROW((void)fitRigid(line, line), std::invalid_argument);
  EXPECT_THROW((void)fitRigid(std::span(from).first(2), std::span(to).first(2)),
               std::invalid_argument);
}

/// A scan of the sample housing and its CAD model, placed far from it, in one project.
class AlignmentTest : public ::testing::Test {
 protected:
  static constexpr double kVoxel = 0.15;

  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("voxelsieve_alignment_" +
            std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
    registerBuiltinOperations(registry_);

    SamplePartOptions part;
    part.scale = 0.3;
    part.resolution_mm = kVoxel;
    SyntheticSpec spec;
    spec.voxel_size = kVoxel;
    spec.padding_mm = 4.0 * kVoxel;
    spec.noise_sigma = 400.0;
    spec.blur_sigma_mm = 0.5 * kVoxel;
    const SyntheticScan scan(samplePartMesh("housing", part), spec);
    writeRaw(dir_ / "scan.raw", scan);
    writeJson(dir_ / "scan.json", scan.toJson());
    // Scan coordinates are mesh coordinates minus the origin of the scan.
    const auto origin = scan.originMm();
    mesh_to_scan_.translation = {-origin[0], -origin[1], -origin[2]};

    part.resolution_mm = 0.05;
    nominal_ = samplePartMesh("housing", part);
    cad_moved_ = RigidTransform::fromAxisAngle({1.0, 2.0, 3.0}, 40.0, {30.0, -12.0, 7.0});
    Mesh cad;
    for (const auto& t : nominal_.triangles) {
      auto& o = cad.triangles.emplace_back();
      for (std::size_t k = 0; k < 3; ++k) {
        const Vec p = cad_moved_.apply({t[k][0], t[k][1], t[k][2]});
        o[k] = {static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2])};
      }
    }
    writeStl(dir_ / "housing.stl", cad);
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  /// The worst distance over the CAD model between where `pose` and the truth put it.
  [[nodiscard]] double poseError(const RigidTransform& pose) const {
    const RigidTransform truth = mesh_to_scan_.after(cad_moved_.inverse());
    double worst = 0.0;
    for (const auto& t : nominal_.triangles) {
      const Vec p = cad_moved_.apply({t[0][0], t[0][1], t[0][2]});
      worst = std::max(worst, distance(pose.apply(p), truth.apply(p)));
    }
    return worst;
  }

  std::filesystem::path dir_;
  OperationRegistry registry_;
  Mesh nominal_;
  RigidTransform cad_moved_;
  RigidTransform mesh_to_scan_;
};

TEST_F(AlignmentTest, AlignsInSequenceAndComparesPlacedObjects) {
  auto project = Project::create(dir_ / "p", "Alignment");
  project.run(registry_, "import_raw",
              {{"path", (dir_ / "scan.raw").string()}, {"brick_size", 64}});
  project.run(registry_, "add_mesh", {{"path", (dir_ / "housing.stl").string()}});
  // Aligning a volume needs its surface.
  EXPECT_THROW(project.run(registry_, "align_surfaces", {{"objects", {"o2"}}, {"target", "o1"}}),
               std::invalid_argument);
  project.run(registry_, "surface", {}, {}, {}, nullptr, {.object = "o1", .name = {}});

  // Far apart: find the orientation from the principal axes first.
  const Step coarse = project.run(
      registry_, "align_surfaces",
      {{"objects", {"o2"}}, {"target", "o1"}, {"start", "principal_axes"}, {"fit_points", 8000}});
  EXPECT_LT(poseError(project.object("o2").pose), 0.15 * kVoxel);
  EXPECT_LT(coarse.summary.at("fit_rms_mm").get<double>(), 0.25 * kVoxel);
  EXPECT_NEAR(coarse.summary.at("resolution_mm").get<double>(), 0.5 * kVoxel, 1e-9);

  // Moved off a little, a fit from where it lies brings it back.
  const auto nudge = RigidTransform::fromAxisAngle({0.0, 1.0, 1.0}, 2.0, {0.3, -0.2, 0.1});
  project.run(registry_, "move",
              {{"objects", {"o2"}}, {"pose", values(nudge.after(project.object("o2").pose))}});
  EXPECT_GT(poseError(project.object("o2").pose), 2.0 * kVoxel);
  project.run(registry_, "align_surfaces",
              {{"objects", {"o2"}}, {"target", "o1"}, {"fit_points", 8000}});
  EXPECT_LT(poseError(project.object("o2").pose), 0.15 * kVoxel);
  EXPECT_EQ(project.object("o2").moved_by.size(), 3U);

  // The comparison measures where the objects lie: on the scan, against the CAD model.
  const Step compared = project.run(registry_, "compare_objects", {{"nominal", "o2"}}, {}, {},
                                    nullptr, {.object = "o1", .name = {}});
  EXPECT_EQ(compared.object, "o1");
  EXPECT_LT(std::abs(compared.summary.at("deviation_mean_mm").get<double>()), 0.05 * kVoxel);
  EXPECT_GT(compared.summary.at("within_tolerance_percent").get<double>(), 97.0);

  // Moving both together changes nothing between them.
  project.run(registry_, "move",
              {{"objects", {"o1", "o2"}},
               {"rotation_deg", 70.0},
               {"rotation_axis", {1.0, 0.0, 0.0}},
               {"translation_mm", {100.0, 0.0, 0.0}}});
  const Step again = project.run(registry_, "compare_objects", {{"nominal", "o2"}}, {}, {}, nullptr,
                                 {.object = "o1", .name = {}});
  EXPECT_NEAR(again.summary.at("deviation_mean_mm").get<double>(),
              compared.summary.at("deviation_mean_mm").get<double>(), 1e-4);
  EXPECT_THROW(project.run(registry_, "compare_objects", {{"nominal", "o1"}}, {}, {}, nullptr,
                           {.object = "o1", .name = {}}),
               std::invalid_argument);
}

TEST_F(AlignmentTest, PointPairsPlaceTheMovingObjectsOnTheTarget) {
  auto project = Project::create(dir_ / "p", "Alignment");
  project.run(registry_, "add_mesh", {{"path", (dir_ / "housing.stl").string()}});
  project.run(registry_, "add_mesh", {{"path", (dir_ / "housing.stl").string()}});
  project.run(registry_, "add_mesh", {{"path", (dir_ / "housing.stl").string()}});
  project.run(registry_, "move",
              {{"objects", {"o2", "o3"}},
               {"rotation_axis", {2.0, -1.0, 0.5}},
               {"rotation_deg", 75.0},
               {"translation_mm", {-8.0, 3.0, 12.0}}});
  project.run(registry_, "move", {{"objects", {"o3"}}, {"translation_mm", {0.0, 0.0, 5.0}}});
  const RigidTransform o2_before = project.object("o2").pose;
  const RigidTransform o3_before = project.object("o3").pose;

  // Pick corners of o2 where they are now and the same corners of o1.
  std::vector<Json> pairs;
  for (std::size_t i = 0; i < nominal_.triangles.size(); i += nominal_.triangles.size() / 5) {
    const auto& corner = nominal_.triangles[i][0];
    const Vec on_o1 = cad_moved_.apply({corner[0], corner[1], corner[2]});
    pairs.push_back({{"moving", project.object("o2").pose.apply(on_o1)}, {"target", on_o1}});
  }
  const Step aligned = project.run(registry_, "align_points",
                                   {{"objects", {"o2", "o3"}}, {"target", "o1"}, {"pairs", pairs}});
  EXPECT_LT(aligned.summary.at("rms_mm").get<double>(), 1e-4);
  EXPECT_EQ(aligned.summary.at("residuals_mm").size(), pairs.size());
  const Vec p{1.0, 2.0, 3.0};
  EXPECT_LT(distance(project.object("o2").pose.apply(p), p), 1e-4);
  // o3 moved with o2 by the same motion, so it keeps its place relative to o2.
  EXPECT_LT(
      distance(project.object("o3").pose.apply(p), o2_before.inverse().apply(o3_before.apply(p))),
      1e-4);

  EXPECT_THROW(project.run(registry_, "align_points",
                           {{"objects", {"o1"}}, {"target", "o1"}, {"pairs", pairs}}),
               std::invalid_argument);
  pairs.resize(2);
  EXPECT_THROW(project.run(registry_, "align_points", {{"objects", {"o2"}}, {"pairs", pairs}}),
               std::invalid_argument);
}

}  // namespace
}  // namespace voxelsieve
