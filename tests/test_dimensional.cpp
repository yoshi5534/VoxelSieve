#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <numbers>
#include <random>
#include <string>
#include <vector>

#include "voxelsieve/compare.hpp"
#include "voxelsieve/dataset.hpp"
#include "voxelsieve/dimensional.hpp"
#include "voxelsieve/mesh.hpp"
#include "voxelsieve/parts.hpp"
#include "voxelsieve/surface.hpp"
#include "voxelsieve/synthetic.hpp"

namespace voxelsieve {
namespace {

using Json = nlohmann::json;
using Vec = std::array<double, 3>;

double norm(const Vec& a) { return std::hypot(a[0], a[1], a[2]); }

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

TEST(FitElementTest, RecoversAnalyticElementsFromNoisyPoints) {
  std::mt19937 rng(7);
  std::normal_distribution<double> noise(0.0, 0.002);
  std::uniform_real_distribution<double> unit(0.0, 1.0);
  const Vec axis{0.36, 0.48, 0.8};  // unit
  const Vec u{0.8, -0.6, 0.0};
  const Vec w{0.48, 0.64, -0.6};  // axis x u
  const Vec center{3.0, -2.0, 5.0};
  std::vector<SurfacePoint> cylinder;
  std::vector<SurfacePoint> plane;
  std::vector<SurfacePoint> sphere;
  for (int i = 0; i < 4000; ++i) {
    const double phi = 2.0 * std::numbers::pi * unit(rng);
    const double h = -6.0 + 12.0 * unit(rng);
    const double r = 2.5 + noise(rng);
    const double a = 10.0 * unit(rng) - 5.0;
    const double b = 10.0 * unit(rng) - 5.0;
    const double off = noise(rng);
    Vec p{};
    Vec q{};
    for (std::size_t k = 0; k < 3; ++k) {
      p[k] = center[k] + h * axis[k] + r * (std::cos(phi) * u[k] + std::sin(phi) * w[k]);
      q[k] = center[k] + a * u[k] + b * w[k] + off * axis[k];
    }
    cylinder.push_back({p, {}});
    plane.push_back({q, axis});
    const double theta = std::acos(2.0 * unit(rng) - 1.0);
    const double rs = 4.0 + noise(rng);
    sphere.push_back(
        {{center[0] + rs * std::sin(theta) * std::cos(phi),
          center[1] + rs * std::sin(theta) * std::sin(phi), center[2] + rs * std::cos(theta)},
         {}});
  }
  GeometricElement start;
  start.type = ElementType::kCylinder;
  start.point = {3.2, -2.1, 5.3};
  start.direction = {0.3, 0.5, 0.81};
  start.radius = 2.3;
  const FittedElement c = fitElement(cylinder, start);
  EXPECT_NEAR(c.element.radius, 2.5, 5e-4);
  EXPECT_GT(std::abs(c.element.direction[0] * axis[0] + c.element.direction[1] * axis[1] +
                     c.element.direction[2] * axis[2]),
            1.0 - 1e-6);
  EXPECT_NEAR(c.element.length, 12.0, 0.1);
  EXPECT_LT(c.rms_mm, 0.0025);

  start.type = ElementType::kPlane;
  start.direction = {0.0, 0.0, 1.0};
  const FittedElement p = fitElement(plane, start);
  EXPECT_NEAR(p.element.direction[2], 0.8, 1e-4);
  EXPECT_LT(std::abs(elementDistance(p.element, center)), 2e-4);
  EXPECT_NEAR(p.formMm(), 0.016, 0.006);  // about 8 sigma of the noise

  start.type = ElementType::kSphere;
  start.point = {2.0, -1.0, 4.0};
  start.radius = 3.0;
  const FittedElement s = fitElement(sphere, start);
  EXPECT_NEAR(s.element.radius, 4.0, 5e-4);
  EXPECT_LT(norm({s.element.point[0] - center[0], s.element.point[1] - center[1],
                  s.element.point[2] - center[2]}),
            5e-4);
  EXPECT_THROW((void)fitElement(std::span<const SurfacePoint>(sphere.data(), 2), start),
               std::invalid_argument);
}

/// The hub (scale 0.5): a bore of diameter 4 along z, a rim of outer diameter 22 from z = -2 to
/// 3, the hub from z = -2 to 5 and five bolt holes of diameter 0.9 on a circle of radius 3.
constexpr double kScale = 0.5;

Mesh hubMesh(double resolution) {
  SamplePartOptions options;
  options.scale = kScale;
  options.resolution_mm = resolution;
  return samplePartMesh("hub", options);
}

Vec boltHole(int i) {
  const double angle = 2.0 * std::numbers::pi * (i + 0.5) / 5.0;
  return {3.0 * std::cos(angle), 3.0 * std::sin(angle), 1.5};
}

TEST(FeatureFinderTest, FindsThePlanesAndCylindersOfTheHub) {
  const FeatureFinder finder(hubMesh(0.08));
  const std::vector<CadFeature> features = finder.detect();
  ASSERT_FALSE(features.empty());
  const auto find = [&](ElementType type, double radius, bool inner) -> const CadFeature* {
    for (const CadFeature& f : features) {
      if (f.element.type == type && std::abs(f.element.radius - radius) < 0.02 &&
          f.element.inner == inner) {
        return &f;
      }
    }
    return nullptr;
  };
  const CadFeature* bore = find(ElementType::kCylinder, 2.0, true);
  ASSERT_NE(bore, nullptr);
  EXPECT_GT(std::abs(bore->element.direction[2]), 0.9999);
  EXPECT_LT(std::hypot(bore->element.point[0], bore->element.point[1]), 0.01);
  const CadFeature* rim = find(ElementType::kCylinder, 11.0, false);
  ASSERT_NE(rim, nullptr);
  int bolt_holes = 0;
  for (const CadFeature& f : features) {
    bolt_holes += f.element.type == ElementType::kCylinder && f.element.inner &&
                  std::abs(f.element.radius - 0.45) < 0.02;
  }
  EXPECT_EQ(bolt_holes, 5);
  // The top face of the rim, a ring at z = 3.
  bool rim_top = false;
  for (const CadFeature& f : features) {
    rim_top = rim_top || (f.element.type == ElementType::kPlane &&
                          std::abs(f.element.direction[2] - 1.0) < 1e-4 &&
                          std::abs(f.element.point[2] - 3.0) < 0.01);
  }
  EXPECT_TRUE(rim_top);

  // Probing at a feature's point finds it again; a cylinder where there is a plane fails.
  const auto probed = finder.probe(ElementType::kCylinder, bore->at);
  ASSERT_TRUE(probed.has_value());
  const CadFeature found = probed.value_or(CadFeature{});
  EXPECT_NEAR(found.element.radius, 2.0, 0.01);
  EXPECT_EQ(found.triangles.size(), bore->triangles.size());
  const auto probed_hole =
      finder.probe(ElementType::kCylinder, {boltHole(2)[0] * 1.15, boltHole(2)[1] * 1.15, 1.5});
  ASSERT_TRUE(probed_hole.has_value());
  const CadFeature hole = probed_hole.value_or(CadFeature{});
  EXPECT_NEAR(hole.element.radius, 0.45, 0.01);
  EXPECT_LT(
      std::hypot(hole.element.point[0] - boltHole(2)[0], hole.element.point[1] - boltHole(2)[1]),
      0.01);
  EXPECT_FALSE(finder.probe(ElementType::kCylinder, {0.0, 9.5, 3.0}).has_value());
  const auto probed_point = finder.probe(ElementType::kPoint, {0.0, 9.5, 3.2});
  ASSERT_TRUE(probed_point.has_value());
  const CadFeature point = probed_point.value_or(CadFeature{});
  EXPECT_NEAR(point.element.point[2], 3.0, 1e-3);
  EXPECT_NEAR(point.element.direction[2], 1.0, 1e-3);
}

class MeasureTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("voxelsieve_measure_" +
            std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  /// Scans `mesh`, sieves the scan and extracts its surface.
  SurfaceMask surfaceOf(const SyntheticScan& scan) {
    DatasetOptions options;
    options.brick_size = 64;
    (void)writeDataset(scan, dir_ / "scan.vsieve", options);
    (void)writeSurface(Dataset::open(dir_ / "scan.vsieve"), dir_ / "surface.vss");
    return SurfaceMask::open(dir_ / "surface.vss");
  }

  std::filesystem::path dir_;
};

const Json* characteristic(const Json& measurement, const std::string& number) {
  for (const Json& c : measurement.at("characteristics")) {
    if (c.at("number") == number) {
      return &c;
    }
  }
  return nullptr;
}

TEST_F(MeasureTest, MeasuresTheHubOnItsDatums) {
  constexpr double kVoxel = 0.125;
  // The part lies tilted in the scanner.
  const Mesh nominal = hubMesh(0.12);
  const auto placed = RigidTransform::fromAxisAngle({1.0, 2.0, 3.0}, 25.0, {4.0, -3.0, 2.0});
  SyntheticSpec spec;
  spec.voxel_size = kVoxel;
  spec.padding_mm = 4.0 * kVoxel;
  spec.noise_sigma = 400.0;
  spec.blur_sigma_mm = 0.5 * kVoxel;
  const SyntheticScan scan(transformed(nominal, placed), spec);
  const SurfaceMask mask = surfaceOf(scan);
  CompareOptions compare;
  compare.fit_points = 8000;
  const CompareResult compared = compareToCad(mask, nominal, compare);

  const Vec hole = boltHole(0);
  const Json plan = {
      {"elements",
       Json::array(
           {{{"id", "A"}, {"type", "plane"}, {"at", {0.0, 9.5, -2.0}}},
            {{"id", "B"}, {"type", "cylinder"}, {"at", {-2.0, 0.0, 2.5}}},
            {{"id", "C"}, {"type", "cylinder"}, {"at", {hole[0] * 1.15, hole[1] * 1.15, 1.5}}},
            {{"id", "top"}, {"type", "plane"}, {"at", {0.0, 9.5, 3.0}}},
            {{"id", "rim"}, {"type", "cylinder"}, {"at", {0.0, 11.0, 0.5}}},
            {{"id", "P"}, {"type", "point"}, {"at", {0.0, -10.0, 3.0}}}})},
      {"alignment", {{"datums", {"A", "B", "C"}}}},
      {"characteristics",
       Json::array(
           {{{"number", "1"}, {"type", "diameter"}, {"element", "B"}, {"tolerance", 0.05}},
            {{"number", "2"},
             {"type", "diameter"},
             {"element", "rim"},
             {"nominal", 22.0},
             {"upper", 0.1},
             {"lower", -0.1}},
            {{"number", "3"},
             {"type", "distance"},
             {"elements", {"A", "top"}},
             {"tolerance", 0.05}},
            {{"number", "4"}, {"type", "flatness"}, {"element", "top"}, {"tolerance", 0.05}},
            {{"number", "5"},
             {"type", "perpendicularity"},
             {"element", "B"},
             {"datum", "A"},
             {"tolerance", 0.05}},
            {{"number", "6"}, {"type", "position"}, {"element", "C"}, {"tolerance", 0.1}},
            {{"number", "7"},
             {"type", "coordinate"},
             {"element", "C"},
             {"axis", "x"},
             {"tolerance", 0.05}},
            {{"number", "8"},
             {"type", "parallelism"},
             {"element", "top"},
             {"datum", "A"},
             {"tolerance", 0.05}},
            {{"number", "9"}, {"type", "deviation"}, {"element", "P"}, {"tolerance", 0.05}},
            {{"number", "10"}, {"type", "profile"}, {"element", "top"}, {"tolerance", 0.1}},
            {{"number", "11"}, {"type", "angle"}, {"elements", {"A", "rim"}}, {"tolerance", 0.2}},
            // Deliberately too tight: the bore is 4 mm, not 3.9.
            {{"number", "12"},
             {"type", "diameter"},
             {"element", "B"},
             {"nominal", 3.9},
             {"tolerance", 0.02}},
            {{"number", "13"}, {"type", "cylindricity"}, {"element", "rim"}}})}};
  const Measurement m = measure(mask, nominal, compared.cad_to_scan, plan);
  const Json& j = m.json;
  for (const Json& c : j.at("characteristics")) {
    SCOPED_TRACE(c.dump());
    ASSERT_TRUE(c.contains("actual"));
  }
  EXPECT_EQ(j.at("alignment").at("fixed_degrees_of_freedom"), 6);
  // Sizes against the analytic geometry, within a tenth of a voxel.
  EXPECT_NEAR(characteristic(j, "1")->at("actual").get<double>(), 4.0, 0.1 * kVoxel);
  EXPECT_NEAR(characteristic(j, "1")->at("nominal").get<double>(), 4.0, 0.01);
  EXPECT_NEAR(characteristic(j, "2")->at("actual").get<double>(), 22.0, 0.1 * kVoxel);
  EXPECT_NEAR(characteristic(j, "3")->at("actual").get<double>(), 5.0, 0.1 * kVoxel);
  EXPECT_NEAR(characteristic(j, "3")->at("nominal").get<double>(), 5.0, 0.01);
  EXPECT_LT(characteristic(j, "4")->at("actual").get<double>(), 0.5 * kVoxel);
  EXPECT_LT(characteristic(j, "5")->at("actual").get<double>(), 0.2 * kVoxel);
  // On the datums, the bolt hole lies where the drawing has it.
  EXPECT_LT(characteristic(j, "6")->at("actual").get<double>(), 0.2 * kVoxel);
  EXPECT_NEAR(characteristic(j, "7")->at("actual").get<double>(), hole[0], 0.1 * kVoxel);
  EXPECT_LT(characteristic(j, "8")->at("actual").get<double>(), 0.5 * kVoxel);
  EXPECT_LT(std::abs(characteristic(j, "9")->at("actual").get<double>()), 0.1 * kVoxel);
  EXPECT_LT(characteristic(j, "10")->at("actual").get<double>(), 0.5 * kVoxel);
  EXPECT_NEAR(characteristic(j, "11")->at("actual").get<double>(), 90.0, 0.1);
  EXPECT_EQ(characteristic(j, "12")->at("result"), "NOK");
  EXPECT_EQ(characteristic(j, "13")->at("result"), "no tolerance");
  for (const char* number : {"1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11"}) {
    EXPECT_EQ(characteristic(j, number)->at("result"), "OK") << number;
  }
  EXPECT_EQ(m.passed, 11U);
  EXPECT_EQ(m.failed, 1U);
  EXPECT_EQ(m.unrated, 1U);
  EXPECT_FALSE(m.allPassed());
  // The scan lands where the drawing has the part.
  const RigidTransform truth = placed.inverse().after(RigidTransform{
      {1, 0, 0, 0, 1, 0, 0, 0, 1}, {scan.originMm()[0], scan.originMm()[1], scan.originMm()[2]}});
  double worst = 0.0;
  for (const Vec& p : {Vec{10.0, 0.0, 0.0}, Vec{0.0, 10.0, 3.0}, Vec{-5.0, -5.0, -2.0}}) {
    const Vec a = m.scan_to_cad.apply(p);
    const Vec b = truth.apply(p);
    worst = std::max(worst, norm({a[0] - b[0], a[1] - b[1], a[2] - b[2]}));
  }
  EXPECT_LT(worst, 0.1 * kVoxel);

  writeMeasurement(m, nominal, dir_ / "out");
  for (const char* file : {"measurement.json", "characteristics.csv", "measurement_view_1.png",
                           "measurement_view_2.png"}) {
    EXPECT_TRUE(std::filesystem::exists(dir_ / "out" / file)) << file;
  }
  const std::string csv = characteristicsCsv(j);
  EXPECT_EQ(std::count(csv.begin(), csv.end(), '\n'), 14);
  EXPECT_NE(csv.find("12,Diameter B,diameter,B,,3.9000,0.0200,-0.0200,"), std::string::npos) << csv;
}

TEST_F(MeasureTest, RejectsPlansThatAreNotWellFormed) {
  const Mesh nominal = hubMesh(0.25);
  SyntheticSpec spec;
  spec.voxel_size = 0.25;
  spec.padding_mm = 1.0;
  const SyntheticScan scan(nominal, spec);
  const SurfaceMask mask = surfaceOf(scan);
  RigidTransform cad_to_scan;
  cad_to_scan.translation = {-scan.originMm()[0], -scan.originMm()[1], -scan.originMm()[2]};
  const Json plane = {{"id", "A"}, {"type", "plane"}, {"at", {0.0, 9.5, -2.0}}};
  const auto fails = [&](const Json& plan, const std::string& message) {
    try {
      (void)measure(mask, nominal, cad_to_scan, plan);
      ADD_FAILURE() << "accepted " << plan.dump();
    } catch (const std::invalid_argument& error) {
      EXPECT_NE(std::string(error.what()).find(message), std::string::npos) << error.what();
    }
  };
  fails(Json::object(), "no elements");
  fails({{"elements", {{{"id", "A"}, {"type", "cone"}, {"at", {0, 0, 0}}}}}}, "Unknown element");
  fails({{"elements", {plane, plane}}}, "defined twice");
  fails({{"elements", {{{"id", "X"}, {"type", "cylinder"}, {"at", {0.0, 9.5, 3.0}}}}}},
        "no cylinder");
  fails({{"elements", {plane}}, {"characteristics", {{{"type", "diameter"}, {"element", "A"}}}}},
        "needs a cylinder");
  fails({{"elements", {plane}}, {"characteristics", {{{"type", "distance"}, {"element", "A"}}}}},
        "relates 2");
  fails({{"elements", {plane}}, {"characteristics", {{{"type", "flatness"}, {"element", "Z"}}}}},
        "no element 'Z'");
  fails({{"elements", {plane}}, {"alignment", "datums"}}, "alignment must be");
  fails({{"elements", {plane}},
         {"characteristics", {{{"type", "coordinate"}, {"element", "A"}, {"axis", "w"}}}}},
        "axis must be");
}

}  // namespace
}  // namespace voxelsieve
