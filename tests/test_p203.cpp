#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <filesystem>
#include <memory>
#include <numbers>
#include <random>
#include <string>
#include <vector>

#include "voxelsieve/dataset.hpp"
#include "voxelsieve/mesh.hpp"
#include "voxelsieve/phantom.hpp"
#include "voxelsieve/porosity.hpp"
#include "voxelsieve/region.hpp"
#include "voxelsieve/report.hpp"
#include "voxelsieve/source.hpp"

namespace voxelsieve {
namespace {

using Json = nlohmann::json;

// A solid box of 14 x 10 x 8 mm (faces at x = +-7, y = +-5, z = +-4 around the volume centre)
// with four spherical pores: A deep inside, B 0.5 mm below the top face, and C1, C2 with 0.3 mm
// of material between them.
constexpr double kVoxel = 0.1;
constexpr Pore kPoreA{{-3.0, 0.0, 0.0}, 0.6};
constexpr Pore kPoreB{{3.0, 0.0, 3.1}, 0.4};
constexpr Pore kPoreC1{{0.0, 2.0, 0.0}, 0.5};
constexpr Pore kPoreC2{{0.0, 3.3, 0.0}, 0.5};

double sphereVolume(double radius) { return 4.0 / 3.0 * std::numbers::pi * std::pow(radius, 3); }

struct Shared {
  std::filesystem::path dir;
  PhantomSpec spec;
  PorosityResult result;
};
Shared* shared = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

class P203Test : public ::testing::Test {
 protected:
  // The scan and its analysis are shared by the tests of the suite; the directory is unique per
  // process, since ctest runs the tests in parallel processes.
  static void SetUpTestSuite() {
    shared = new Shared;
    shared->dir = std::filesystem::temp_directory_path() /
                  ("voxelsieve_p203_" + std::to_string(std::random_device{}()));
    std::filesystem::remove_all(shared->dir);
    PhantomSpec& spec = shared->spec;
    spec.dims = {160, 120, 100};
    spec.voxel_size = VoxelSize(kVoxel);
    spec.outer_size_mm = {14.0, 10.0, 8.0};
    spec.wall_thickness_mm = 0.0;
    spec.noise_sigma = 300.0;
    spec.pores = {kPoreA, kPoreB, kPoreC1, kPoreC2};
    DatasetOptions options;
    options.brick_size = 32;
    (void)writeDataset(PhantomSource(spec), shared->dir / "scan.vsieve", options);
    const Dataset dataset = Dataset::open(shared->dir / "scan.vsieve");
    shared->result = analyzePorosity(dataset);
  }
  static void TearDownTestSuite() {
    std::filesystem::remove_all(shared->dir);
    delete shared;
    shared = nullptr;
  }

  /// Phantom coordinates (around the volume centre) to dataset coordinates.
  static PointMm toDataset(const std::array<double, 3>& phantom_mm) {
    const auto origin = voxelCenterMm(shared->spec, 0, 0, 0);
    return {phantom_mm[0] - origin[0], phantom_mm[1] - origin[1], phantom_mm[2] - origin[2]};
  }

  /// The detected pore whose centre is closest to `pore`.
  static const DetectedPore& find(const Pore& pore) {
    const PointMm truth = toDataset(pore.center_mm);
    const DetectedPore* best = nullptr;
    double best_distance = 1e9;
    for (const DetectedPore& p : shared->result.pores) {
      const auto c = shared->result.voxel_size.toMm(p.center_voxels);
      const double d = std::hypot(c[0] - truth[0], c[1] - truth[1], c[2] - truth[2]);
      if (d < best_distance) {
        best_distance = d;
        best = &p;
      }
    }
    EXPECT_LT(best_distance, 0.1);
    return *best;
  }

  static InspectionZone zoneWithKey(const std::string& key) {
    InspectionZone zone;
    zone.name = key;
    zone.key = key;
    applyPorosityKey(key, zone.limits);
    return zone;
  }

