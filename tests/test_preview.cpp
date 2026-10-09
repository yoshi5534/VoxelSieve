#include <gtest/gtest.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <numeric>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "detail/network.hpp"
#include "voxelsieve/dataset.hpp"
#include "voxelsieve/io.hpp"
#include "voxelsieve/phantom.hpp"
#include "voxelsieve/preview.hpp"
#include "voxelsieve/source.hpp"
#include "voxelsieve/studio.hpp"

namespace voxelsieve {
namespace {

using Json = nlohmann::json;

class PreviewTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("voxelsieve_preview_" +
            std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  /// A phantom that is not a cube and not a multiple of the preview's block size.
  static PhantomSpec spec() {
    PhantomSpec spec = defaultPhantomSpec();
    spec.dims = {100, 90, 70};
    spec.voxel_size = VoxelSize(0.1, 0.1, 0.15);
    spec.outer_size_mm = {7.0, 6.0, 7.0};
    spec.noise_sigma = 300.0;
    return spec;
  }

  std::filesystem::path dir_;
};

/// A phantom that claims slow random access, like a TIFF stack on a share, and records the
/// slices it was asked for.
class SlowPhantomSource final : public VolumeSource {
 public:
  explicit SlowPhantomSource(const PhantomSpec& spec)
      : phantom_(spec), reads_(static_cast<std::size_t>(spec.dims[2])) {}
  [[nodiscard]] std::array<std::int64_t, 3> dims() const override { return phantom_.dims(); }
  [[nodiscard]] VoxelSize voxelSize() const override { return phantom_.voxelSize(); }
  [[nodiscard]] bool slowRandomAccess() const override { return true; }
  void readRegion(const Box& box, std::span<std::uint16_t> out) const override {
    for (std::int64_t z = box.min[2]; z < box.max[2]; ++z) {
      reads_[static_cast<std::size_t>(z)] += box.size(0) * box.size(1);
    }
    phantom_.readRegion(box, out);
  }
  /// Voxels read per slice.
  [[nodiscard]] std::vector<std::int64_t> reads() const {
    std::vector<std::int64_t> reads;
    reads.reserve(reads_.size());
    for (const auto& count : reads_) {
      reads.push_back(count.load());
    }
    return reads;
  }

