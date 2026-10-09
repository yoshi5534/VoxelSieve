#include <gtest/gtest.h>
#include <openvdb/io/File.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "detail/blocks.hpp"
#include "voxelsieve/phantom.hpp"
#include "voxelsieve/sieve.hpp"

namespace voxelsieve {
namespace {

openvdb::Coord coord(std::int64_t x, std::int64_t y, std::int64_t z) {
  return {static_cast<openvdb::Int32>(x), static_cast<openvdb::Int32>(y),
          static_cast<openvdb::Int32>(z)};
}

PhantomSpec noisyPhantom() {
  PhantomSpec spec = defaultPhantomSpec();
  spec.noise_sigma = 500.0;
  return spec;
}

TEST(Sieve, OtsuThresholdSeparatesAirAndMaterial) {
  const PhantomSpec spec = noisyPhantom();
  const float threshold = estimateThreshold(generatePhantom(spec), 2);
  const auto midpoint = 0.5F * static_cast<float>(spec.air_value + spec.material_value);
  const auto contrast = static_cast<float>(spec.material_value - spec.air_value);
  EXPECT_NEAR(threshold, midpoint, 0.2F * contrast);
}

// Gaussian peak of `count` entries at `mean` with spread `sigma`, added to a grey histogram.
void addPeak(detail::Histogram& histogram, double mean, double sigma, double count) {
  for (std::size_t value = 0; value < histogram.size(); ++value) {
    const double z = (static_cast<double>(value) - mean) / sigma;
    histogram[value] += static_cast<std::uint64_t>(count * std::exp(-0.5 * z * z));
  }
}

TEST(Sieve, AirThresholdIsOtsuForAirAndOneMaterial) {
  detail::Histogram histogram(detail::kHistogramBins, 0);
  addPeak(histogram, 5000.0, 600.0, 1000.0);
  addPeak(histogram, 30000.0, 900.0, 500.0);
  EXPECT_EQ(detail::airThreshold(histogram).threshold, detail::otsuThreshold(histogram).threshold);
}

// A scan of several materials: little air, a light material (a plastic, organic fillings) and a
// lot of dense material. Otsu splits light from dense; the air threshold lies between air and
// the light material, so that is kept.
TEST(Sieve, AirThresholdSeparatesAirFromTheLightestMaterial) {
  detail::Histogram histogram(detail::kHistogramBins, 0);
  addPeak(histogram, 3000.0, 400.0, 3000.0);
  addPeak(histogram, 9000.0, 1500.0, 1500.0);
  addPeak(histogram, 30000.0, 3000.0, 4000.0);
  // Clipped or masked voxels at grey 0 are no material peak.
  histogram.front() += 10'000'000;
  EXPECT_GT(detail::otsuThreshold(histogram).threshold, 12000.0F);
  const detail::ThresholdResult air = detail::airThreshold(histogram);
  EXPECT_GT(air.threshold, 4200.0F);
  EXPECT_LT(air.threshold, 6500.0F);
  EXPECT_LT(air.air_level, 3000.0F);  // the clipped voxels count as air
}

TEST(Sieve, KeepsALightMaterialNextToDenseOne) {
  // Noisy air around a slab of light material that touches the volume boundary, and a larger
  // dense block. With Otsu's split between light and dense, the light slab would be removed as
  // air connected to the boundary.
  constexpr std::int64_t kSize = 64;
  Volume16 volume({kSize, kSize, kSize}, VoxelSize(1.0));
  std::mt19937 random(7);
  std::normal_distribution<double> noise(0.0, 300.0);
  for (std::int64_t z = 0; z < kSize; ++z) {
    for (std::int64_t y = 0; y < kSize; ++y) {
      for (std::int64_t x = 0; x < kSize; ++x) {
        double value = 3000.0;
        if (x >= 40) {
          value = 30000.0;
        } else if (x >= 16 && y < 16) {
          value = 9000.0;
        }
        volume.data[volume.index(x, y, z)] = static_cast<std::uint16_t>(value + noise(random));
      }
    }
  }
  const SieveResult result = sieve(volume);
  EXPECT_LT(result.stats.threshold, 9000.0F);
  EXPECT_GT(result.stats.threshold, 3000.0F);
  const auto accessor = result.grid->getConstAccessor();
  for (std::int64_t z = 0; z < kSize; ++z) {
    for (std::int64_t y = 0; y < 16; ++y) {
      for (std::int64_t x = 16; x < 40; ++x) {
        ASSERT_TRUE(accessor.isValueOn(coord(x, y, z))) << x << " " << y << " " << z;
      }
    }
  }
  // Air away from the part is still removed.
  EXPECT_FALSE(accessor.isValueOn(coord(0, kSize - 1, kSize / 2)));
}

TEST(Sieve, KeepsAllMaterialAndInternalAir) {
  const PhantomSpec spec = noisyPhantom();
  const Volume16 volume = generatePhantom(spec);
  const SieveResult result = sieve(volume);
  const auto accessor = result.grid->getConstAccessor();

  std::int64_t material = 0;
  std::int64_t internal_air = 0;
  for (std::int64_t z = 0; z < spec.dims[2]; ++z) {
    for (std::int64_t y = 0; y < spec.dims[1]; ++y) {
      for (std::int64_t x = 0; x < spec.dims[0]; ++x) {
        const auto center = voxelCenterMm(spec, x, y, z);
        const bool inside_box =
            std::abs(center[0]) < 4.0 && std::abs(center[1]) < 4.0 && std::abs(center[2]) < 4.0;
        if (!inside_box) {
          continue;
        }
        // Everything inside the outer box is material, cavity or pore; all of it must stay.
        const bool is_material = phantomSignedDistanceMm(spec, center) < 0.0;
        material += is_material ? 1 : 0;
        internal_air += is_material ? 0 : 1;
        ASSERT_TRUE(accessor.isValueOn(coord(x, y, z)))
            << "lost voxel " << x << " " << y << " " << z;
        ASSERT_EQ(accessor.getValue(coord(x, y, z)), static_cast<float>(volume.at(x, y, z)));
      }
    }
  }
  EXPECT_GT(material, 0);
  EXPECT_GT(internal_air, 0);
}

TEST(Sieve, RemovesOutsideAirButKeepsMargin) {
  const PhantomSpec spec = noisyPhantom();
  SieveOptions options;
  options.margin_voxels = 3;
  const SieveResult result = sieve(generatePhantom(spec), options);
  const auto accessor = result.grid->getConstAccessor();

  // Voxel indices along x for points on the centre line (y = z = centre).
  const auto index_at_mm = [&](double mm) {
    return static_cast<std::int64_t>(
        std::floor(mm / spec.voxel_size[0] + static_cast<double>(spec.dims[0]) / 2.0));
  };
  const std::int64_t mid = spec.dims[1] / 2;
  EXPECT_TRUE(accessor.isValueOn(coord(index_at_mm(4.0 + 0.25), mid, mid)));  // inside margin
  EXPECT_FALSE(accessor.isValueOn(coord(index_at_mm(4.0 + 2.0), mid, mid)));  // far outside
  EXPECT_FALSE(accessor.isValueOn(coord(0, 0, 0)));

  EXPECT_GT(result.stats.outside_air_block_count, 0);
  EXPECT_LT(result.stats.active_voxel_count, result.stats.voxel_count / 2);
  EXPECT_EQ(result.stats.voxel_count, spec.dims[0] * spec.dims[1] * spec.dims[2]);
}

TEST(Sieve, RemovesCavityThatIsOpenToTheOutside) {
  // Cube shell of material, 4 voxels thick, with the +z face missing: its interior is outside air.
  constexpr std::int64_t kN = 48;
  Volume16 volume({kN, kN, kN}, 0.1);
  for (std::int64_t z = 0; z < kN; ++z) {
    for (std::int64_t y = 0; y < kN; ++y) {
      for (std::int64_t x = 0; x < kN; ++x) {
        const bool in_outer = x >= 8 && x < 40 && y >= 8 && y < 40 && z >= 8;
        const bool in_inner = x >= 12 && x < 36 && y >= 12 && y < 36 && z >= 12;
        volume.at(x, y, z) = (in_outer && !in_inner) ? 20000 : 1000;
      }
    }
  }
  SieveOptions options;
  options.margin_voxels = 2;
  const SieveResult result = sieve(volume, options);
  const auto accessor = result.grid->getConstAccessor();
  EXPECT_TRUE(accessor.isValueOn(coord(10, 24, 24)));   // wall
  EXPECT_FALSE(accessor.isValueOn(coord(24, 24, 40)));  // open interior, far from the walls
  EXPECT_FALSE(accessor.isValueOn(coord(2, 2, 2)));     // outside
}

/// Square pipe along z through the whole volume, walls 4 voxels thick: the first and last slice
/// cut through it, so its inside is open towards them.
Volume16 pipeVolume() {
  constexpr std::int64_t kN = 48;
  Volume16 volume({kN, kN, kN}, 0.1);
  for (std::int64_t z = 0; z < kN; ++z) {
    for (std::int64_t y = 0; y < kN; ++y) {
      for (std::int64_t x = 0; x < kN; ++x) {
        const bool in_outer = x >= 8 && x < 40 && y >= 8 && y < 40;
        const bool in_inner = x >= 12 && x < 36 && y >= 12 && y < 36;
        volume.at(x, y, z) = (in_outer && !in_inner) ? 20000 : 1000;
      }
    }
  }
  return volume;
}

TEST(Sieve, KeepsTheInsideOfAPipeWhenAirEntersOnlyFromTheSides) {
  const Volume16 volume = pipeVolume();
  SieveOptions options;
  options.margin_voxels = 2;
  const SieveResult all = sieve(volume, options);
  const auto everywhere = all.grid->getConstAccessor();
  EXPECT_FALSE(everywhere.isValueOn(coord(24, 24, 24)));  // reached through the end slices

  options.outside_air_axes = parseAirAxes("xy");
  const SieveResult from_sides = sieve(volume, options);
  const auto sides = from_sides.grid->getConstAccessor();
  EXPECT_TRUE(sides.isValueOn(coord(24, 24, 24)));  // the inside stays
  EXPECT_TRUE(sides.isValueOn(coord(24, 24, 0)));
  EXPECT_TRUE(sides.isValueOn(coord(10, 24, 24)));  // wall
  EXPECT_FALSE(sides.isValueOn(coord(2, 2, 24)));   // outside the pipe
  EXPECT_FALSE(sides.isValueOn(coord(2, 2, 0)));    // also in the end slices
}

TEST(Sieve, ParsesAirAxes) {
  EXPECT_EQ(parseAirAxes("xy"), (AirAxes{true, true, false}));
  EXPECT_EQ(parseAirAxes("zx"), (AirAxes{true, false, true}));
  EXPECT_EQ(airAxesName(parseAirAxes("zyx")), "xyz");
  EXPECT_THROW((void)parseAirAxes(""), std::invalid_argument);
  EXPECT_THROW((void)parseAirAxes("xw"), std::invalid_argument);
}

TEST(Sieve, SolidVolumeWithoutAirKeepsEverything) {
  constexpr std::int64_t kVoxels = std::int64_t{20} * 20 * 20;
  Volume16 volume({20, 20, 20}, 0.2);
  for (std::int64_t i = 0; i < kVoxels; ++i) {
    volume.data[static_cast<std::size_t>(i)] = static_cast<std::uint16_t>(i % 2 == 0 ? 100 : 60000);
  }
  // Checkerboard-like data: every block holds material, so nothing is outside air.
  const SieveResult result = sieve(volume);
  EXPECT_EQ(result.stats.active_voxel_count, kVoxels);
  EXPECT_EQ(result.stats.outside_air_block_count, 0);
}

TEST(Sieve, StoresMetadataAndRoundTripsThroughFile) {
  PhantomSpec spec = noisyPhantom();
  spec.dims = {64, 64, 64};
  spec.voxel_size = 0.2;
  const SieveResult result = sieve(generatePhantom(spec));

  const auto path = std::filesystem::temp_directory_path() /
                    ("voxelsieve_sieve_" +
                     std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + ".vdb");
  writeVdb(path, {result.grid});

  openvdb::io::File file(path.string());
  file.open();
  const auto loaded = openvdb::gridPtrCast<openvdb::FloatGrid>(file.readGrid("density"));
  file.close();
  std::filesystem::remove(path);

  ASSERT_TRUE(loaded);
  EXPECT_EQ(loaded->activeVoxelCount(), result.grid->activeVoxelCount());
  EXPECT_DOUBLE_EQ(loaded->voxelSize()[0], 0.2);
  EXPECT_FLOAT_EQ(loaded->metaValue<float>("voxelsieve_threshold"), result.stats.threshold);
  EXPECT_EQ(loaded->metaValue<openvdb::Vec3i>("voxelsieve_source_dims"),
            openvdb::Vec3i(64, 64, 64));

  // A file that cannot be created is reported, not left out silently.
  EXPECT_THROW(writeVdb(path.parent_path() / "voxelsieve_no_such_dir" / "grid.vdb", {result.grid}),
               std::runtime_error);
}

TEST(Sieve, RejectsInvalidOptions) {
  const Volume16 volume({8, 8, 8}, 1.0);
  SieveOptions options;
  options.margin_voxels = -1;
  EXPECT_THROW((void)sieve(volume, options), std::invalid_argument);
  options.margin_voxels = 0;
  options.histogram_stride = 0;
  EXPECT_THROW((void)sieve(volume, options), std::invalid_argument);
}

}  // namespace
}  // namespace voxelsieve