  static const CriterionResult& criterion(const ZoneEvaluation& zone, const std::string& name) {
    for (const CriterionResult& c : zone.criteria) {
      if (c.name == name) {
        return c;
      }
    }
    ADD_FAILURE() << "no criterion " << name;
    return zone.criteria.front();
  }
};

TEST_F(P203Test, PoreCharacteristicsMatchTheSpheres) {
  ASSERT_EQ(shared->result.pores.size(), 4U);
  for (const Pore& truth : {kPoreA, kPoreB, kPoreC1, kPoreC2}) {
    const DetectedPore& pore = find(truth);
    EXPECT_NEAR(pore.enclosing_diameter_mm, 2.0 * truth.radius_mm, 1.5 * kVoxel);
    EXPECT_GT(pore.sphericity, 0.8);
    EXPECT_LE(pore.sphericity, 1.0);
    EXPECT_GT(pore.compactness(), 0.7);
    EXPECT_LT(pore.compactness(), 1.2);
    EXPECT_NEAR(pore.surface_area_mm2, 4.0 * std::numbers::pi * truth.radius_mm * truth.radius_mm,
                0.15 * 4.0 * std::numbers::pi * truth.radius_mm * truth.radius_mm);
  }
}

TEST_F(P203Test, SurfaceDistanceIsTheMaterialAboveThePore) {
  // B: 4 - 3.1 - 0.4 mm to the top face; A: 7 - 3 - 0.6 to the face at x = -7 and 4 - 0.6 to
  // the faces in z.
  EXPECT_NEAR(find(kPoreB).surface_distance_mm, 0.5, 1.5 * kVoxel);
  EXPECT_NEAR(find(kPoreA).surface_distance_mm, 3.4, 1.5 * kVoxel);
  EXPECT_NEAR(find(kPoreC2).surface_distance_mm, 5.0 - 3.3 - 0.5, 1.5 * kVoxel);
}

TEST_F(P203Test, GapIsTheMaterialBetweenNeighbouringPores) {
  std::vector<std::size_t> all(shared->result.pores.size());
  for (std::size_t i = 0; i < all.size(); ++i) {
    all[i] = i;
  }
  const PoreGap gap = smallestPoreGap(shared->result, all);
  EXPECT_NEAR(gap.gap_mm, 0.3, 1.5 * kVoxel);
  const int c1 = find(kPoreC1).id;
  const int c2 = find(kPoreC2).id;
  EXPECT_EQ(gap.first, std::min(c1, c2));
  EXPECT_EQ(gap.second, std::max(c1, c2));
}

TEST_F(P203Test, CharacteristicsSurviveSavingAndLoading) {
  savePorosityResult(shared->result, shared->dir / "saved");
  const PorosityResult loaded = loadPorosityResult(shared->dir / "saved");
  ASSERT_EQ(loaded.pores.size(), shared->result.pores.size());
  for (std::size_t i = 0; i < loaded.pores.size(); ++i) {
    EXPECT_DOUBLE_EQ(loaded.pores[i].enclosing_diameter_mm,
                     shared->result.pores[i].enclosing_diameter_mm);
    EXPECT_DOUBLE_EQ(loaded.pores[i].sphericity, shared->result.pores[i].sphericity);
    EXPECT_DOUBLE_EQ(loaded.pores[i].surface_distance_mm,
                     shared->result.pores[i].surface_distance_mm);
  }
}

TEST_F(P203Test, RegionsOfSolidsSelectPoresAndPartVolume) {
  // A sphere of 2 mm around pore A lies inside the part.
  const InspectionRegion sphere({RegionShape::sphere(toDataset(kPoreA.center_mm), 2.0)});
  EXPECT_NEAR(regionPartVolumeMm3(shared->result, sphere), sphereVolume(2.0),
              0.03 * sphereVolume(2.0));
  EXPECT_TRUE(sphere.contains(toDataset(kPoreA.center_mm)));
  EXPECT_FALSE(sphere.contains(toDataset(kPoreC1.center_mm)));

  // A box mesh around pore B sticks out of the top face at z = 4, by more than a block: the
  // material of a block counts by the share of the block in the region, so a region boundary
  // within a block at the surface would miss some.
  const auto b = toDataset(kPoreB.center_mm);
  writeStl(shared->dir / "roi.stl", boxMesh({2.0, 2.0, 3.0}, b));
  const InspectionRegion mesh({RegionShape::mesh(shared->dir / "roi.stl")});
  const double inside = 2.0 * 2.0 * (4.0 - (kPoreB.center_mm[2] - 1.5));
  EXPECT_NEAR(regionPartVolumeMm3(shared->result, mesh), inside, 0.05 * inside);
  // A region that ends right at the surface, as one from the CAD model would, needs the voxels.
  const Dataset dataset = Dataset::open(shared->dir / "scan.vsieve");
  const InspectionRegion flush(
      {RegionShape::box({toDataset({1.0, -2.0, 2.0}), toDataset({7.0, 2.0, 4.0})})});
  const double flush_volume = 6.0 * 4.0 * 2.0;
  EXPECT_LT(regionPartVolumeMm3(shared->result, flush), 0.97 * flush_volume);
  EXPECT_NEAR(regionPartVolumeMm3(shared->result, flush, &dataset), flush_volume,
              0.01 * flush_volume);
  // A cylinder along y through C1 and C2.
  const InspectionRegion cylinder(
      {RegionShape::cylinder(toDataset({0.0, 1.0, 0.0}), toDataset({0.0, 4.5, 0.0}), 1.0)});

  const InspectionZone around_a{"A", sphere, {}, {}};
  const InspectionZone around_b{"B", mesh, {}, {}};
  const InspectionZone around_c{"C", cylinder, {}, {}};
  const InspectionZone without_a{
      "rest",
      InspectionRegion({}, {RegionShape::sphere(toDataset(kPoreA.center_mm), 2.0)}),
      {},
      {}};
  const Evaluation evaluation = evaluate(shared->result, {around_a, around_b, around_c, without_a});
  EXPECT_EQ(evaluation.zones[0].pore_count, 1);
  EXPECT_EQ(evaluation.zones[1].pore_count, 1);
  EXPECT_EQ(evaluation.zones[2].pore_count, 2);
  EXPECT_EQ(evaluation.zones[3].pore_count, 3);
  EXPECT_NE(evaluation.zones[3].region.find("whole part without sphere"), std::string::npos);

  // The porosity of the sphere is the void of pore A over the sphere (PROI).
  const double truth = 100.0 * sphereVolume(kPoreA.radius_mm) / sphereVolume(2.0);
  EXPECT_NEAR(evaluation.zones[0].porosity_percent, truth, 0.06 * truth);
}

TEST_F(P203Test, ResultRestrictedToARegionKeepsOnlyItsPores) {
  // A cylinder along y through C1 and C2 keeps those two, removes A and B from the result and
  // from the pore labels, and the part volume becomes that of the cylinder.
  PorosityResult result = shared->result;
  result.pore_labels = shared->result.pore_labels->deepCopy();
  const InspectionRegion cylinder(
      {RegionShape::cylinder(toDataset({0.0, 1.0, 0.0}), toDataset({0.0, 4.5, 0.0}), 1.0)});
  const int removed = find(kPoreA).id;
  const int kept = find(kPoreC1).id;
  restrictToRegion(result, cylinder);
  ASSERT_EQ(result.pores.size(), 2U);
  for (const DetectedPore& pore : result.pores) {
    EXPECT_TRUE(cylinder.contains(result.voxel_size.toMm(pore.center_voxels)));
  }
  const double volume = std::numbers::pi * 1.0 * 1.0 * 3.5;
  EXPECT_NEAR(result.part_volume_mm3, volume, 0.05 * volume);
  const double voids = 2.0 * sphereVolume(kPoreC1.radius_mm);
  EXPECT_NEAR(result.porosity(), voids / volume, 0.1 * voids / volume);
  bool removed_left = false;
  bool kept_left = false;
  for (auto it = result.pore_labels->cbeginValueOn(); it; ++it) {
    removed_left = removed_left || *it == removed;
    kept_left = kept_left || *it == kept;
  }
  EXPECT_FALSE(removed_left);
  EXPECT_TRUE(kept_left);
  // The shared result is untouched.
  EXPECT_EQ(shared->result.pores.size(), 4U);
}

TEST_F(P203Test, RegionsAreReadFromTheOrder) {
  writeStl(shared->dir / "face.stl", boxMesh({1.0, 1.0, 1.0}));
  const Json order = Json::parse(R"({"acceptance": {"zones": [
    {"name": "R", "region": {
       "include": [{"box_mm": [[0, 0, 0], [1, 1, 1]]},
                   {"sphere": {"center_mm": [5, 5, 5], "radius_mm": 1}},
                   {"cylinder": {"from_mm": [0, 0, 5], "to_mm": [0, 0, 9], "radius_mm": 0.5}},
                   {"mesh": "face.stl", "pose": [1, 0, 0, 10, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]}],
       "exclude": [{"box_mm": [[0.4, 0.4, 0.4], [0.6, 0.6, 0.6]]}]},
     "porosity_key": "BDG P203 - PROI2/ Øaq3/ da3 /UØaq1"}]}})");
  const auto zones = inspectionZonesFromJson(order, shared->dir);
  ASSERT_EQ(zones.size(), 1U);
  const InspectionRegion& region = zones[0].region;
  EXPECT_EQ(region.include().size(), 4U);
  EXPECT_EQ(region.exclude().size(), 1U);
  EXPECT_TRUE(region.contains({0.2, 0.2, 0.2}));
  EXPECT_FALSE(region.contains({0.5, 0.5, 0.5}));  // excluded
  EXPECT_TRUE(region.contains({5.5, 5.0, 5.0}));
  EXPECT_TRUE(region.contains({0.0, 0.3, 8.0}));
  EXPECT_TRUE(region.contains({10.3, 0.0, 0.0}));  // the mesh moved by its pose
  EXPECT_FALSE(region.contains({20.0, 20.0, 20.0}));
  EXPECT_EQ(zones[0].limits.max_porosity_percent, 2.0);
  EXPECT_EQ(zones[0].limits.max_equivalent_diameter_mm, 3.0);
  EXPECT_EQ(zones[0].limits.min_surface_distance_mm, 3.0);
  EXPECT_EQ(zones[0].limits.disregard.equivalent_diameter_mm, 1.0);
  EXPECT_FALSE(region.mesh().triangles.empty());

  EXPECT_THROW((void)inspectionZonesFromJson(Json::parse(
                   R"({"acceptance": {"zones": [{"region": [{"mesh_object": "obj-1"}]}]}})")),
               std::invalid_argument);
  EXPECT_THROW((void)inspectionZonesFromJson(
                   Json::parse(R"({"acceptance": {"zones": [{"porosity_key": "PV7"}]}})")),
               std::invalid_argument);  // PV without a reference volume
}

