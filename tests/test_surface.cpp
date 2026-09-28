#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "voxelsieve/dataset.hpp"
#include "voxelsieve/mesh.hpp"
#include "voxelsieve/parts.hpp"
#include "voxelsieve/surface.hpp"
#include "voxelsieve/synthetic.hpp"

namespace voxelsieve {
namespace {

TEST(SurfaceCodeTest, KeepsTheSignAndStaysWithinHalfAStep) {
  for (const int bits : {2, 3, 4, 6, 8}) {
    for (const double band : {0.5, 1.0, 2.0}) {
      const int max_code = (1 << bits) - 1;
      const double step = 2.0 * band / (max_code - 1);
      for (double d = -3.0; d <= 3.0; d += 0.001) {
        for (const bool inside : {true, false}) {
          const double signed_d = inside ? -std::abs(d) : std::abs(d);
          const std::uint8_t code = encodeSurfaceDistance(d, inside, bits, band);
          const float decoded = decodeSurfaceDistance(code, bits, band);
          ASSERT_LE(code, max_code);
          // The side of the surface is exact, also for voxels on it.
          ASSERT_EQ(decoded < 0.0F, inside) << bits << " " << band << " " << d;
          if (std::abs(d) >= band) {
            ASSERT_EQ(code, inside ? 0 : max_code);
          } else {
            ASSERT_LE(std::abs(decoded - signed_d), step / 2.0 + 1e-6) << bits << " " << d;
          }
        }
      }
    }
  }
}

TEST(SamplePartTest, MeshesAreClosedAndMatchTheirDistanceFunction) {
  ASSERT_EQ(samplePartInfos().size(), 3U);
  for (const SamplePartInfo& part : samplePartInfos()) {
    SamplePartOptions options;
    options.resolution_mm = 0.25;
    const Mesh mesh = samplePartMesh(part.name, options);
    const Bounds bounds = meshBounds(mesh);
    for (std::size_t i = 0; i < 3; ++i) {
      EXPECT_NEAR(bounds.min[i], part.bounds.min[i], 0.3) << part.name;
      EXPECT_NEAR(bounds.max[i], part.bounds.max[i], 0.3) << part.name;
    }
    // Monte Carlo volume of the distance function against the enclosed mesh volume.
    std::mt19937_64 random(7);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    constexpr int kSamples = 200000;
    int inside = 0;
    double box_volume = 1.0;
    for (std::size_t i = 0; i < 3; ++i) {
      box_volume *= part.bounds.max[i] - part.bounds.min[i];
    }
    for (int s = 0; s < kSamples; ++s) {
      std::array<double, 3> p{};
      for (std::size_t i = 0; i < 3; ++i) {
        p[i] = part.bounds.min[i] + unit(random) * (part.bounds.max[i] - part.bounds.min[i]);
      }
      inside += samplePartDistance(part.name, p) < 0.0 ? 1 : 0;
    }
    const double expected = box_volume * inside / kSamples;
    EXPECT_NEAR(meshVolumeMm3(mesh), expected, 0.02 * expected) << part.name;
    // Scaling scales the distances.
    EXPECT_NEAR(samplePartDistance(part.name, {1.0, 2.0, 3.0}, 2.0),
                2.0 * samplePartDistance(part.name, {0.5, 1.0, 1.5}), 1e-9);
  }
  EXPECT_THROW((void)samplePartMesh("teapot"), std::invalid_argument);
}

/// Accuracy of a surface file against the analytic ground truth of a synthetic scan.
struct Accuracy {
  double band_rms_voxels = 0.0;  // decoded minus true distance, voxels within the band
  double band_max_voxels = 0.0;
  double wrong_side = 0.0;         // fraction of voxels farther than 0.5 voxels on the wrong side
  double vertex_rms_voxels = 0.0;  // distance of mesh vertices to the true surface
  double vertex_p99_voxels = 0.0;
  double vertex_mean_voxels = 0.0;  // signed: positive when the surface lies too far out
  double volume_error = 0.0;        // relative
};

class SurfaceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("voxelsieve_surface_" +
            std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  /// Synthetic scan of a sample part with realistic grey values: noise, cupping and an
  /// unsharpness of half a voxel.
  static std::unique_ptr<SyntheticScan> scanOf(const std::string& part, double scale,
                                               double voxel_size_mm, int lunkers = 0,
                                               double padding_voxels = 4.0) {
    SamplePartOptions options;
    options.scale = scale;
    options.resolution_mm = voxel_size_mm;
    SyntheticSpec spec;
    spec.voxel_size_mm = voxel_size_mm;
    spec.padding_mm = padding_voxels * voxel_size_mm;
    spec.noise_sigma = 400.0;
    spec.cupping = 0.1;
    spec.blur_sigma_mm = 0.5 * voxel_size_mm;
    spec.lunker_count = lunkers;
    spec.lunker_radius_mm = 4.0 * voxel_size_mm;
    return std::make_unique<SyntheticScan>(samplePartMesh(part, options), spec);
  }

  /// Sieves `scan` into a dataset directory `name` and returns it opened.
  Dataset sieve(const SyntheticScan& scan, const std::string& name, std::int64_t brick_size = 64) {
    DatasetOptions options;
    options.brick_size = brick_size;
    (void)writeDataset(scan, dir_ / name, options);
    return Dataset::open(dir_ / name);
  }

  static Accuracy measure(const SyntheticScan& scan, const SurfaceMask& mask) {
    const SurfaceInfo& info = mask.info();
    const double v = info.voxel_size_mm;
    const auto origin = scan.originMm();
    const auto dims = info.dims;
    Accuracy result;
    double sum = 0.0;
    std::int64_t band = 0;
    std::int64_t wrong = 0;
    std::int64_t counted = 0;
    std::vector<std::uint8_t> codes(static_cast<std::size_t>(dims[0] * dims[1] * dims[2]));
    mask.readCodes({{0, 0, 0}, dims}, codes);
    std::size_t offset = 0;
    for (std::int64_t z = 0; z < dims[2]; ++z) {
      for (std::int64_t y = 0; y < dims[1]; ++y) {
        for (std::int64_t x = 0; x < dims[0]; ++x) {
          const std::uint8_t code = codes[offset++];
          const float decoded = decodeSurfaceDistance(code, info.bits, info.band_voxels);
          const bool far = code == 0 || code == info.maxCode();
          // The ground truth is expensive; sample the far voxels sparsely.
          if (far && (x + y + z) % 5 != 0) {
            continue;
          }
          const double truth = scan.signedDistanceMm({origin[0] + static_cast<double>(x) * v,
                                                      origin[1] + static_cast<double>(y) * v,
                                                      origin[2] + static_cast<double>(z) * v}) /
                               v;
          ++counted;
          if ((truth < -0.5 && decoded > 0.0F) || (truth > 0.5 && decoded < 0.0F)) {
            ++wrong;
          }
          if (std::abs(truth) < info.band_voxels) {
            const double error = decoded - std::clamp(truth, -info.band_voxels, info.band_voxels);
            sum += error * error;
            result.band_max_voxels = std::max(result.band_max_voxels, std::abs(error));
            ++band;
          }
        }
      }
    }
    result.band_rms_voxels = std::sqrt(sum / static_cast<double>(std::max<std::int64_t>(band, 1)));
    result.wrong_side = static_cast<double>(wrong) / static_cast<double>(counted);

    // Mesh vertices lie in the dataset's world space, voxel index times voxel size.
    const Mesh mesh = mask.toMesh();
    std::vector<double> deviations;
    double signed_sum = 0.0;
    for (std::size_t t = 0; t < mesh.triangles.size(); t += 3) {
      const auto& p = mesh.triangles[t][0];
      const double d =
          scan.signedDistanceMm({origin[0] + p[0], origin[1] + p[1], origin[2] + p[2]}) / v;
      signed_sum += d;
      deviations.push_back(std::abs(d));
    }
    double squares = 0.0;
    for (const double d : deviations) {
      squares += d * d;
    }
    result.vertex_rms_voxels = std::sqrt(squares / static_cast<double>(deviations.size()));
    result.vertex_mean_voxels = signed_sum / static_cast<double>(deviations.size());
    std::sort(deviations.begin(), deviations.end());
    result.vertex_p99_voxels = deviations[deviations.size() * 99 / 100];

    const auto truth = scan.toJson().at("ground_truth").at("material_volume_mm3").get<double>();
    result.volume_error = (info.volume_mm3 - truth) / truth;
    return result;
  }

  static void print(const std::string& name, const SurfaceInfo& info, const Accuracy& a) {
    const auto voxels = static_cast<double>(info.dims[0] * info.dims[1] * info.dims[2]);
    const auto bits = 8.0 * static_cast<double>(info.file_bytes);
    std::cout << name << ": " << info.dims[0] << "x" << info.dims[1] << "x" << info.dims[2]
              << " voxels, " << info.band_voxel_count << " in the band, " << info.file_bytes
              << " bytes = " << bits / voxels << " bit/voxel, "
              << bits / static_cast<double>(info.band_voxel_count) << " bit/band voxel\n"
              << "  distance rms " << a.band_rms_voxels << " max " << a.band_max_voxels
              << " | surface rms " << a.vertex_rms_voxels << " p99 " << a.vertex_p99_voxels
              << " mean " << a.vertex_mean_voxels << " | volume " << 100.0 * a.volume_error
              << " %\n";
  }

  std::filesystem::path dir_;
};

TEST_F(SurfaceTest, SamplePartsAreLocatedWithSubVoxelAccuracy) {
  for (const SamplePartInfo& part : samplePartInfos()) {
    const auto scan = scanOf(part.name, 0.3, 0.15, part.name == "housing" ? 3 : 0);
    const Dataset dataset = sieve(*scan, part.name + ".vsieve");
    const SurfaceInfo info = writeSurface(dataset, dir_ / (part.name + ".vss"));
    const SurfaceMask mask = SurfaceMask::open(dir_ / (part.name + ".vss"));
    EXPECT_EQ(mask.info().surface_blocks, info.surface_blocks);
    EXPECT_EQ(mask.info().file_bytes, info.file_bytes);
    const Accuracy a = measure(*scan, mask);
    print(part.name, info, a);
    // Distances within the band to a tenth of a voxel, the surface itself better than that.
    EXPECT_LT(a.band_rms_voxels, 0.12) << part.name;
    EXPECT_LT(a.vertex_rms_voxels, 0.1) << part.name;
    EXPECT_LT(a.vertex_p99_voxels, 0.3) << part.name;
    EXPECT_LT(std::abs(a.vertex_mean_voxels), 0.03) << part.name;
    EXPECT_EQ(a.wrong_side, 0.0) << part.name;
    EXPECT_NEAR(a.volume_error, 0.0, 0.01) << part.name;
    // Far below the four bits the band voxels would take uncompressed.
    EXPECT_LT(
        8.0 * static_cast<double>(info.file_bytes) / static_cast<double>(info.band_voxel_count),
        4.0)
        << part.name;
  }
}

TEST_F(SurfaceTest, MoreBitsGiveFinerDistances) {
  const auto scan = scanOf("bracket", 0.3, 0.15);
  const Dataset dataset = sieve(*scan, "scan.vsieve");
  double previous = 1.0;
  for (const int bits : {2, 3, 4, 6}) {
    SurfaceOptions options;
    options.bits = bits;
    const auto file = dir_ / ("bits" + std::to_string(bits) + ".vss");
    (void)writeSurface(dataset, file, options);
    const Accuracy a = measure(*scan, SurfaceMask::open(file));
    EXPECT_LT(a.band_rms_voxels, previous) << bits;
    EXPECT_EQ(a.wrong_side, 0.0) << bits;
    previous = a.band_rms_voxels;
  }
}

TEST_F(SurfaceTest, ChunksAreIndependentOfTheBrickSize) {
  const auto scan = scanOf("hub", 0.25, 0.15);
  const Dataset small = sieve(*scan, "small.vsieve", 32);
  const Dataset large = sieve(*scan, "large.vsieve", 64);
  (void)writeSurface(small, dir_ / "small.vss");
  (void)writeSurface(large, dir_ / "large.vss");
  const SurfaceMask a = SurfaceMask::open(dir_ / "small.vss");
  const SurfaceMask b = SurfaceMask::open(dir_ / "large.vss");
  ASSERT_EQ(a.info().dims, b.info().dims);
  EXPECT_EQ(a.info().chunk_size, 32);
  const auto dims = a.info().dims;
  const Box all{{0, 0, 0}, dims};
  std::vector<std::uint8_t> codes_a(static_cast<std::size_t>(all.voxelCount()));
  std::vector<std::uint8_t> codes_b(codes_a.size());
  a.readCodes(all, codes_a);
  b.readCodes(all, codes_b);
  // The halo around each chunk holds the whole triangulation that reaches into the band, so the
  // split does not matter; float rounding may move a distance across a step boundary.
  std::int64_t differences = 0;
  for (std::size_t i = 0; i < codes_a.size(); ++i) {
    if (codes_a[i] != codes_b[i]) {
      ASSERT_LE(std::abs(codes_a[i] - codes_b[i]), 1) << i;
      ++differences;
    }
  }
  EXPECT_LT(differences, a.info().band_voxel_count / 1000 + 1);

  // Single voxels, a sub-box and positions outside the volume agree with the bulk read.
  std::mt19937_64 random(3);
  for (int i = 0; i < 2000; ++i) {
    const std::array<std::int64_t, 3> voxel{
        std::uniform_int_distribution<std::int64_t>(0, dims[0] - 1)(random),
        std::uniform_int_distribution<std::int64_t>(0, dims[1] - 1)(random),
        std::uniform_int_distribution<std::int64_t>(0, dims[2] - 1)(random)};
    const auto index =
        static_cast<std::size_t>(voxel[0] + dims[0] * (voxel[1] + dims[1] * voxel[2]));
    ASSERT_EQ(a.code(voxel), codes_a[index]);
    ASSERT_EQ(a.distance(voxel), decodeSurfaceDistance(codes_a[index], 4, 1.0));
  }
  EXPECT_EQ(a.code({-1, 0, 0}), a.info().maxCode());
  EXPECT_EQ(a.code({0, 0, dims[2]}), a.info().maxCode());
  const Box sub{{5, -3, 7}, {40, 30, dims[2] + 2}};
  std::vector<std::uint8_t> part(static_cast<std::size_t>(sub.voxelCount()));
  a.readCodes(sub, part);
  std::size_t offset = 0;
  for (std::int64_t z = sub.min[2]; z < sub.max[2]; ++z) {
    for (std::int64_t y = sub.min[1]; y < sub.max[1]; ++y) {
      for (std::int64_t x = sub.min[0]; x < sub.max[0]; ++x) {
        ASSERT_EQ(part[offset++], a.code({x, y, z}));
      }
    }
  }
}

TEST_F(SurfaceTest, AirAroundThePartCostsAlmostNothing) {
  // The same part with 4 and with 60 voxels of air on every side: 13 times the voxels.
  const auto tight = scanOf("bracket", 0.25, 0.15, 0, 4.0);
  const auto loose = scanOf("bracket", 0.25, 0.15, 0, 60.0);
  const SurfaceInfo a = writeSurface(sieve(*tight, "tight.vsieve", 32), dir_ / "tight.vss");
  const SurfaceInfo b = writeSurface(sieve(*loose, "loose.vsieve", 32), dir_ / "loose.vss");
  const auto voxels = [](const SurfaceInfo& info) {
    return static_cast<double>(info.dims[0] * info.dims[1] * info.dims[2]);
  };
  EXPECT_GT(voxels(b), 8.0 * voxels(a));
  EXPECT_GT(b.chunks, 8 * a.chunks);
  EXPECT_NEAR(static_cast<double>(b.band_voxel_count), static_cast<double>(a.band_voxel_count),
              0.02 * static_cast<double>(a.band_voxel_count));
  // Only the chunk table and the split of the surface into more chunks cost a little.
  EXPECT_LT(static_cast<double>(b.file_bytes), 1.25 * static_cast<double>(a.file_bytes));
}

TEST_F(SurfaceTest, LevelSetAndMeshFollowTheCodes) {
  const auto scan = scanOf("housing", 0.25, 0.15);
  const Dataset dataset = sieve(*scan, "scan.vsieve");
  (void)writeSurface(dataset, dir_ / "surface.vss");
  const SurfaceMask mask = SurfaceMask::open(dir_ / "surface.vss");
  const auto grid = mask.toLevelSet();
  EXPECT_EQ(grid->getGridClass(), openvdb::GRID_LEVEL_SET);
  const double v = mask.info().voxel_size_mm;
  EXPECT_NEAR(grid->voxelSize()[0], v, 1e-12);
  const auto dims = mask.info().dims;
  auto accessor = grid->getConstAccessor();
  for (std::int64_t z = 0; z < dims[2]; z += 3) {
    for (std::int64_t y = 0; y < dims[1]; y += 3) {
      for (std::int64_t x = 0; x < dims[0]; x += 3) {
        const float value = accessor.getValue(
            openvdb::Coord(static_cast<int>(x), static_cast<int>(y), static_cast<int>(z)));
        ASSERT_EQ(value < 0.0F, mask.distance({x, y, z}) < 0.0F) << x << " " << y << " " << z;
        if (accessor.isValueOn(
                openvdb::Coord(static_cast<int>(x), static_cast<int>(y), static_cast<int>(z)))) {
          ASSERT_NEAR(value, mask.distance({x, y, z}) * v, 1e-6);
        }
      }
    }
  }
  // The mesh encloses the volume counted from the codes.
  const Mesh mesh = mask.toMesh();
  EXPECT_NEAR(meshVolumeMm3(mesh), mask.info().volume_mm3, 0.01 * mask.info().volume_mm3);
}

TEST_F(SurfaceTest, DisplayMeshFitsTheBudgetAndLiesOnTheSurface) {
  const auto scan = scanOf("hub", 0.3, 0.15);
  const Dataset dataset = sieve(*scan, "scan.vsieve");
  (void)writeSurface(dataset, dir_ / "surface.vss");
  const SurfaceMask mask = SurfaceMask::open(dir_ / "surface.vss");
  const double v = mask.info().voxel_size_mm;
  const auto origin = scan->originMm();
  const IndexedMesh full = surfaceDisplayMesh(mask, 10000000);
  ASSERT_FALSE(full.triangles.empty());
  // Points in level-0 voxel coordinates, on the true surface within a fraction of a voxel.
  double squares = 0.0;
  for (const auto& p : full.points) {
    const double d =
        scan->signedDistanceMm({origin[0] + p[0] * v, origin[1] + p[1] * v, origin[2] + p[2] * v}) /
        v;
    squares += d * d;
  }
  EXPECT_LT(std::sqrt(squares / static_cast<double>(full.points.size())), 0.15);
  for (const auto& t : full.triangles) {
    for (const std::uint32_t index : t) {
      ASSERT_LT(index, full.points.size());
    }
  }
  // A small budget coarsens the surface until it fits.
  const std::size_t budget = full.triangles.size() / 5;
  const IndexedMesh coarse = surfaceDisplayMesh(mask, budget);
  EXPECT_LE(coarse.triangles.size(), budget);
  EXPECT_GT(coarse.triangles.size(), budget / 20);
}

TEST_F(SurfaceTest, RejectsInvalidOptionsAndFiles) {
  const auto scan = scanOf("bracket", 0.2, 0.2);
  const Dataset dataset = sieve(*scan, "scan.vsieve");
  SurfaceOptions options;
  options.bits = 1;
  EXPECT_THROW((void)writeSurface(dataset, dir_ / "s.vss", options), std::invalid_argument);
  options.bits = 9;
  EXPECT_THROW((void)writeSurface(dataset, dir_ / "s.vss", options), std::invalid_argument);
  options.bits = 4;
  options.band_voxels = 4.0;  // wider than the three voxels of air the sieve keeps
  EXPECT_THROW((void)writeSurface(dataset, dir_ / "s.vss", options), std::invalid_argument);
  options.band_voxels = 1.0;
  options.compression_level = 23;
  EXPECT_THROW((void)writeSurface(dataset, dir_ / "s.vss", options), std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(dir_ / "s.vss"));

  (void)writeSurface(dataset, dir_ / "s.vss");
  const auto size = std::filesystem::file_size(dir_ / "s.vss");
  std::filesystem::resize_file(dir_ / "s.vss", size - 3);
  EXPECT_THROW((void)SurfaceMask::open(dir_ / "s.vss"), std::runtime_error);
  EXPECT_THROW((void)readSurfaceInfo(dir_ / "missing.vss"), std::exception);
}

}  // namespace
}  // namespace voxelsieve
