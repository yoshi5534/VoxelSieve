#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <string>
#include <vector>

#include "voxelsieve/compare.hpp"
#include "voxelsieve/dataset.hpp"
#include "voxelsieve/mesh.hpp"
#include "voxelsieve/porosity.hpp"
#include "voxelsieve/report.hpp"
#include "voxelsieve/surface.hpp"
#include "voxelsieve/synthetic.hpp"

namespace voxelsieve {
namespace {

using Json = nlohmann::json;

TEST(TemplateTest, VariablesAreEscapedUnlessTripleBraced) {
  const Json data = {{"name", "<b>A & B</b>"}, {"n", 3}};
  EXPECT_EQ(renderTemplate("{{name}}|{{{name}}}|{{ n }}|{{missing}}", data),
            "&lt;b&gt;A &amp; B&lt;/b&gt;|<b>A & B</b>|3|");
}

TEST(TemplateTest, DottedNamesAndComments) {
  const Json data = {{"part", {{"name", "Gehäuse"}, {"serial", {{"no", "7"}}}}}};
  EXPECT_EQ(renderTemplate("{{! note }}{{part.name}} {{part.serial.no}} {{part.none.x}}", data),
            "Gehäuse 7 ");
}

TEST(TemplateTest, SectionsIterateListsAndSeeOuterNames) {
  const Json data = {{"unit", "mm"},
                     {"items", Json::array({{{"v", 1}}, {{"v", 2}}})},
                     {"tags", Json::array({"a", "b"})}};
  EXPECT_EQ(renderTemplate("{{#items}}{{v}} {{unit}};{{/items}}", data), "1 mm;2 mm;");
  EXPECT_EQ(renderTemplate("{{#tags}}[{{.}}]{{/tags}}", data), "[a][b]");
}

TEST(TemplateTest, SectionsAndInvertedSectionsFollowTruthiness) {
  const Json data = {{"yes", true}, {"no", false}, {"empty", Json::array()},
                     {"text", ""},  {"zero", 0},   {"obj", {{"k", "v"}}}};
  EXPECT_EQ(renderTemplate("{{#yes}}Y{{/yes}}{{^yes}}N{{/yes}}", data), "Y");
  EXPECT_EQ(renderTemplate("{{#no}}Y{{/no}}{{^no}}N{{/no}}", data), "N");
  EXPECT_EQ(renderTemplate("{{#empty}}Y{{/empty}}{{^empty}}N{{/empty}}", data), "N");
  EXPECT_EQ(renderTemplate("{{#text}}Y{{/text}}{{^text}}N{{/text}}", data), "N");
  EXPECT_EQ(renderTemplate("{{#missing}}Y{{/missing}}{{^missing}}N{{/missing}}", data), "N");
  EXPECT_EQ(renderTemplate("{{#zero}}{{zero}}{{/zero}}", data), "0");
  EXPECT_EQ(renderTemplate("{{#obj}}{{k}}{{/obj}}", data), "v");
  EXPECT_EQ(renderTemplate("{{#no}}{{#yes}}inner{{/yes}}{{/no}}after", data), "after");
}

TEST(TemplateTest, MalformedTemplatesThrow) {
  const Json data = {{"a", true}};
  EXPECT_THROW((void)renderTemplate("{{#a}}open", data), std::runtime_error);
  EXPECT_THROW((void)renderTemplate("{{/a}}", data), std::runtime_error);
  EXPECT_THROW((void)renderTemplate("{{#a}}x{{/b}}", data), std::runtime_error);
  EXPECT_THROW((void)renderTemplate("{{a", data), std::runtime_error);
}

class ReportTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("voxelsieve_report_" +
            std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
    std::filesystem::remove_all(dir_);
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  /// Box of 10 x 6 x 4 mm with one lunker, sieved and analysed.
  void analyzeBoxWithLunker() {
    SyntheticSpec spec;
    spec.noise_sigma = 500.0;
    spec.lunker_count = 1;
    spec.lunker_radius_mm = 0.5;
    scan_ = std::make_unique<SyntheticScan>(boxMesh(kSize), spec);
    ASSERT_EQ(scan_->defects().size(), 1U);
    DatasetOptions options;
    options.brick_size = 32;
    (void)writeDataset(*scan_, dir_ / "scan.vsieve", options);
    dataset_ = std::make_unique<Dataset>(Dataset::open(dir_ / "scan.vsieve"));
    result_ = analyzePorosity(*dataset_);
    ASSERT_EQ(result_.pores.size(), 1U);
  }

  /// Mesh coordinates to dataset coordinates (voxel index times voxel size).
  [[nodiscard]] std::array<double, 3> toDataset(const std::array<double, 3>& mesh_mm) const {
    const auto origin = scan_->originMm();
    return {mesh_mm[0] - origin[0], mesh_mm[1] - origin[1], mesh_mm[2] - origin[2]};
  }

  static constexpr std::array<double, 3> kSize{10.0, 6.0, 4.0};
  std::filesystem::path dir_;
  std::unique_ptr<SyntheticScan> scan_;
  std::unique_ptr<Dataset> dataset_;
  PorosityResult result_;
};

TEST_F(ReportTest, PoreSizeLimitSeparatesAtTheLunkerExtent) {
  analyzeBoxWithLunker();
  const Defect& lunker = scan_->defects().front();
  const double size = poreSizeMm(result_.pores.front(), result_.voxel_size_mm);
  // The lobes lie inside the enclosing sphere; the extent is at least the core sphere.
  EXPECT_LE(size, 2.0 * lunker.radius_mm + result_.voxel_size_mm);
  EXPECT_GE(size, 2.0 * lunker.spheres.front().radius_mm - result_.voxel_size_mm);

  InspectionZone tight{"tight", std::nullopt, {}};
  tight.limits.max_pore_size_mm = 0.5 * size;
  InspectionZone loose{"loose", std::nullopt, {}};
  loose.limits.max_pore_size_mm = 2.0 * lunker.radius_mm + result_.voxel_size_mm;
  const Evaluation evaluation = evaluate(result_, {tight, loose});
  ASSERT_EQ(evaluation.zones.size(), 2U);
  EXPECT_FALSE(evaluation.zones[0].passed());
  EXPECT_TRUE(evaluation.zones[1].passed());
  EXPECT_FALSE(evaluation.passed());
  EXPECT_EQ(evaluation.zones[0].largest_pore_id, result_.pores.front().id);
}

TEST_F(ReportTest, ZonesCountOnlyWhatLiesInTheirBox) {
  analyzeBoxWithLunker();
  const Defect& lunker = scan_->defects().front();
  const auto center = toDataset(lunker.center_mm);
  const auto part_min = toDataset({-kSize[0] / 2, -kSize[1] / 2, -kSize[2] / 2});
  const auto part_max = toDataset({kSize[0] / 2, kSize[1] / 2, kSize[2] / 2});

  // Half of the part along x, on the side away from the lunker.
  const double middle = 0.5 * (part_min[0] + part_max[0]);
  const bool lunker_low = center[0] < middle;
  std::array<std::array<double, 3>, 2> box{
      {{lunker_low ? middle : part_min[0] - 1.0, part_min[1] - 1.0, part_min[2] - 1.0},
       {lunker_low ? part_max[0] + 1.0 : middle, part_max[1] + 1.0, part_max[2] + 1.0}}};
  InspectionZone away{"away", box, {}};
  away.limits.max_porosity_percent = 0.0;
  away.limits.max_pore_count = 0;
  InspectionZone whole{"whole", std::nullopt, {}};
  whole.limits.max_porosity_percent = 100.0;

  const Evaluation evaluation = evaluate(result_, {away, whole});
  const ZoneEvaluation& half = evaluation.zones[0];
  EXPECT_EQ(half.pore_count, 0);
  EXPECT_TRUE(half.passed());
  const double half_volume = 0.5 * kSize[0] * kSize[1] * kSize[2];
  EXPECT_NEAR(half.part_volume_mm3, half_volume, 0.01 * half_volume);

  const ZoneEvaluation& all = evaluation.zones[1];
  EXPECT_EQ(all.pore_count, 1);
  const double truth = 100.0 * lunker.void_volume_mm3 / (kSize[0] * kSize[1] * kSize[2]);
  EXPECT_NEAR(all.porosity_percent, truth, 0.05 * truth);
}

TEST_F(ReportTest, LooseningCanBeForbidden) {
  SyntheticSpec spec;
  spec.noise_sigma = 300.0;
  spec.loosening_count = 1;
  spec.loosening_radius_mm = 1.2;
  spec.loosening_porosity = 0.05;
  const SyntheticScan scan(boxMesh({8.0, 8.0, 8.0}), spec);
  DatasetOptions options;
  options.brick_size = 32;
  (void)writeDataset(scan, dir_ / "scan.vsieve", options);
  const auto dataset = Dataset::open(dir_ / "scan.vsieve");
  const PorosityResult result = analyzePorosity(dataset);
  ASSERT_EQ(result.zones.size(), 1U);

  InspectionZone zone{"no loosening", std::nullopt, {}};
  zone.limits.allow_loosening = false;
  const Evaluation evaluation = evaluate(result, {zone});
  ASSERT_EQ(evaluation.zones.front().criteria.size(), 1U);
  EXPECT_EQ(evaluation.zones.front().loosening_zones, 1);
  EXPECT_FALSE(evaluation.passed());
}

TEST_F(ReportTest, ZonesAreReadFromTheOrder) {
  const Json order = Json::parse(R"({
    "acceptance": {"zones": [
      {"name": "A", "box_mm": [[0, 0, 0], [1, 2, 3]], "max_pore_size_mm": 0.5,
       "max_pore_count": 3, "count_min_size_mm": 0.2, "max_porosity_percent": 1.5,
       "allow_loosening": false},
      {"max_porosity_percent": 2}
    ]}})");
  const auto zones = inspectionZonesFromJson(order);
  ASSERT_EQ(zones.size(), 2U);
  EXPECT_EQ(zones[0].name, "A");
  ASSERT_TRUE(zones[0].box_mm.has_value());
  EXPECT_EQ(zones[0].box_mm.value_or(std::array<std::array<double, 3>, 2>{})[1][2], 3.0);
  EXPECT_EQ(zones[0].limits.max_pore_size_mm, 0.5);
  EXPECT_EQ(zones[0].limits.max_pore_count, 3);
  EXPECT_EQ(zones[0].limits.count_min_size_mm, 0.2);
  EXPECT_FALSE(zones[0].limits.allow_loosening);
  EXPECT_EQ(zones[1].name, "Zone 2");
  EXPECT_FALSE(zones[1].box_mm.has_value());
  EXPECT_FALSE(zones[1].limits.max_pore_size_mm.has_value());