TEST_F(P203Test, PorosityKeysOfTheGuidelineParse) {
  AcceptanceLimits a;
  applyPorosityKey("BDG P203 \xE2\x80\x93 PV7/ \xC3\x98p6/ U\xC3\x98p2", a);
  EXPECT_EQ(a.max_reference_porosity_percent, 7.0);
  EXPECT_EQ(a.max_enclosing_diameter_mm, 6.0);
  EXPECT_EQ(a.disregard.enclosing_diameter_mm, 2.0);

  AcceptanceLimits b;
  applyPorosityKey("BDG P203 - PG2/ PV7/ Dp5/ UPsi0,55/ UDp1,5", b);
  EXPECT_EQ(b.max_global_porosity_percent, 2.0);
  EXPECT_EQ(b.disregard.sphericity, 0.55);
  EXPECT_EQ(b.disregard.enclosing_diameter_mm, 1.5);

  AcceptanceLimits c;
  applyPorosityKey(
      "Vp50/ \xCE\xA8"
      "0,33/Gap0.8/Z3/C0.4/\xC3\x98"
      "4",
      c);
  EXPECT_EQ(c.max_pore_volume_mm3, 50.0);
  EXPECT_EQ(c.min_sphericity, 0.33);
  EXPECT_EQ(c.min_pore_gap_mm, 0.8);
  EXPECT_EQ(c.max_pore_count, 3);
  EXPECT_EQ(c.min_compactness, 0.4);
  EXPECT_EQ(c.max_enclosing_diameter_mm, 4.0);

  AcceptanceLimits d;
  EXPECT_THROW(applyPorosityKey("PX3", d), std::invalid_argument);
  EXPECT_THROW(applyPorosityKey("PG", d), std::invalid_argument);
  EXPECT_THROW(applyPorosityKey("UPG2", d), std::invalid_argument);
  EXPECT_THROW(applyPorosityKey("BDG P203 -", d), std::invalid_argument);
}

