#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <string>
#include <vector>

#include "voxelsieve/mesh.hpp"
#include "voxelsieve/sieve.hpp"
#include "voxelsieve/source.hpp"
#include "voxelsieve/synthetic.hpp"

namespace voxelsieve {
namespace {

constexpr std::array<double, 3> kBoxSize{6.0, 5.0, 4.0};

std::vector<std::uint16_t> render(const SyntheticScan& scan) {
  const auto dims = scan.dims();
  std::vector<std::uint16_t> voxels(static_cast<std::size_t>(dims[0] * dims[1] * dims[2]));
  scan.readRegion({{0, 0, 0}, dims}, voxels);
  return voxels;
}

/// Material volume measured from the grey values through the partial-volume model.
double measuredMaterialMm3(const SyntheticScan& scan) {
  const SyntheticSpec& spec = scan.spec();
  const double contrast = static_cast<double>(spec.material_value) - spec.air_value;
  double fraction = 0.0;
  for (const std::uint16_t value : render(scan)) {
    fraction += (static_cast<double>(value) - spec.air_value) / contrast;
  }
  return fraction * spec.voxel_size.volumeMm3();
}

/// Distance from `p` to the nearest face of the box mesh, positive inside.
double boxDepth(const std::array<double, 3>& p) {
  double depth = 1e9;
  for (std::size_t i = 0; i < 3; ++i) {
    depth = std::min(depth, kBoxSize[i] / 2.0 - std::abs(p[i]));
  }
  return depth;
}

SyntheticSpec defectSpec() {
  SyntheticSpec spec;
  spec.lunker_count = 3;
  spec.lunker_radius_mm = 0.5;
  spec.loosening_count = 1;
  spec.loosening_radius_mm = 0.8;
  spec.loosening_porosity = 0.1;
  return spec;
}

TEST(Mesh, StlRoundTripAndVolume) {
  const Mesh box = boxMesh(kBoxSize, {1.0, 2.0, 3.0});
  EXPECT_EQ(box.triangles.size(), 12U);
  EXPECT_NEAR(meshVolumeMm3(box), 120.0, 1e-4);
  const Bounds bounds = meshBounds(box);
  EXPECT_DOUBLE_EQ(bounds.min[0], -2.0);
  EXPECT_DOUBLE_EQ(bounds.max[2], 5.0);

  const auto dir = std::filesystem::temp_directory_path();
  const auto binary = dir / "voxelsieve_box.stl";
  writeStl(binary, box);
  EXPECT_EQ(readStl(binary).triangles, box.triangles);

  const auto ascii = dir / "voxelsieve_box_ascii.stl";
  {
    std::ofstream out(ascii);
    out << "solid box\n";
    for (const auto& t : box.triangles) {
      out << "facet normal 0 0 0\nouter loop\n";
      for (const auto& v : t) {
        out << "vertex " << v[0] << ' ' << v[1] << ' ' << v[2] << '\n';
      }
      out << "endloop\nendfacet\n";
    }
    out << "endsolid box\n";
  }
  EXPECT_NEAR(meshVolumeMm3(readStl(ascii)), 120.0, 1e-3);

  std::filesystem::remove(binary);
  std::filesystem::remove(ascii);
  EXPECT_THROW((void)readStl(dir / "voxelsieve_missing.stl"), std::runtime_error);
}

TEST(Synthetic, PlainPartHasExactVolume) {
  const SyntheticScan scan(boxMesh(kBoxSize), SyntheticSpec{});
  EXPECT_EQ(scan.dims(), (std::array<std::int64_t, 3>{80, 70, 60}));  // 1 mm padding
  EXPECT_TRUE(scan.defects().empty());
  // The faces lie on voxel boundaries, so the partial-volume model is exact.
  EXPECT_NEAR(measuredMaterialMm3(scan), 120.0, 0.01);
}

TEST(Synthetic, DefectsMatchGroundTruth) {
  const SyntheticScan scan(boxMesh(kBoxSize), defectSpec());
  ASSERT_EQ(scan.defects().size(), 4U);
  EXPECT_TRUE(scan.warnings().empty());

  double void_volume = 0.0;
  for (const Defect& defect : scan.defects()) {
    for (const Sphere& sphere : defect.spheres) {
      EXPECT_GT(boxDepth(sphere.center_mm), sphere.radius_mm) << toString(defect.type);
      EXPECT_GT(scan.signedDistanceMm(sphere.center_mm), 0.0);  // void, not material
    }
    if (defect.type == DefectType::kLunker) {
      EXPECT_GT(defect.spheres.size(), 2U);
      // The union is smaller than the enclosing ball and larger than the core sphere.
      EXPECT_LT(defect.void_volume_mm3,
                4.0 / 3.0 * std::numbers::pi * std::pow(defect.radius_mm, 3));
      EXPECT_GT(defect.void_volume_mm3,
                4.0 / 3.0 * std::numbers::pi * std::pow(0.6 * defect.radius_mm, 3));
    } else {
      EXPECT_GT(defect.spheres.size(), 50U);
    }
    void_volume += defect.void_volume_mm3;
  }
  const auto json = scan.toJson();
  EXPECT_NEAR(json.at("ground_truth").at("void_volume_mm3").get<double>(), void_volume, 1e-9);

  // The grey values carry the void volume. Lunkers are supersampled (error below 2 %).
  const double measured_void = 120.0 - measuredMaterialMm3(scan);
  EXPECT_NEAR(measured_void, void_volume, 0.02 * void_volume);
}

TEST(Synthetic, SubVoxelPoresAreVolumeTrue) {
  // Loosening pores far below the voxel size only lower grey values; their exact overlap with
  // each voxel keeps the total void volume right.
  for (const double pore_voxels : {0.6, 0.3, 0.13}) {
    SyntheticSpec spec;
    spec.loosening_count = 2;
    spec.loosening_radius_mm = 0.8;
    spec.loosening_porosity = 0.1;
    spec.loosening_pore_radius_mm = pore_voxels * spec.voxel_size[0];
    const SyntheticScan scan(boxMesh(kBoxSize), spec);
    double void_volume = 0.0;
    for (const Defect& defect : scan.defects()) {
      void_volume += defect.void_volume_mm3;
    }
    ASSERT_GT(void_volume, 0.1);
    // Remaining error: rounding of the grey values to integers.
    EXPECT_NEAR(120.0 - measuredMaterialMm3(scan), void_volume, 0.002 * void_volume)
        << pore_voxels << " voxels";
  }
}

TEST(Synthetic, RegionsAreConsistentAndSeeded) {
  SyntheticSpec spec = defectSpec();
  spec.noise_sigma = 400.0;
  spec.ring_count = 4;
  spec.ring_strength = 300.0;
  spec.cupping = 0.1;
  const SyntheticScan scan(boxMesh(kBoxSize), spec);
  const auto full = render(scan);

  const Box box{{10, 20, 30}, {50, 41, 33}};
  std::vector<std::uint16_t> part(static_cast<std::size_t>(box.voxelCount()));
  scan.readRegion(box, part);
  const auto dims = scan.dims();
  std::size_t i = 0;
  for (std::int64_t z = box.min[2]; z < box.max[2]; ++z) {
    for (std::int64_t y = box.min[1]; y < box.max[1]; ++y) {
      for (std::int64_t x = box.min[0]; x < box.max[0]; ++x, ++i) {
        ASSERT_EQ(part[i], full[static_cast<std::size_t>(x + dims[0] * (y + dims[1] * z))]);
      }
    }
  }

  EXPECT_EQ(render(SyntheticScan(boxMesh(kBoxSize), spec)), full);
  spec.seed = 43;
  EXPECT_NE(render(SyntheticScan(boxMesh(kBoxSize), spec)), full);
}

TEST(Synthetic, ArtefactsChangeGreyValues) {
  SyntheticSpec spec;
  spec.cupping = 0.2;
  spec.cupping_depth_mm = 0.5;
  const SyntheticScan cupped(boxMesh(kBoxSize), spec);
  const auto voxels = render(cupped);
  const auto dims = cupped.dims();
  const auto at = [&](std::int64_t x, std::int64_t y, std::int64_t z) {
    return voxels[static_cast<std::size_t>(x + dims[0] * (y + dims[1] * z))];
  };
  // The centre of the part is darker than material just below the surface.
  EXPECT_LT(at(40, 35, 30), at(40, 35, 11));
  EXPECT_NEAR(at(40, 35, 30), 0.8 * spec.material_value, 0.02 * spec.material_value);

  // Rings disturb the air, which is otherwise exactly the air value without noise.
  EXPECT_EQ(at(1, 1, 1), spec.air_value);
  spec.cupping = 0.0;
  spec.ring_count = 6;
  spec.ring_strength = 500.0;
  const auto ringed = render(SyntheticScan(boxMesh(kBoxSize), spec));
  EXPECT_TRUE(std::any_of(ringed.begin(), ringed.end(), [&](std::uint16_t v) {
    return v != spec.air_value && v < spec.air_value + 2000;
  }));
}

TEST(Synthetic, BlurWidensEdgesAndKeepsTheVolume) {
  SyntheticSpec spec;
  spec.blur_sigma_mm = 0.08;  // 0.8 voxels
  const SyntheticScan blurred(boxMesh(kBoxSize), spec);
  // A Gaussian keeps the sum of the grey values, so the material volume stays exact.
  EXPECT_NEAR(measuredMaterialMm3(blurred), 120.0, 0.01);
  const auto voxels = render(blurred);
  const auto dims = blurred.dims();
  const auto at = [&](std::int64_t x) {
    return voxels[static_cast<std::size_t>(x + dims[0] * (35 + dims[1] * 30))];
  };
  // The face between x = 9 and 10 lies on a voxel boundary: sharp, the voxels on both sides are
  // pure air and material; blurred, the first material voxel loses the air share of the kernel.
  double air_share = 0.0;
  double total = 0.0;
  for (int k = -3; k <= 3; ++k) {
    const double w = std::exp(-0.5 * k * k / 0.64);
    total += w;
    air_share += k < 0 ? w : 0.0;
  }
  const double contrast = static_cast<double>(spec.material_value) - spec.air_value;
  const double fraction = (static_cast<double>(at(10)) - spec.air_value) / contrast;
  EXPECT_NEAR(fraction, 1.0 - air_share / total, 0.002);
  EXPECT_NEAR((static_cast<double>(at(9)) - spec.air_value) / contrast, 1.0 - fraction, 0.002);

  // Regions get the same values as the whole volume, also at the volume border.
  const Box box{{0, 3, 20}, {17, 9, 23}};
  std::vector<std::uint16_t> part(static_cast<std::size_t>(box.voxelCount()));
  blurred.readRegion(box, part);
  std::size_t i = 0;
  for (std::int64_t z = box.min[2]; z < box.max[2]; ++z) {
    for (std::int64_t y = box.min[1]; y < box.max[1]; ++y) {
      for (std::int64_t x = box.min[0]; x < box.max[0]; ++x, ++i) {
        ASSERT_EQ(part[i], voxels[static_cast<std::size_t>(x + dims[0] * (y + dims[1] * z))]);
      }
    }
  }
  spec.blur_sigma_mm = -1.0;
  EXPECT_THROW(SyntheticScan(boxMesh(kBoxSize), spec), std::invalid_argument);
}

TEST(Synthetic, SieveKeepsDefects) {
  SyntheticSpec spec = defectSpec();
  spec.noise_sigma = 300.0;
  const SyntheticScan scan(boxMesh(kBoxSize), spec);
  Volume16 volume(scan.dims(), scan.voxelSize());
  scan.readRegion({{0, 0, 0}, volume.dims}, volume.data);
  const SieveResult result = sieve(volume);
  const auto accessor = result.grid->getConstAccessor();
  const auto origin = scan.originMm();
  for (const Defect& defect : scan.defects()) {
    const auto& c = defect.center_mm;
    const openvdb::Coord voxel(
        static_cast<int>(std::lround((c[0] - origin[0]) / spec.voxel_size[0])),
        static_cast<int>(std::lround((c[1] - origin[1]) / spec.voxel_size[1])),
        static_cast<int>(std::lround((c[2] - origin[2]) / spec.voxel_size[2])));
    EXPECT_TRUE(accessor.isValueOn(voxel)) << toString(defect.type) << " at " << voxel;
  }
}

TEST(Synthetic, ThinPartsAndBadMeshes) {
  SyntheticSpec spec;
  spec.lunker_count = 2;
  spec.lunker_radius_mm = 0.5;
  const SyntheticScan thin(boxMesh({10.0, 10.0, 0.4}), spec);
  EXPECT_TRUE(thin.defects().empty());
  EXPECT_EQ(thin.warnings().size(), 1U);

  Mesh inverted = boxMesh(kBoxSize);
  for (auto& t : inverted.triangles) {
    std::swap(t[1], t[2]);
  }
  EXPECT_THROW(SyntheticScan(inverted, spec), std::invalid_argument);
  EXPECT_THROW(SyntheticScan(Mesh{}, spec), std::invalid_argument);
}

}  // namespace
}  // namespace voxelsieve