  EXPECT_TRUE(inspectionZonesFromJson(Json::object()).empty());
  EXPECT_THROW((void)inspectionZonesFromJson(
                   Json::parse(R"({"acceptance": {"zones": [{"box_mm": [1, 2]}]}})")),
               std::invalid_argument);
}

TEST_F(ReportTest, DefaultTemplateRendersReportWithMissingFieldsMarked) {
  analyzeBoxWithLunker();
  const Json order = Json::parse(R"({
    "part": {"name": "Prüfkörper <1>"},
    "acceptance": {"zones": [{"name": "Gesamt", "max_pore_size_mm": 0.01}]}})");
  const auto zones = inspectionZonesFromJson(order);
  const Evaluation evaluation = evaluate(result_, zones);
  writePorosityImages(*dataset_, result_, dir_ / "out");

  std::vector<std::string> warnings;
  const Json data =
      reportData(order, result_, PorosityOptions{}, evaluation, dir_ / "out", &warnings);
  EXPECT_EQ(data.at("part").at("name"), "Prüfkörper <1>");
  EXPECT_EQ(data.at("part").at("drawing"), "nicht angegeben");
  EXPECT_EQ(warnings.size(), 19U);  // every mandatory field except the part name
  EXPECT_FALSE(data.at("evaluation").at("passed").get<bool>());
  EXPECT_TRUE(data.at("images").at("z").get<std::string>().starts_with("data:image/png;base64,"));