TEST_F(P203Test, KeyCriteriaFindTheWorstPore) {
  const ZoneEvaluation zone =
      evaluate(shared->result, {zoneWithKey("PG2/\xC3\x98p1.5/da0.8/Gap0.2/\xCE\xA8"
                                            "0.5")})
          .zones.front();
  const int b = find(kPoreB).id;
  const CriterionResult da = criterion(zone, "min_surface_distance_mm");
  EXPECT_FALSE(da.passed);
  EXPECT_NEAR(da.measured, 0.5, 1.5 * kVoxel);
  EXPECT_EQ(da.note, "pore " + std::to_string(b));
  EXPECT_TRUE(criterion(zone, "min_pore_gap_mm").passed);
  EXPECT_TRUE(criterion(zone, "max_enclosing_diameter_mm").passed);
  EXPECT_NEAR(criterion(zone, "max_enclosing_diameter_mm").measured, 1.2, 1.5 * kVoxel);
  EXPECT_TRUE(criterion(zone, "min_sphericity").passed);
  const double part = 14.0 * 10.0 * 8.0;
  const double voids = sphereVolume(kPoreA.radius_mm) + sphereVolume(kPoreB.radius_mm) +
                       2.0 * sphereVolume(kPoreC1.radius_mm);
  EXPECT_NEAR(criterion(zone, "max_global_porosity_percent").measured, 100.0 * voids / part,
              0.05 * 100.0 * voids / part);
  EXPECT_FALSE(zone.passed());

  const CriterionResult gap =
      criterion(evaluate(shared->result, {zoneWithKey("Gap0.5")}).zones.front(), "min_pore_gap_mm");
  EXPECT_FALSE(gap.passed);
  EXPECT_NE(gap.note.find("pores"), std::string::npos);
}

