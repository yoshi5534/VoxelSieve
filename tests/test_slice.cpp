#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <string>

#include "voxelsieve/dataset.hpp"
#include "voxelsieve/mesh.hpp"
#include "voxelsieve/porosity.hpp"
#include "voxelsieve/slice.hpp"
#include "voxelsieve/synthetic.hpp"

namespace voxelsieve {
namespace {

class SliceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("voxelsieve_slice_" +
            std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
    std::filesystem::remove_all(dir_);
    SyntheticSpec spec;
    spec.noise_sigma = 300.0;
    spec.lunker_count = 2;
    spec.lunker_radius_mm = 0.5;
    spec.loosening_count = 1;
    spec.loosening_radius_mm = 1.0;
    scan_ = std::make_unique<SyntheticScan>(boxMesh({8.0, 6.0, 5.0}), spec);
    DatasetOptions options;
    options.brick_size = 32;  // slices cross bricks
    (void)writeDataset(*scan_, dir_ / "scan.vsieve", options);
    dataset_ = std::make_unique<Dataset>(Dataset::open(dir_ / "scan.vsieve"));
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  [[nodiscard]] std::array<std::int64_t, 3> voxelOf(const std::array<double, 3>& mm) const {
    const auto origin = scan_->originMm();
    std::array<std::int64_t, 3> voxel{};
    for (std::size_t a = 0; a < 3; ++a) {
      voxel[a] = static_cast<std::int64_t>(std::floor((mm[a] - origin[a]) / scan_->voxelSize()[a]));
    }
    return voxel;
  }

  std::filesystem::path dir_;
  std::unique_ptr<SyntheticScan> scan_;
  std::unique_ptr<Dataset> dataset_;
};

TEST_F(SliceTest, PixelsMatchTheDatasetOnEveryAxis) {
  const auto& dims = dataset_->info().dims;
  for (int axis = 0; axis < 3; ++axis) {
    const auto [u, v] = sliceAxes(axis);
    SliceRequest request;
    request.axis = axis;
    request.index = dims[static_cast<std::size_t>(axis)] / 2;
    request.origin = {-5, 3};  // partly outside the volume
    request.size = {dims[static_cast<std::size_t>(u)] + 10, 40};
    const SliceImage image = readSlice(*dataset_, request);
    ASSERT_EQ(image.grey.size(), static_cast<std::size_t>(image.width * image.height));
    for (std::int64_t py = 0; py < image.height; py += 3) {
      for (std::int64_t px = 0; px < image.width; px += 5) {
        std::array<std::int64_t, 3> voxel{};
        voxel[static_cast<std::size_t>(axis)] = request.index;
        voxel[static_cast<std::size_t>(u)] = request.origin[0] + px;
        voxel[static_cast<std::size_t>(v)] = request.origin[1] + py;
        const float expected = dataset_->sample(0, voxel).value_or(dataset_->info().air_level);
        EXPECT_EQ(image.grey[static_cast<std::size_t>(py * image.width + px)], expected)
            << "axis " << axis << " pixel " << px << ", " << py;
      }
    }
  }
}

TEST_F(SliceTest, OverlayShowsTheGroundTruthDefectsOnEveryLevel) {
  const PorosityResult porosity = analyzePorosity(*dataset_);
  for (const Defect& defect : scan_->defects()) {
    const auto center = voxelOf(defect.center_mm);
    const auto expected =
        defect.type == DefectType::kLunker ? SliceOverlay::kPore : SliceOverlay::kZone;
    for (int level = 0; level < static_cast<int>(dataset_->info().levels.size()); ++level) {
      const auto shift = static_cast<unsigned>(level);
      SliceRequest request;
      request.axis = 2;
      request.index = center[2];
      request.level = level;
      request.origin = {(center[0] >> shift) - 8, (center[1] >> shift) - 8};
      request.size = {16, 16};
      const SliceImage image = readSlice(*dataset_, request, &porosity);
      // The pixel holding the defect centre is marked; for a lunker also darker than material.
      const auto centre = static_cast<std::size_t>(8 * 16 + 8);
      EXPECT_EQ(image.overlay[centre], static_cast<std::uint8_t>(expected))
          << "level " << level << " defect at " << center[0] << ", " << center[1];
      if (level == 0 && defect.type == DefectType::kLunker) {
        EXPECT_LT(image.grey[centre], porosity.material_level / 2);
      }
    }
  }
  // Far from all defects nothing is marked.
  SliceRequest corner;
  corner.index = dataset_->info().dims[2] / 2;
  corner.size = {12, 12};
  const SliceImage image = readSlice(*dataset_, corner, &porosity);
  EXPECT_EQ(std::count(image.overlay.begin(), image.overlay.end(), 0), 144);
}

TEST_F(SliceTest, CoarseLevelsHalveTheSlice) {
  SliceRequest request;
  request.level = 1;
  request.index = dataset_->info().dims[2] / 2;
  request.size = {dataset_->level(1).dims[0], dataset_->level(1).dims[1]};
  const SliceImage image = readSlice(*dataset_, request);
  const std::int64_t x = request.size[0] / 2;
  const std::int64_t y = request.size[1] / 2;
  const auto expected = dataset_->sample(1, {x, y, request.index >> 1U});
  EXPECT_EQ(image.grey[static_cast<std::size_t>(y * image.width + x)], expected.value_or(-1.0F));
}

TEST_F(SliceTest, VolumePreviewFitsAndShowsTheDefects) {
  const PorosityResult porosity = analyzePorosity(*dataset_);
  const VolumePreview preview = readVolumePreview(*dataset_, 48, &porosity);
  EXPECT_LE(*std::max_element(preview.dims.begin(), preview.dims.end()), 48);
  EXPECT_EQ(preview.dims, dataset_->level(preview.level).dims);
  const auto voxels = static_cast<std::size_t>(preview.dims[0] * preview.dims[1] * preview.dims[2]);
  ASSERT_EQ(preview.grey.size(), voxels);
  ASSERT_EQ(preview.overlay.size(), voxels);
  // The window spans air to material.
  EXPECT_NEAR(preview.low, 1000.0F, 1500.0F);
  EXPECT_NEAR(preview.high, 20000.0F, 1500.0F);
  // Every defect centre is marked in the coarse volume.
  for (const Defect& defect : scan_->defects()) {
    const auto center = voxelOf(defect.center_mm);
    const auto shift = static_cast<unsigned>(preview.level);
    const auto index = static_cast<std::size_t>(
        (center[0] >> shift) +
        preview.dims[0] * ((center[1] >> shift) + preview.dims[1] * (center[2] >> shift)));
    EXPECT_NE(preview.overlay[index], 0);
  }
  // Without a size limit that small, the finest level that fits is used.
  EXPECT_EQ(readVolumePreview(*dataset_, 4096).level, 0);
}

TEST_F(SliceTest, VolumeRegionIsReadFinerWithTheWindowOfTheWhole) {
  const PorosityResult porosity = analyzePorosity(*dataset_);
  const VolumePreview whole = readVolumePreview(*dataset_, 32, &porosity);
  ASSERT_GT(whole.level, 0);
  // A part around a lunker, reaching past the volume on one side.
  const Defect& lunker =
      *std::find_if(scan_->defects().begin(), scan_->defects().end(),
                    [](const Defect& d) { return d.type == DefectType::kLunker; });
  const auto center = voxelOf(lunker.center_mm);
  VolumeRequest request;
  request.max_size = 32;
  request.region = Box{{center[0] - 12, center[1] - 12, -5}, {center[0] + 12, center[1] + 12, 20}};
  request.window = std::array<float, 2>{whole.low, whole.high};
  const VolumePreview part = readVolumePreview(*dataset_, request, &porosity);
  EXPECT_EQ(part.level, 0);
  EXPECT_EQ(part.origin, (std::array<std::int64_t, 3>{center[0] - 12, center[1] - 12, 0}));
  EXPECT_EQ(part.dims, (std::array<std::int64_t, 3>{24, 24, 20}));
  EXPECT_EQ(part.low, whole.low);
  EXPECT_EQ(part.high, whole.high);
  const float scale = 255.0F / (part.high - part.low);
  for (std::int64_t z = 0; z < part.dims[2]; z += 3) {
    for (std::int64_t y = 0; y < part.dims[1]; y += 5) {
      for (std::int64_t x = 0; x < part.dims[0]; x += 7) {
        const std::array<std::int64_t, 3> voxel{part.origin[0] + x, part.origin[1] + y, z};
        const float value = dataset_->sample(0, voxel).value_or(dataset_->info().air_level);
        const auto expected = std::lround(std::clamp((value - part.low) * scale, 0.0F, 255.0F));
        EXPECT_EQ(part.grey[static_cast<std::size_t>(x + 24 * (y + 24 * z))], expected);
      }
    }
  }
  // The lunker is marked at the finer level too, if it lies in the region.
  if (center[2] < 20) {
    const auto index = static_cast<std::size_t>(12 + 24 * (12 + 24 * center[2]));
    EXPECT_EQ(part.overlay[index], static_cast<std::uint8_t>(SliceOverlay::kPore));
  }
  // A coarser level where the region does not fit, with the voxels covering it.
  request.max_size = 8;
  const VolumePreview coarse = readVolumePreview(*dataset_, request);
  ASSERT_GT(coarse.level, 0);
  const auto shift = static_cast<unsigned>(coarse.level);
  EXPECT_LE(*std::max_element(coarse.dims.begin(), coarse.dims.end()), 8);
  EXPECT_EQ(coarse.origin[0], (center[0] - 12) >> shift);
  EXPECT_EQ(coarse.dims[2], ((20 - 1) >> shift) + 1);
  // A region outside the volume holds nothing.
  request.region = Box{{-10, -10, -10}, {-1, 5, 5}};
  EXPECT_THROW((void)readVolumePreview(*dataset_, request), std::invalid_argument);
}

TEST_F(SliceTest, RejectsInvalidRequests) {
  SliceRequest request;
  request.axis = 3;
  EXPECT_THROW((void)readSlice(*dataset_, request), std::invalid_argument);
  request.axis = 2;
  request.index = dataset_->info().dims[2];
  EXPECT_THROW((void)readSlice(*dataset_, request), std::invalid_argument);
  request.index = 0;
  request.level = 99;
  EXPECT_THROW((void)readSlice(*dataset_, request), std::invalid_argument);
  request.level = 0;
  request.size = {0, 10};
  EXPECT_THROW((void)readSlice(*dataset_, request), std::invalid_argument);
}

TEST_F(SliceTest, OnAccessLoadingReadsTheSameAndKeepsTheBudget) {
  constexpr std::size_t kBudget = std::size_t{2} << 20U;
  const Dataset lazy = Dataset::open(dir_ / "scan.vsieve", kBudget, BrickLoading::kOnAccess);
  const auto& dims = dataset_->info().dims;
  for (std::int64_t z = 0; z < dims[2]; z += 4) {
    SliceRequest request;
    request.index = z;
    request.size = {dims[0], dims[1]};
    EXPECT_EQ(readSlice(lazy, request).grey, readSlice(*dataset_, request).grey) << "z " << z;
  }
  // Touching every voxel grows the bricks after loading; the cache still keeps its budget,
  // apart from the most recent brick, which is always kept.
  Box all;
  all.max = dims;
  std::vector<float> values(static_cast<std::size_t>(all.voxelCount()));
  lazy.readRegion(0, all, values);
  lazy.readRegion(0, all, values);
  const CacheStats stats = lazy.cacheStats();
  EXPECT_GT(stats.misses, 0U);
  EXPECT_LE(stats.bytes, kBudget + (std::size_t{1} << 20U));
}

}  // namespace
}  // namespace voxelsieve