 private:
  PhantomSource phantom_;
  mutable std::vector<std::atomic<std::int64_t>> reads_;
};

TEST(PreviewSlices, AreSpreadEvenlyAndTakeAllOfAShortVolume) {
  EXPECT_EQ(previewSlices(100, 4), (std::vector<std::int64_t>{12, 37, 62, 87}));
  EXPECT_EQ(previewSlices(5, 64), (std::vector<std::int64_t>{0, 1, 2, 3, 4}));
  EXPECT_EQ(previewSlices(1, 1), (std::vector<std::int64_t>{0}));
  EXPECT_TRUE(previewSlices(0, 8).empty());
  const auto slices = previewSlices(2000, 64);
  ASSERT_EQ(slices.size(), 64U);
  EXPECT_TRUE(std::is_sorted(slices.begin(), slices.end()));
  EXPECT_EQ(std::adjacent_find(slices.begin(), slices.end()), slices.end());
  EXPECT_EQ(slices.front(), 15);  // the middle of the first 31.25 slices
  EXPECT_EQ(slices.back(), 1984);
}

TEST_F(PreviewTest, AveragesWholeSlicesAndEstimatesTheThreshold) {
  const PhantomSpec phantom = spec();
  const PhantomSource source(phantom);
  PreviewOptions options;
  options.slices = 8;
  options.size = 32;
  const ImportPreview preview = readImportPreview(source, options);

  // 100 x 90 in blocks of 4 x 4 voxels (ceil(100 / 32)), 8 slices.
  EXPECT_EQ(preview.pixel_stride, 4);
  EXPECT_EQ(preview.volume.dims, (std::array<std::int64_t, 3>{25, 23, 8}));
  EXPECT_EQ(preview.slices, previewSlices(70, 8));
  EXPECT_EQ(preview.source_dims, phantom.dims);
  EXPECT_DOUBLE_EQ(preview.volume.voxel_size[0], 0.4);
  EXPECT_DOUBLE_EQ(preview.volume.voxel_size[1], 0.4);
  EXPECT_DOUBLE_EQ(preview.volume.voxel_size[2], 0.15 * 70.0 / 8.0);
  EXPECT_DOUBLE_EQ(preview.fractionRead(), 8.0 / 70.0);

  // Every voxel of the slices read counts in the histogram at full resolution.
  const std::uint64_t counted =
      std::accumulate(preview.histogram.begin(), preview.histogram.end(), std::uint64_t{0});
  EXPECT_EQ(counted, 8U * 100U * 90U);

  // A preview voxel is the rounded mean of its block, clipped at the edge of the slice.
  for (const std::array<std::int64_t, 3> voxel :
       {std::array<std::int64_t, 3>{0, 0, 0}, std::array<std::int64_t, 3>{12, 11, 4},
        std::array<std::int64_t, 3>{24, 22, 7}}) {
    std::uint64_t sum = 0;
    std::uint64_t count = 0;
    const std::int64_t z = preview.slices[static_cast<std::size_t>(voxel[2])];
    for (std::int64_t y = voxel[1] * 4; y < std::min<std::int64_t>(voxel[1] * 4 + 4, 90); ++y) {
      for (std::int64_t x = voxel[0] * 4; x < std::min<std::int64_t>(voxel[0] * 4 + 4, 100); ++x) {
        sum += phantomValue(phantom, x, y, z);
        ++count;
      }
    }
    EXPECT_EQ(preview.volume.at(voxel[0], voxel[1], voxel[2]), (sum + count / 2) / count);
  }

  // The threshold lies between air and material, as the import's estimate from all slices.
  EXPECT_GT(preview.threshold, static_cast<float>(phantom.air_value) + 1000.0F);
  EXPECT_LT(preview.threshold, static_cast<float>(phantom.material_value) - 1000.0F);
  EXPECT_NEAR(preview.air_level, phantom.air_value, 300.0);
  EXPECT_LT(preview.window[0], preview.window[1]);
}

TEST_F(PreviewTest, StagedImportReadsThePreviewSlicesFirstAndEverySliceOnce) {
  const PhantomSpec phantom = spec();
  DatasetOptions options;
  options.brick_size = 32;
  options.preview_options.slices = 6;
  options.preview_options.size = 50;
  std::vector<std::string> events;
  std::vector<std::int64_t> reads_at_preview;
  const SlowPhantomSource slow(phantom);
  ImportPreview during;
  options.preview = [&](const ImportPreview& preview) {
    events.emplace_back("preview");
    reads_at_preview = slow.reads();
    during = preview;
  };
  options.progress = [&events](std::string_view stage, double) {
    if (events.empty() || events.back() != stage) {
      events.emplace_back(stage);
    }
  };
  const DatasetInfo info = writeDataset(slow, dir_ / "staged", options);
  EXPECT_EQ(events, (std::vector<std::string>{"staging", "preview", "staging", "histogram",
                                              "bricks", "levels"}));

  // When the preview came, only its slices had been read; in the end every slice exactly once.
  const auto slices = previewSlices(phantom.dims[2], 6);
  const std::int64_t slice_voxels = phantom.dims[0] * phantom.dims[1];
  for (std::int64_t z = 0; z < phantom.dims[2]; ++z) {
    const bool previewed = std::find(slices.begin(), slices.end(), z) != slices.end();
    EXPECT_EQ(reads_at_preview[static_cast<std::size_t>(z)], previewed ? slice_voxels : 0) << z;
    EXPECT_EQ(slow.reads()[static_cast<std::size_t>(z)], slice_voxels) << z;
  }

  // The same preview as read directly, and the same dataset as without one.
  ASSERT_FALSE(during.slices.empty());
  const ImportPreview direct = readImportPreview(PhantomSource(phantom), options.preview_options);
  EXPECT_EQ(during.volume.data, direct.volume.data);
  EXPECT_EQ(during.histogram, direct.histogram);
  EXPECT_EQ(during.threshold, direct.threshold);
  DatasetOptions plain;
  plain.brick_size = 32;
  const DatasetInfo reference = writeDataset(PhantomSource(phantom), dir_ / "plain", plain);
  EXPECT_EQ(info.active_voxel_count, reference.active_voxel_count);
  EXPECT_EQ(info.threshold, reference.threshold);

  // A source read directly also gets its preview.
  int previews = 0;
  options.preview = [&previews](const ImportPreview& preview) {
    ++previews;
    EXPECT_EQ(preview.slices.size(), 6U);
  };
  options.progress = {};
  const DatasetInfo direct_info = writeDataset(PhantomSource(phantom), dir_ / "direct", options);
  EXPECT_EQ(previews, 1);
  EXPECT_EQ(direct_info.active_voxel_count, reference.active_voxel_count);
}

TEST_F(PreviewTest, PictureShowsTheSectionsInTrueProportionsAndTheThreshold) {
  const ImportPreview preview = readImportPreview(PhantomSource(spec()), {});
  const PreviewImage image = renderPreview(preview);
  // The longest edge (10.5 mm along z) is 256 pixels, 10 mm along x 244 and 9 mm along y 219.
  EXPECT_EQ(image.width, 244 + 4 + 244 + 4 + 219);
  EXPECT_EQ(image.height, 256 + 4 + 96);
  ASSERT_EQ(image.rgb.size(), static_cast<std::size_t>(image.width * image.height) * 3);
  const auto pixel = [&image](int x, int y) {
    const auto i = static_cast<std::size_t>(x + image.width * y) * 3;
    return std::array<std::uint8_t, 3>{image.rgb[i], image.rgb[i + 1], image.rgb[i + 2]};
  };
  // Air at the corner of the slice, the box wall bright.
  EXPECT_LT(pixel(2, 2)[0], 40);
  bool bright = false;
  for (int x = 0; x < 244; ++x) {
    bright = bright || pixel(x, 110)[0] > 200;
  }
  EXPECT_TRUE(bright);
  int red = 0;
  for (int x = 0; x < image.width; ++x) {
    const auto p = pixel(x, image.height - 1);
    red += static_cast<int>(p[0] > 200 && p[1] < 100);
  }
  EXPECT_EQ(red, 1);
  const std::vector<std::uint8_t> png = previewPng(preview);
  ASSERT_GT(png.size(), 8U);
  EXPECT_EQ(png[1], 'P');

  const Json summary = previewSummary(preview, 64);
  EXPECT_EQ(summary.at("slices_read"), 64);
  EXPECT_EQ(summary.at("slices"), 70);
  EXPECT_EQ(summary.at("pixel_stride"), 1);
  const auto counts = summary.at("histogram").at("counts").get<std::vector<std::uint64_t>>();
  EXPECT_EQ(counts.size(), 64U);
  EXPECT_EQ(std::accumulate(counts.begin(), counts.end(), std::uint64_t{0}), 64U * 100U * 90U);
}

TEST_F(PreviewTest, LocalFilesAreNotOnANetworkShare) {
  EXPECT_FALSE(detail::onNetworkShare(dir_));
  EXPECT_FALSE(detail::onNetworkShare(dir_ / "missing.raw"));
  const Volume16 volume = generatePhantom(spec());
  writeRaw(dir_ / "scan.raw", volume);
  EXPECT_FALSE(
      MappedRawSource(dir_ / "scan.raw", volume.dims, volume.voxel_size).slowRandomAccess());
}

/// Hands over a preview, then blocks until released, to observe the studio in between.
class PreviewThenWait final : public Operation {
 public:
  explicit PreviewThenWait(std::shared_future<void> release) : release_(std::move(release)) {
    info_.id = "preview_then_wait";
    info_.title = "Preview, then wait";
    info_.outputs = {{"dataset", artifact::kDataset, ""}};
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }
  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    PhantomSpec phantom = defaultPhantomSpec();
    phantom.dims = {32, 32, 32};
    const ImportPreview preview = readImportPreview(PhantomSource(phantom), {});
    context.preview(previewSummary(preview), previewPng(preview));
    release_.wait();
    throw std::runtime_error("released");
  }