TEST_F(P203Test, DisregardedPoresCountForNothing) {
  // Pores up to 0.6 mm³ (B, C1, C2) are disregarded: only A is left, deep in the part.
  const ZoneEvaluation zone =
      evaluate(shared->result, {zoneWithKey("da0.8/Gap0.5/UVp0.6")}).zones.front();
  EXPECT_EQ(zone.disregarded_pores, 3);
  EXPECT_EQ(zone.pore_count, 1);
  EXPECT_TRUE(zone.passed());
  EXPECT_NEAR(criterion(zone, "min_surface_distance_mm").measured, 3.4, 1.5 * kVoxel);
  EXPECT_FALSE(std::isfinite(criterion(zone, "min_pore_gap_mm").measured));
}

TEST_F(P203Test, ReferenceVolumeIsPlacedWhereThePorosityIsHighest) {
  // Restricted to the left half of the part, the highest porosity is that of a reference volume
  // holding all of pore A.
  const InspectionRegion left(
      {RegionShape::box({toDataset({-8.0, -6.0, -5.0}), toDataset({-1.0, 6.0, 5.0})})});
  const std::vector<bool> all(shared->result.pores.size(), true);
  const double void_a = find(kPoreA).volume_mm3;

  ReferenceVolume cube;
  cube.size_mm = {2.0, 2.0, 2.0};
  const ReferencePlacement in_cube = maxReferencePorosity(shared->result, left, cube, all);
  ASSERT_TRUE(in_cube.found);
  EXPECT_NEAR(in_cube.volume_mm3, 8.0, 0.01);
  EXPECT_NEAR(in_cube.porosity_percent, 100.0 * void_a / 8.0, 0.02 * 100.0 * void_a / 8.0);
  const auto a = toDataset(kPoreA.center_mm);
  for (std::size_t i = 0; i < 3; ++i) {
    EXPECT_NEAR(in_cube.center_mm[i], a[i], 0.4 + 1e-9);
  }

  const ReferenceVolume sphere =
      ReferenceVolume::fromJson({{"shape", "sphere"}, {"diameter_mm", 2.0}});
  const ReferencePlacement in_sphere = maxReferencePorosity(shared->result, left, sphere, all);
  EXPECT_NEAR(in_sphere.volume_mm3, sphereVolume(1.0), 0.03 * sphereVolume(1.0));
  EXPECT_NEAR(in_sphere.porosity_percent, 100.0 * void_a / in_sphere.volume_mm3,
              0.02 * 100.0 * void_a / in_sphere.volume_mm3);

  // A reference volume larger than the part fits nowhere.
  ReferenceVolume huge;
  huge.size_mm = {12.0, 12.0, 12.0};
  EXPECT_FALSE(maxReferencePorosity(shared->result, InspectionRegion(), huge, all).found);

  // Through the order: PV with a cube, limited to the left half.
  InspectionZone zone = zoneWithKey("PV5");
  zone.region = left;
  zone.limits.reference_volume = cube;
  const CriterionResult pv =
      criterion(evaluate(shared->result, {zone}).zones.front(), "max_reference_porosity_percent");
  EXPECT_FALSE(pv.passed);  // about 11 %
  EXPECT_NE(pv.note.find("cube of 2 mm at"), std::string::npos);
}

TEST_F(P203Test, OldResultsWithoutCharacteristicsAreRejected) {
  PorosityResult old = shared->result;
  for (DetectedPore& pore : old.pores) {
    pore.enclosing_diameter_mm = std::numeric_limits<double>::quiet_NaN();
  }
  EXPECT_THROW((void)evaluate(old, {zoneWithKey("\xC3\x98p2")}), std::invalid_argument);
  EXPECT_NO_THROW((void)evaluate(old, {zoneWithKey("\xC3\x98"
                                                   "aq2")}));
}

}  // namespace
}  // namespace voxelsieve
