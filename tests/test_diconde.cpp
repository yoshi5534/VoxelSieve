#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <numbers>
#include <string>
#include <vector>

#include "voxelsieve/dataset.hpp"
#include "voxelsieve/dicom.hpp"
#include "voxelsieve/diconde.hpp"
#include "voxelsieve/io.hpp"
#include "voxelsieve/materials.hpp"
#include "voxelsieve/phantom.hpp"
#include "voxelsieve/porosity.hpp"
#include "voxelsieve/report.hpp"
#include "voxelsieve/source.hpp"
#include "voxelsieve/studio.hpp"

namespace voxelsieve {
namespace {

using Json = nlohmann::json;

std::vector<std::uint16_t> readAll(const VolumeSource& source) {
  const auto dims = source.dims();
  std::vector<std::uint16_t> out(static_cast<std::size_t>(dims[0] * dims[1] * dims[2]));
  source.readRegion({{0, 0, 0}, dims}, out);
  return out;
}

/// A volume of signed samples as an import of a signed DICOM scan leaves it: grey values 32768
/// above the samples and a value mapping that undoes the shift and applies the rescaling.
class SignedSource final : public VolumeSource {
 public:
  [[nodiscard]] std::array<std::int64_t, 3> dims() const override { return {12, 10, 6}; }
  [[nodiscard]] VoxelSize voxelSize() const override { return {0.3, 0.3, 0.5}; }
  [[nodiscard]] ValueMapping valueMapping() const override {
    return {.offset = -1024.0 - 2.0 * 32768.0, .scale = 2.0};
  }
  void readRegion(const Box& box, std::span<std::uint16_t> out) const override {
    std::size_t i = 0;
    for (std::int64_t z = box.min[2]; z < box.max[2]; ++z) {
      for (std::int64_t y = box.min[1]; y < box.max[1]; ++y) {
        for (std::int64_t x = box.min[0]; x < box.max[0]; ++x) {
          out[i++] = grey(x, y, z);
        }
      }
    }
  }
  static std::uint16_t grey(std::int64_t x, std::int64_t y, std::int64_t z) {
    return static_cast<std::uint16_t>(32768 - 1000 + 97 * x + 31 * y + 400 * z);
  }
};

class DicondeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("voxelsieve_diconde_" +
            std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  static Json exampleOrder() {
    std::ifstream in(std::filesystem::path(VOXELSIEVE_SOURCE_DIR) / "examples" /
                     "inspection_order.json");
    return Json::parse(in);
  }

  std::filesystem::path dir_;
};

TEST_F(DicondeTest, VolumeComesBackExactlyWhereItLies) {
  PhantomSpec spec = defaultPhantomSpec();
  spec.dims = {40, 36, 30};
  spec.voxel_size = VoxelSize(0.25, 0.25, 0.4);
  spec.outer_size_mm = {7.0, 6.0, 8.0};
  spec.pores.clear();
  const PhantomSource phantom(spec);
  DicondeWriteOptions options;
  options.pose = RigidTransform::fromAxisAngle({1.0, 2.0, 0.5}, 35.0, {10.0, -4.0, 120.0});
  options.series_description = "Phantom";
  options.component.name = "Gehäuse";  // UTF-8 survives
  options.component.id = "S-17";
  const DicondeSeries series = writeDicondeSeries(phantom, dir_ / "volume", options);
  EXPECT_EQ(series.files, spec.dims[2]);
  EXPECT_FALSE(series.study_uid.empty());
  EXPECT_THROW((void)writeDicondeSeries(phantom, dir_ / "volume", options), std::invalid_argument);

  const DicomStackSource read(dir_ / "volume");
  EXPECT_EQ(read.dims(), spec.dims);
  EXPECT_EQ(read.sampleType(), SampleType::kUInt16);
  EXPECT_FALSE(read.isSigned());
  EXPECT_TRUE(read.valueMapping().isIdentity());
  for (std::size_t axis = 0; axis < 3; ++axis) {
    EXPECT_NEAR(read.voxelSize()[axis], spec.voxel_size[axis], 1e-9) << axis;
  }
  EXPECT_EQ(readAll(read), readAll(phantom));
  const auto pose = read.filePose().matrix();
  const auto expected = options.pose.matrix();
  for (std::size_t i = 0; i < 16; ++i) {
    EXPECT_NEAR(pose[i], expected[i], 1e-9) << i;
  }
  const DicondeInfo info = readDicondeInfo(read);
  EXPECT_EQ(info.modality, "CT");
  EXPECT_EQ(info.series_uid, series.series_uid);
  EXPECT_EQ(info.frame_of_reference_uid, series.frame_of_reference_uid);
  EXPECT_EQ(info.series_description, "Phantom");
  EXPECT_EQ(info.series_number, 1);
  EXPECT_EQ(info.component.name, "Gehäuse");
  EXPECT_EQ(info.component.id, "S-17");
  EXPECT_FALSE(info.derived);
  EXPECT_TRUE(info.indications.empty());
}

TEST_F(DicondeTest, SignedScansGetTheirStoredValuesBack) {
  const SignedSource source;
  (void)writeDicondeSeries(source, dir_ / "signed");
  const DicomStackSource read(dir_ / "signed");
  EXPECT_EQ(readAll(read), readAll(source));
  EXPECT_EQ(read.valueMapping(), source.valueMapping());
  // value = intercept + slope * stored sample, as in the scan the volume came from.
  EXPECT_DOUBLE_EQ(read.valueMapping().toValue(SignedSource::grey(3, 2, 1)),
                   -1024.0 + 2.0 * (-1000 + 97 * 3 + 31 * 2 + 400));
}

TEST_F(DicondeTest, ResultsBecomeIndicationsAndLabels) {
  PhantomSpec spec = defaultPhantomSpec();
  spec.dims = {96, 96, 96};
  const PhantomSource phantom(spec);
  DatasetOptions sieve;
  sieve.brick_size = 32;
  (void)writeDataset(phantom, dir_ / "dataset", sieve);
  const Dataset dataset =
      Dataset::open(dir_ / "dataset", Dataset::kDefaultCacheBytes, BrickLoading::kOnAccess);
  const PorosityResult porosity = analyzePorosity(dataset);
  ASSERT_GE(porosity.pores.size(), spec.pores.size());
  SegmentationOptions segmentation;
  segmentation.materials = 1;
  segmentation.names = {"Aluminium"};
  (void)segmentMaterials(dataset, dir_ / "materials", segmentation);

  const Json order = exampleOrder();
  DicondeWriteOptions options;
  options.indications = porosityIndications(porosity);
  const Evaluation evaluation = evaluate(porosity, inspectionZonesFromJson(order));
  applyInspectionOrder(order, options, &evaluation);
  const DicondeSeries volume = writeDicondeDataset(dataset, dir_ / "out" / "volume", options);
  DicondeWriteOptions labels = options;
  labels.inspection.study_uid = volume.study_uid;
  labels.frame_of_reference_uid = volume.frame_of_reference_uid;
  labels.indications.clear();
  (void)writeDicondeMaterials(MaterialVolume::open(dir_ / "materials"), dir_ / "out" / "materials",
                              labels);

  // The grey values of the part are exact; removed outside air reads as the air level.
  const DicomStackSource grey(dir_ / "out" / "volume");
  const auto values = readAll(grey);
  const auto& dims = spec.dims;
  const auto at = [&dims](std::int64_t x, std::int64_t y, std::int64_t z) {
    return static_cast<std::size_t>(x + dims[0] * (y + dims[1] * z));
  };
  const auto expected = readAll(phantom);
  const auto air = static_cast<std::uint16_t>(std::lround(dataset.info().air_level));
  for (const std::array<std::int64_t, 3> voxel :
       {std::array<std::int64_t, 3>{48, 48, 8}, {48, 10, 48}, {0, 0, 0}, {95, 95, 95}}) {
    const auto sample = dataset.sample(0, voxel);
    EXPECT_EQ(values[at(voxel[0], voxel[1], voxel[2])],
              sample ? expected[at(voxel[0], voxel[1], voxel[2])] : air);
  }

  // Every pore is an indication at its analytic centre, with its volume.
  const DicondeInfo info = readDicondeInfo(grey);
  ASSERT_EQ(info.indications.size(), porosity.pores.size() + porosity.zones.size());
  const auto origin = voxelCenterMm(spec, 0, 0, 0);  // voxel 0 relative to the volume centre
  for (const Pore& pore : spec.pores) {
    const auto found = std::ranges::find_if(info.indications, [&](const DicondeIndication& i) {
      double distance = 0.0;
      for (std::size_t axis = 0; axis < 3; ++axis) {
        distance += std::pow(i.center_mm[axis] + origin[axis] - pore.center_mm[axis], 2);
      }
      return i.type == "PORE" && std::sqrt(distance) < spec.voxel_size[0];
    });
    ASSERT_NE(found, info.indications.end()) << pore.center_mm[0];
    ASSERT_FALSE(found->properties.empty());
    EXPECT_EQ(found->properties[0].label, "Volume");
    EXPECT_EQ(found->properties[0].unit, "mm3");
    EXPECT_NEAR(found->properties[0].value,
                4.0 / 3.0 * std::numbers::pi * std::pow(pore.radius_mm, 3),
                0.3 * 4.0 / 3.0 * std::numbers::pi * std::pow(pore.radius_mm, 3));
  }
  for (std::size_t i = 0; i < info.indications.size(); ++i) {
    EXPECT_EQ(info.indications[i].number, static_cast<int>(i) + 1);
  }

  // The inspection order fills in component, inspection and equipment.
  EXPECT_EQ(info.component.name, "Gearbox housing");
  EXPECT_EQ(info.component.id, "Batch 2026-38, part 7");
  EXPECT_EQ(info.component.material, "EN AC-AlSi9Cu3(Fe)");
  EXPECT_EQ(info.component.other_ids, std::vector<std::string>{"DWG 123-456 rev. C"});
  EXPECT_EQ(info.inspection.study_id, "TR-2026-0001");
  EXPECT_EQ(info.inspection.accession, "A-4711");
  EXPECT_EQ(info.inspection.date, "20260925");
  EXPECT_EQ(info.inspection.company, "Example CT Testing Lab");
  EXPECT_EQ(info.inspection.owner, "Example Foundry Ltd");
  EXPECT_EQ(info.inspection.inspectors, std::vector<std::string>{"M. Example"});
  EXPECT_NE(info.inspection.notes.find(evaluation.passed() ? "passed" : "failed"),
            std::string::npos);
  EXPECT_NE(info.inspection.notes.find("Zone A (sealing face)"), std::string::npos);
  EXPECT_EQ(info.equipment.kv, 200.0);
  EXPECT_EQ(info.equipment.current_ua, 300.0);
  EXPECT_EQ(info.equipment.exposure_ms, 500.0);
  EXPECT_EQ(info.equipment.filter, "0.5 mm Cu");

  // The labels are a derived 8-bit series in the same study and coordinates.
  const DicomStackSource materials(dir_ / "out" / "materials");
  EXPECT_EQ(materials.sampleType(), SampleType::kUInt8);
  const DicondeInfo labelled = readDicondeInfo(materials);
  EXPECT_TRUE(labelled.derived);
  EXPECT_EQ(labelled.inspection.study_uid, volume.study_uid);
  EXPECT_EQ(labelled.frame_of_reference_uid, volume.frame_of_reference_uid);
  EXPECT_EQ(labelled.image_comments, "Labels: 0 air; 1 Aluminium");
  const auto label_values = readAll(materials);
  std::vector<std::uint8_t> segmented(label_values.size());
  MaterialVolume::open(dir_ / "materials").readRegion({{0, 0, 0}, dims}, segmented);
  EXPECT_TRUE(std::ranges::equal(label_values, segmented));
  EXPECT_GT(std::ranges::count(label_values, 1), 0);
  // Air in the box wall (the cavity) and outside is 0, the wall 1.
  EXPECT_EQ(label_values[at(48, 48, 48)], 0);
  EXPECT_EQ(label_values[at(0, 0, 0)], 0);
  EXPECT_EQ(label_values[at(48, 48, 13)], 1);  // z wall: |z| in [2.5, 4] mm
}

TEST_F(DicondeTest, LongTextIsCutToTheLimitsOfItsAttributes) {
  const SignedSource source;
  DicondeWriteOptions options;
  options.inspection.study_id = "A study id far longer than sixteen";
  options.component.name = std::string(40, 'a') + "\\" + std::string(40, 'b');
  DicondeIndication indication;
  indication.number = 1;
  indication.label = "A label longer than sixteen characters";
  indication.type = "pore, gas";
  indication.center_mm = {1.0, 1.0, 1.0};
  options.indications = {indication};
  (void)writeDicondeSeries(source, dir_ / "cut", options);
  const DicondeInfo info = readDicondeInfo(DicomStackSource(dir_ / "cut"));
  EXPECT_EQ(info.inspection.study_id, "A study id far l");
  EXPECT_EQ(info.component.name, std::string(40, 'a') + "/" + std::string(23, 'b'));
  ASSERT_EQ(info.indications.size(), 1U);
  EXPECT_EQ(info.indications[0].label, "A label longer t");
  EXPECT_EQ(info.indications[0].type, "PORE_ GAS");
}

TEST_F(DicondeTest, StudioExportsAndImportsAgain) {
  PhantomSpec spec = defaultPhantomSpec();
  spec.dims = {96, 96, 96};
  writeRaw(dir_ / "scan.raw", generatePhantom(spec));
  writeJson(dir_ / "scan.json", phantomToJson(spec));
  writeJson(dir_ / "order.json", exampleOrder());
  Studio studio({});
  studio.call("project_create", {{"path", (dir_ / "project").string()}});
  const Json imported = studio.call(
      "run_import_raw",
      {{"path", (dir_ / "scan.raw").string()}, {"brick_size", 32}, {"object_name", "Housing"}});
  ASSERT_EQ(imported.at("status"), "done") << imported.dump();
  ASSERT_EQ(studio.call("run_move", {{"objects", {"o1"}}, {"translation_mm", {5.0, 0.0, -2.0}}})
                .at("status"),
            "done");
  ASSERT_EQ(studio.call("run_porosity", {}).at("status"), "done");
  ASSERT_EQ(studio.call("run_segment_materials", {{"materials", 1}}).at("status"), "done");

  const Json exported = studio.call(
      "run_export_diconde",
      {{"order_path", (dir_ / "order.json").string()}, {"path", (dir_ / "copy").string()}});
  ASSERT_EQ(exported.at("status"), "done") << exported.dump();
  EXPECT_EQ(exported.at("summary").at("series"), 2);
  EXPECT_EQ(exported.at("summary").at("slices"), spec.dims[2]);
  EXPECT_GE(exported.at("summary").at("indications").get<std::size_t>(), spec.pores.size());
  EXPECT_TRUE(std::filesystem::exists(dir_ / "copy" / "volume" / "slice_00000.dcm"));
  EXPECT_TRUE(std::filesystem::exists(dir_ / "copy" / "materials" / "slice_00095.dcm"));
  EXPECT_TRUE(std::filesystem::exists(dir_ / "copy" / "diconde.json"));
  // The series lies where the object lies, and keeps the object's name.
  const DicomStackSource written(dir_ / "copy" / "volume");
  EXPECT_NEAR(written.filePose().translation[0], 5.0, 1e-9);
  EXPECT_NEAR(written.filePose().translation[2], -2.0, 1e-9);
  EXPECT_EQ(readDicondeInfo(written).series_description, "Housing");

  // Importing it again gives the same grey values and tells what the files say.
  const Json again = studio.call(
      "run_import_dicom", {{"path", (dir_ / "copy" / "volume").string()}, {"brick_size", 32}});
  ASSERT_EQ(again.at("status"), "done") << again.dump();
  EXPECT_EQ(again.at("summary").at("component"), "Gearbox housing");
  EXPECT_EQ(again.at("summary").at("study_id"), "TR-2026-0001");
  EXPECT_EQ(again.at("summary").at("indications"), exported.at("summary").at("indications"));
  const Dataset first =
      Dataset::open(dir_ / "project" / "steps" / "1-import_raw" / "dataset.vsieve");
  const Dataset second = Dataset::open(
      dir_ / "project" / "steps" / (std::to_string(again.at("id").get<int>()) + "-import_dicom") /
      "dataset.vsieve");
  for (const std::array<std::int64_t, 3> voxel :
       {std::array<std::int64_t, 3>{48, 48, 8}, {48, 10, 48}, {20, 30, 40}}) {
    EXPECT_EQ(first.sample(0, voxel), second.sample(0, voxel));
  }
  const Json objects = studio.call("objects", {}).at("objects");
  ASSERT_EQ(objects.size(), 2U);
  const auto moved = objects[0].at("pose").get<std::vector<double>>();
  const auto placed = objects[1].at("pose").get<std::vector<double>>();
  ASSERT_EQ(placed.size(), moved.size());
  for (std::size_t i = 0; i < moved.size(); ++i) {
    EXPECT_NEAR(placed[i], moved[i], 1e-9) << i;
  }
}

}  // namespace
}  // namespace voxelsieve