 private:
  OperationInfo info_;
  std::shared_future<void> release_;
};

TEST_F(PreviewTest, StudioShowsThePreviewWhileTheImportRunsAndKeepsItWithTheStep) {
  std::promise<void> release;
  Studio studio;
  studio.registry().add(std::make_shared<PreviewThenWait>(release.get_future().share()));
  (void)studio.call("project_create", {{"path", (dir_ / "p").string()}});
  EXPECT_THROW((void)studio.call("import_preview", {}), std::invalid_argument);

  std::thread runner([&studio] {
    EXPECT_THROW((void)studio.call("run_preview_then_wait", {}), std::runtime_error);
  });
  Json status;
  for (int i = 0; i < 1000; ++i) {
    status = studio.call("project_status", {});
    if (status.at("running").contains("preview")) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_EQ(status.at("running").at("preview").at("slices_read"), 32);
  const Json running = studio.call("import_preview", {});
  EXPECT_TRUE(running.at("running").get<bool>());
  EXPECT_EQ(running.at("image").at("mime_type"), "image/png");
  EXPECT_FALSE(running.at("image").at("base64").get<std::string>().empty());
  release.set_value();
  runner.join();
  EXPECT_TRUE(studio.call("project_status", {}).at("running").is_null());

  // A real import keeps its preview with the step and says when it came.
  const Volume16 volume = generatePhantom(spec());
  writeRaw(dir_ / "scan.raw", volume);
  writeJson(dir_ / "scan.json",
            {{"dims", volume.dims}, {"voxel_size_mm", {0.1, 0.1, 0.15}}, {"format", "uint16"}});
  const Json step =
      studio.call("run_import_raw", {{"path", (dir_ / "scan.raw").string()}, {"brick_size", 32}});
  const auto messages = step.at("messages").get<std::vector<std::string>>();
  EXPECT_TRUE(std::any_of(messages.begin(), messages.end(), [](const std::string& message) {
    return message.starts_with("Preview after ");
  }));
  const Json kept = studio.call("import_preview", {});
  EXPECT_EQ(kept.at("step"), step.at("id"));
  EXPECT_FALSE(kept.at("running").get<bool>());
  EXPECT_EQ(kept.at("slices_read"), 64);
  EXPECT_GT(kept.at("threshold").get<double>(), 1000.0);
  EXPECT_TRUE(kept.contains("seconds"));
  EXPECT_EQ(studio.call("import_preview", {{"step", step.at("id")}}).at("step"), step.at("id"));
  EXPECT_THROW((void)studio.call("import_preview", {{"step", 1}}), std::invalid_argument);
}

TEST_F(PreviewTest, StudioImportsGzipRawVolumesWithTheirSidecar) {
  const Volume16 volume = generatePhantom(spec());
  gzFile file = gzopen((dir_ / "scan.raw.gz").string().c_str(), "wb");
  ASSERT_NE(file, nullptr);
  const auto bytes = static_cast<unsigned>(volume.data.size() * sizeof(std::uint16_t));
  ASSERT_EQ(gzwrite(file, volume.data.data(), bytes), static_cast<int>(bytes));
  gzclose(file);
  writeJson(dir_ / "scan.json",
            {{"dims", volume.dims}, {"voxel_size_mm", {0.1, 0.1, 0.15}}, {"format", "uint16"}});

  Studio studio;
  (void)studio.call("project_create", {{"path", (dir_ / "p").string()}});
  const Json listed = studio.call("browse", {{"path", dir_.string()}});
  const Json& entries = listed.at("entries");
  EXPECT_TRUE(std::any_of(entries.begin(), entries.end(), [](const Json& entry) {
    return entry.at("name") == "scan.raw.gz" && entry.at("kind") == "raw";
  }));
  const Json step = studio.call("run_import_raw",
                                {{"path", (dir_ / "scan.raw.gz").string()}, {"brick_size", 32}});
  const auto messages = step.at("messages").get<std::vector<std::string>>();
  EXPECT_TRUE(std::any_of(messages.begin(), messages.end(), [](const std::string& message) {
    return message.starts_with("gzip-compressed");
  }));
  EXPECT_EQ(step.at("summary").at("dims"), volume.dims);
  EXPECT_EQ(studio.call("import_preview", {}).at("slices_read"), 64);
}

}  // namespace
}  // namespace voxelsieve