  const std::string html = renderTemplate(defaultReportTemplate(), data);
  EXPECT_NE(html.find("Prüfbericht"), std::string::npos);
  EXPECT_NE(html.find("Prüfkörper &lt;1&gt;"), std::string::npos);
  EXPECT_NE(html.find("Die Anforderungen sind nicht erfüllt."), std::string::npos);
  EXPECT_EQ(html.find("{{"), std::string::npos);
}

TEST_F(ReportTest, ShowsPartViewsAndTheNominalActualComparison) {
  analyzeBoxWithLunker();
  (void)writeSurface(*dataset_, dir_ / "surface.vss");
  const SurfaceMask mask = SurfaceMask::open(dir_ / "surface.vss");
  const CompareResult compared =
      compareToCad(mask, boxMesh({kSize[0], kSize[1], kSize[2]}, {5.0, -2.0, 1.0}));
  writeComparison(compared, dir_ / "comparison");

  Json data = reportData(Json::object(), result_, PorosityOptions{}, Evaluation{}, dir_);
  const std::string without = renderTemplate(defaultReportTemplate(), data);
  EXPECT_EQ(without.find("Soll-Ist-Vergleich"), std::string::npos);
  EXPECT_NE(without.find("7 Bewertung"), std::string::npos);

  addPartImages(data, mask, result_);
  addComparison(data, dir_ / "comparison", "box.stl");
  const Json& images = data.at("images");
  for (const char* name : {"part", "pores", "deviation_1", "deviation_2"}) {
    EXPECT_TRUE(images.at(name).get<std::string>().starts_with("data:image/png;base64,")) << name;
  }
  EXPECT_NE(images.at("part"), images.at("pores"));
  const auto histogram = images.at("deviation_histogram").get<std::string>();
  EXPECT_TRUE(histogram.starts_with("<svg"));
  EXPECT_NE(histogram.find("<rect"), std::string::npos);
  const Json& comparison = data.at("comparison");
  EXPECT_EQ(comparison.at("cad"), "box.stl");
  EXPECT_EQ(comparison.at("tolerance"), "± 0,100 mm");
  EXPECT_TRUE(comparison.at("within").get<std::string>().ends_with(" %"));

  const std::string html = renderTemplate(defaultReportTemplate(), data);
  EXPECT_NE(html.find("7 Soll-Ist-Vergleich"), std::string::npos);
  EXPECT_NE(html.find("8 Bewertung"), std::string::npos);
  EXPECT_NE(html.find("box.stl"), std::string::npos);
  EXPECT_NE(html.find(images.at("pores").get<std::string>()), std::string::npos);
  EXPECT_EQ(html.find("{{"), std::string::npos);
  EXPECT_THROW(addComparison(data, dir_ / "none"), std::runtime_error);
}

}  // namespace
}  // namespace voxelsieve
