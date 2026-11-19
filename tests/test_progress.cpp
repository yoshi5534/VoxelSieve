// Progress and cancellation of long runs (review step 5.1).

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "voxelsieve/dataset.hpp"
#include "voxelsieve/materials.hpp"
#include "voxelsieve/orientation.hpp"
#include "voxelsieve/phantom.hpp"
#include "voxelsieve/porosity.hpp"
#include "voxelsieve/progress.hpp"
#include "voxelsieve/source.hpp"
#include "voxelsieve/surface.hpp"

namespace voxelsieve {
namespace {

TEST(ProgressToken, ReportsPartsInOrderAndStopsWhenCancelled) {
  std::vector<double> reported;
  std::atomic<bool> cancel{false};
  const ProgressToken token([&](double f) { reported.push_back(f); }, &cancel);
  const ProgressToken second = token.part(0.5, 1.0);
  token.part(0.0, 0.5).report(0.5);
  token.part(0.0, 0.5).report(0.4);  // backwards: not reported
  second.report(0.5);
  second.report(0.501);  // the same whole percent
  EXPECT_EQ(reported, (std::vector<double>{0.25, 0.75}));
  EXPECT_FALSE(token.cancelled());
  cancel = true;
  EXPECT_TRUE(second.cancelled());
  EXPECT_THROW(second.check(), Cancelled);
  EXPECT_THROW(token.report(1.0), Cancelled);
  EXPECT_EQ(reported.size(), 2U);

  // The default token reports nothing and is never cancelled.
  const ProgressToken none;
  EXPECT_NO_THROW(none.report(0.5));
  EXPECT_FALSE(none.cancelled());

  ProgressSteps steps(token.part(0.0, 1.0), 4);
  EXPECT_THROW(steps.step(), Cancelled);
}

class CancelTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("voxelsieve_cancel_" +
            std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
    std::filesystem::remove_all(dir_);
    DatasetOptions options;
    options.brick_size = 32;  // 64 bricks: the run has steps to be cancelled between
    (void)writeDataset(PhantomSource(PhantomSpec{}), dir_ / "scan.vsieve", options);
    dataset_ = std::make_unique<Dataset>(Dataset::open(dir_ / "scan.vsieve"));
  }
  void TearDown() override {
    dataset_.reset();
    std::filesystem::remove_all(dir_);
  }

  /// A token that cancels the run at its first report above 0, and what was reported.
  struct Canceller {
    std::atomic<bool> cancel{false};
    double at = -1.0;
    double last = 0.0;
    ProgressToken token() {
      return ProgressToken(
          [this](double f) {
            last = f;
            if (f > 0.0 && !cancel) {
              at = f;
              cancel = true;
            }
          },
          &cancel);
    }
  };

  /// Runs `run` once to its end and once cancelled midway: the cancelled run throws Cancelled
  /// and reports nothing after the cancellation.
  static void expectCancellable(const std::function<void(const ProgressToken&)>& run) {
    double full = 0.0;
    run(ProgressToken([&](double f) { full = f; }, nullptr));
    EXPECT_GT(full, 0.5);
    Canceller canceller;
    EXPECT_THROW(run(canceller.token()), Cancelled);
    EXPECT_TRUE(canceller.cancel.load());
    EXPECT_EQ(canceller.last, canceller.at);
    EXPECT_LT(canceller.at, full);
  }

  std::filesystem::path dir_;
  std::unique_ptr<Dataset> dataset_;
};

TEST_F(CancelTest, PorosityStopsBetweenBricks) {
  expectCancellable([&](const ProgressToken& token) {
    PorosityOptions options;
    options.progress = token;
    (void)analyzePorosity(*dataset_, options);
  });
}

TEST_F(CancelTest, SurfaceStopsBetweenChunksAndLeavesNoFile) {
  int run = 0;
  expectCancellable([&](const ProgressToken& token) {
    SurfaceOptions options;
    options.progress = token;
    (void)writeSurface(*dataset_, dir_ / ("surface" + std::to_string(run++) + ".vss"), options);
  });
  EXPECT_TRUE(std::filesystem::exists(dir_ / "surface0.vss"));
  for (const auto& entry : std::filesystem::directory_iterator(dir_)) {
    EXPECT_EQ(entry.path().filename().string().find("surface1"), std::string::npos) << entry.path();
  }
}

TEST_F(CancelTest, SegmentationStopsBetweenBricks) {
  int run = 0;
  expectCancellable([&](const ProgressToken& token) {
    SegmentationOptions options;
    options.progress = token;
    (void)segmentMaterials(*dataset_, dir_ / ("materials" + std::to_string(run++)), options);
  });
}

TEST_F(CancelTest, OrientationStopsBetweenTiles) {
  expectCancellable([&](const ProgressToken& token) {
    OrientationOptions options;
    options.progress = token;
    (void)analyzeOrientation(*dataset_, options);
  });
}

}  // namespace
}  // namespace voxelsieve
