#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "voxelsieve/beam_hardening.hpp"
#include "voxelsieve/dataset.hpp"
#include "voxelsieve/mesh.hpp"
#include "voxelsieve/source.hpp"
#include "voxelsieve/synthetic.hpp"
#include "voxelsieve/volume.hpp"

namespace voxelsieve {
namespace {

constexpr double kPitch = 0.1;
constexpr std::int64_t kEdge = 96;
constexpr std::int64_t kSlices = 16;
constexpr double kRadiusMm = 4.0;
constexpr double kRodRadiusMm = 0.8;
constexpr double kRodOffsetMm = 2.0;
constexpr double kMaterialMu = 0.5;  // per mm
constexpr double kRodMu = 1.5;
constexpr double kAir = 2000.0;
/// Grey value per attenuation: material without beam hardening at 12000.
constexpr double kGreyPerMu = 10000.0 / kMaterialMu;

/// A slice across a cylinder of material (radius 4 mm) about the slice centre, optionally with
/// two dense rods 2 mm left and right of it.
std::vector<float> cylinderSlice(bool rods) {
  std::vector<float> image(static_cast<std::size_t>(kEdge * kEdge), 0.0F);
  const double c = 0.5 * static_cast<double>(kEdge - 1);
  for (std::int64_t y = 0; y < kEdge; ++y) {
    for (std::int64_t x = 0; x < kEdge; ++x) {
      const double mx = (static_cast<double>(x) - c) * kPitch;
      const double my = (static_cast<double>(y) - c) * kPitch;
      double mu = std::hypot(mx, my) <= kRadiusMm ? kMaterialMu : 0.0;
      if (rods && (std::hypot(mx - kRodOffsetMm, my) <= kRodRadiusMm ||
                   std::hypot(mx + kRodOffsetMm, my) <= kRodRadiusMm)) {
        mu = kRodMu;
      }
      image[static_cast<std::size_t>(x + kEdge * y)] = static_cast<float>(mu);
    }
  }
  return image;
}

/// Reconstruction of a slice scanned with a polychromatic beam: the measured line integral
/// saturates as (1 - exp(-beta p)) / beta instead of growing as p.
std::vector<float> scanSlice(const std::vector<float>& mu, double beta) {
  const ParallelBeam beam(kEdge, kEdge, kPitch, kPitch);
  std::vector<float> sinogram = beam.project(mu);
  if (beta > 0.0) {
    for (float& p : sinogram) {
      p = static_cast<float>((1.0 - std::exp(-beta * p)) / beta);
    }
  }
  return beam.reconstruct(sinogram);
}

/// The volume of `kSlices` equal slices, with the rotation axis along `axis`, and Gaussian noise
/// of `noise` grey values.
Volume16 volumeOf(const std::vector<float>& slice, int axis, double noise = 0.0) {
  std::mt19937 random(7);
  std::normal_distribution<double> gauss(0.0, 1.0);
  std::array<std::int64_t, 3> dims{kEdge, kEdge, kEdge};
  dims[static_cast<std::size_t>(axis)] = kSlices;
  Volume16 volume(dims, VoxelSize(kPitch));
  for (std::int64_t s = 0; s < kSlices; ++s) {
    for (std::int64_t v = 0; v < kEdge; ++v) {
      for (std::int64_t u = 0; u < kEdge; ++u) {
        std::array<std::int64_t, 3> p{};
        p[static_cast<std::size_t>(axis)] = s;
        p[axis == 0 ? 1U : 0U] = u;
        p[axis == 2 ? 1U : 2U] = v;
        const double grey = kAir + kGreyPerMu * slice[static_cast<std::size_t>(u + kEdge * v)] +
                            noise * gauss(random);
        volume.data[static_cast<std::size_t>(p[0] + dims[0] * (p[1] + dims[1] * p[2]))] =
            static_cast<std::uint16_t>(std::clamp(std::round(grey), 0.0, 65535.0));
      }
    }
  }
  return volume;
}

class BeamHardeningTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("voxelsieve_beam_hardening_" +
            std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  /// Sieves `volume` into a dataset of small bricks, so the correction crosses brick borders.
  Dataset sieve(const Volume16& volume, const std::string& name) {
    DatasetOptions options;
    options.brick_size = 32;
    options.threshold = static_cast<float>(kAir + 0.5 * (12000.0 - kAir));
    (void)writeDataset(MemorySource(volume), dir_ / name, options);
    return Dataset::open(dir_ / name);
  }

  /// Mean grey value of the voxels within `radius_mm` of the axis through the point (x, y) in mm
  /// from the slice centre, over all slices across the z axis.
  static double meanAround(const Dataset& dataset, double x_mm, double y_mm, double radius_mm) {
    const auto dims = dataset.info().dims;
    std::vector<float> values(static_cast<std::size_t>(dims[0] * dims[1] * dims[2]));
    dataset.readRegion(0, {{0, 0, 0}, dims}, values, static_cast<float>(kAir));
    const double c = 0.5 * static_cast<double>(kEdge - 1);
    double sum = 0.0;
    int count = 0;
    for (std::int64_t z = 0; z < dims[2]; ++z) {
      for (std::int64_t y = 0; y < dims[1]; ++y) {
        for (std::int64_t x = 0; x < dims[0]; ++x) {
          if (std::hypot((static_cast<double>(x) - c) * kPitch - x_mm,
                         (static_cast<double>(y) - c) * kPitch - y_mm) <= radius_mm) {
            sum += values[static_cast<std::size_t>(x + dims[0] * (y + dims[1] * z))];
            ++count;
          }
        }
      }
    }
    return sum / count;
  }

  /// Standard deviation of the difference between `dataset` and `reference` in the material of
  /// the cylinder away from its surface and the rods: what is left of beam hardening, plus the
  /// noise, but not a shift of the material's grey value.
  static double structureError(const Dataset& dataset, const Dataset& reference) {
    const auto dims = dataset.info().dims;
    const auto count = static_cast<std::size_t>(dims[0] * dims[1] * dims[2]);
    std::vector<float> values(count);
    std::vector<float> truth(count);
    dataset.readRegion(0, {{0, 0, 0}, dims}, values, static_cast<float>(kAir));
    reference.readRegion(0, {{0, 0, 0}, dims}, truth, static_cast<float>(kAir));
    const double c = 0.5 * static_cast<double>(kEdge - 1);
    double sum = 0.0;
    double sum_sq = 0.0;
    int n = 0;
    for (std::int64_t z = 0; z < dims[2]; ++z) {
      for (std::int64_t y = 0; y < dims[1]; ++y) {
        for (std::int64_t x = 0; x < dims[0]; ++x) {
          const double mx = (static_cast<double>(x) - c) * kPitch;
          const double my = (static_cast<double>(y) - c) * kPitch;
          if (std::hypot(mx, my) > kRadiusMm - 0.3 ||
              std::hypot(std::abs(mx) - kRodOffsetMm, my) < kRodRadiusMm + 0.3) {
            continue;
          }
          const auto i = static_cast<std::size_t>(x + dims[0] * (y + dims[1] * z));
          const double d = values[i] - truth[i];
          sum += d;
          sum_sq += d * d;
          ++n;
        }
      }
    }
    const double mean = sum / n;
    return std::sqrt(sum_sq / n - mean * mean);
  }

  std::filesystem::path dir_;
};

TEST(ParallelBeamTest, ReconstructsWhatItProjects) {
  // A disc of radius 3 mm in pixels of 0.1 x 0.15 mm.
  constexpr std::int64_t kNx = 80;
  constexpr std::int64_t kNy = 56;
  std::vector<float> image(static_cast<std::size_t>(kNx * kNy), 0.0F);
  const auto mm = [](std::int64_t i, std::int64_t n, double pitch) {
    return (static_cast<double>(i) - 0.5 * static_cast<double>(n - 1)) * pitch;
  };
  for (std::int64_t y = 0; y < kNy; ++y) {
    for (std::int64_t x = 0; x < kNx; ++x) {
      if (std::hypot(mm(x, kNx, 0.1), mm(y, kNy, 0.15)) <= 3.0) {
        image[static_cast<std::size_t>(x + kNx * y)] = 1.0F;
      }
    }
  }
  const ParallelBeam beam(kNx, kNy, 0.1, 0.15);
  EXPECT_DOUBLE_EQ(beam.binMm(), 0.1);
  EXPECT_EQ(beam.angles(), 126);  // pi / 2 * 80
  const std::vector<float> sinogram = beam.project(image);
  ASSERT_EQ(sinogram.size(), static_cast<std::size_t>(beam.angles() * beam.bins()));
  // Through the centre, every ray crosses the disc's diameter.
  for (int a = 0; a < beam.angles(); a += 21) {
    EXPECT_NEAR(sinogram[static_cast<std::size_t>(a * beam.bins() + beam.bins() / 2)], 6.0, 0.15);
  }
  const std::vector<float> back = beam.reconstruct(sinogram);
  double sum = 0.0;
  double sum_sq = 0.0;
  int count = 0;
  for (std::int64_t y = 0; y < kNy; ++y) {
    for (std::int64_t x = 0; x < kNx; ++x) {
      if (std::hypot(mm(x, kNx, 0.1), mm(y, kNy, 0.15)) <= 2.5) {
        const double v = back[static_cast<std::size_t>(x + kNx * y)];
        sum += v;
        sum_sq += v * v;
        ++count;
      } else if (std::hypot(mm(x, kNx, 0.1), mm(y, kNy, 0.15)) >= 3.5) {
        EXPECT_NEAR(back[static_cast<std::size_t>(x + kNx * y)], 0.0, 0.08);
      }
    }
  }
  const double mean = sum / count;
  EXPECT_NEAR(mean, 1.0, 0.02);
  EXPECT_LT(std::sqrt(sum_sq / count - mean * mean), 0.03);
  EXPECT_THROW((void)beam.project(std::vector<float>(10)), std::invalid_argument);
  EXPECT_THROW((void)beam.reconstruct(std::vector<float>(10)), std::invalid_argument);
  EXPECT_THROW(ParallelBeam(0, 4, 0.1, 0.1), std::invalid_argument);
}

TEST_F(BeamHardeningTest, ReprojectionRemovesCuppingAndStreaks) {
  const Dataset clean = sieve(volumeOf(scanSlice(cylinderSlice(true), 0.0), 2), "clean.vsieve");
  // Noise of a twentieth of the contrast, which the fit must not mistake for beam hardening.
  constexpr double kNoise = 500.0;
  const Dataset hard =
      sieve(volumeOf(scanSlice(cylinderSlice(true), 0.15), 2, kNoise), "hard.vsieve");
  // The streak between the rods against the material as far from the centre across them.
  const auto streak = [&](const Dataset& dataset) {
    const double across = meanAround(dataset, 0.0, 2.0, 0.3);
    return (across - meanAround(dataset, 0.0, 0.0, 0.3)) / (across - kAir);
  };
  const double clean_cupping = measureCupping(clean, 0).cupping;
  const CuppingProfile before = measureCupping(hard, 0);
  EXPECT_GT(before.cupping, clean_cupping + 0.08);
  EXPECT_GT(streak(hard), streak(clean) + 0.05);

  const BeamHardeningInfo info = correctBeamHardening(hard, dir_ / "corrected.vsieve");
  EXPECT_EQ(info.method, BeamHardeningMethod::kReprojection);
  EXPECT_EQ(info.level, 0);
  ASSERT_EQ(info.coefficients.size(), 2U);
  // Longer paths lost more and get more back.
  EXPECT_GT(info.after.level_grey.back() - info.before.level_grey.back(),
            info.after.level_grey.front() - info.before.level_grey.front() + 500.0);
  EXPECT_NEAR(info.before.cupping, before.cupping, 1e-9);
  // The cupping by depth, measured on noisy material, is close to that of the scan without
  // beam hardening; against that scan, less than 2 % of the contrast is left beside the noise.
  EXPECT_LT(std::abs(info.after.cupping - clean_cupping), 0.05);
  EXPECT_LT(info.material_spread_after, 0.8 * info.material_spread_before);
  const Dataset corrected = Dataset::open(dir_ / "corrected.vsieve");
  EXPECT_EQ(corrected.info().active_voxel_count, hard.info().active_voxel_count);
  EXPECT_EQ(corrected.info().value_type, hard.info().value_type);
  EXPECT_LT(std::abs(streak(corrected) - streak(clean)), 0.03);
  constexpr double kLeft = 0.02 * 10000.0;
  EXPECT_GT(structureError(hard, clean), 1.4 * kNoise);
  EXPECT_LT(structureError(corrected, clean), std::hypot(kNoise, kLeft));
}

TEST_F(BeamHardeningTest, CorrectionFromACoarseLevelIsInterpolated) {
  const Dataset hard = sieve(volumeOf(scanSlice(cylinderSlice(false), 0.15), 2), "hard.vsieve");
  BeamHardeningOptions options;
  options.resolution = 48;
  options.order = 2;
  const BeamHardeningInfo info = correctBeamHardening(hard, dir_ / "corrected.vsieve", options);
  EXPECT_EQ(info.level, 1);
  ASSERT_EQ(info.coefficients.size(), 1U);
  EXPECT_GT(info.coefficients[0], 0.0);
  EXPECT_GT(info.before.cupping, 0.08);
  EXPECT_LT(std::abs(info.after.cupping), 0.03);
  // Measured on level 0 too.
  // On level 0 too, below the outermost voxels of level 1, where the correction is
  // extrapolated (ADR 0033).
  const CuppingProfile fine = measureCupping(Dataset::open(dir_ / "corrected.vsieve"), 0);
  const double core = fine.level_grey.back();
  for (std::size_t b = 0; b < fine.depth_mm.size(); ++b) {
    if (fine.depth_mm[b] >= 2.0 * 2.0 * kPitch) {
      EXPECT_NEAR(fine.level_grey[b], core, 0.02 * (core - kAir)) << fine.depth_mm[b] << " mm";
    }
  }
}

TEST_F(BeamHardeningTest, RotationAxisSelectsTheSlices) {
  const Dataset hard = sieve(volumeOf(scanSlice(cylinderSlice(true), 0.15), 0), "hard.vsieve");
  BeamHardeningOptions options;
  options.rotation_axis = 0;
  const BeamHardeningInfo info = correctBeamHardening(hard, dir_ / "corrected.vsieve", options);
  EXPECT_GT(info.before.cupping, 0.08);
  EXPECT_LT(std::abs(info.after.cupping), 0.03);
  EXPECT_LT(info.material_spread_after, 0.4 * info.material_spread_before);
}

TEST_F(BeamHardeningTest, DepthMethodRemovesCuppingOfOneMaterial) {
  const Dataset hard = sieve(volumeOf(scanSlice(cylinderSlice(false), 0.15), 2), "hard.vsieve");
  BeamHardeningOptions options;
  options.method = BeamHardeningMethod::kDepth;
  options.output.value_type = ValueType::kFloat;
  const BeamHardeningInfo info = correctBeamHardening(hard, dir_ / "corrected.vsieve", options);
  EXPECT_EQ(info.method, BeamHardeningMethod::kDepth);
  EXPECT_TRUE(info.coefficients.empty());
  EXPECT_GT(info.before.cupping, 0.08);
  EXPECT_LT(std::abs(info.after.cupping), 0.02);
  EXPECT_LT(info.material_spread_after, 0.5 * info.material_spread_before);
  const Dataset corrected = Dataset::open(dir_ / "corrected.vsieve");
  EXPECT_EQ(corrected.info().value_type, ValueType::kFloat);
  // The material at the surface keeps its grey value; air inside stays air.
  EXPECT_NEAR(info.after.level_grey.front(), info.before.level_grey.front(),
              0.01 * (info.before.level_grey.front() - kAir));
}

TEST_F(BeamHardeningTest, DepthMethodCorrectsTheCuppingOfASyntheticScan) {
  // The synthetic scan darkens material by depth d below the surface as
  // 1 - cupping * (1 - exp(-d / depth)); the box of 6 x 5 x 4 mm is 2 mm deep at its core.
  SyntheticSpec spec;
  spec.cupping = 0.2;
  spec.cupping_depth_mm = 1.0;
  spec.noise_sigma = 300.0;
  const SyntheticScan scan(boxMesh({6.0, 5.0, 4.0}), spec);
  DatasetOptions sieve_options;
  sieve_options.brick_size = 32;
  (void)writeDataset(scan, dir_ / "scan.vsieve", sieve_options);
  const Dataset dataset = Dataset::open(dir_ / "scan.vsieve");
  const double contrast = spec.material_value - spec.air_value;
  const CuppingProfile profile = measureCupping(dataset, 0);
  ASSERT_GE(profile.depth_mm.size(), 10U);
  for (std::size_t b = 0; b < profile.depth_mm.size(); ++b) {
    const double truth =
        spec.material_value *
        (1.0 - spec.cupping * (1.0 - std::exp(-profile.depth_mm[b] / spec.cupping_depth_mm)));
    EXPECT_NEAR(profile.level_grey[b], truth, 0.03 * contrast) << profile.depth_mm[b] << " mm";
  }
  EXPECT_GT(profile.cupping, 0.1);

  BeamHardeningOptions options;
  options.method = BeamHardeningMethod::kDepth;
  const BeamHardeningInfo info = correctBeamHardening(dataset, dir_ / "corrected.vsieve", options);
  EXPECT_NEAR(info.before.cupping, profile.cupping, 1e-9);
  EXPECT_LT(std::abs(info.after.cupping), 0.02);
  for (const double level : info.after.level_grey) {
    EXPECT_NEAR(level, info.after.level_grey.front(), 0.02 * contrast);
  }
}

TEST_F(BeamHardeningTest, RejectsInvalidOptions) {
  const Dataset hard = sieve(volumeOf(scanSlice(cylinderSlice(false), 0.15), 2), "hard.vsieve");
  BeamHardeningOptions options;
  options.order = 5;
  EXPECT_THROW((void)correctBeamHardening(hard, dir_ / "a", options), std::invalid_argument);
  options.order = 3;
  options.rotation_axis = 3;
  EXPECT_THROW((void)correctBeamHardening(hard, dir_ / "b", options), std::invalid_argument);
  EXPECT_EQ(parseBeamHardeningMethod("depth"), BeamHardeningMethod::kDepth);
  EXPECT_EQ(beamHardeningMethodName(BeamHardeningMethod::kReprojection), "reprojection");
  EXPECT_THROW((void)parseBeamHardeningMethod("other"), std::invalid_argument);
}

}  // namespace
}  // namespace voxelsieve
