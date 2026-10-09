#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "voxelsieve/io.hpp"
#include "voxelsieve/mesh.hpp"
#include "voxelsieve/operation.hpp"
#include "voxelsieve/project.hpp"
#include "voxelsieve/synthetic.hpp"

namespace voxelsieve {
namespace {

using Json = nlohmann::json;

int readNumber(const std::filesystem::path& file) {
  std::ifstream in(file);
  int value = 0;
  in >> value;
  return value;
}

/// Writes a number; no inputs.
class MakeNumber final : public Operation {
 public:
  MakeNumber() {
    info_.id = "make_number";
    info_.title = "Make number";
    info_.outputs = {{"number", "number", ""}};
    info_.parameters = {{"type", "object"},
                        {"properties", {{"value", {{"type", "integer"}, {"default", 1}}}}}};
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }
  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    std::ofstream(context.output_dir / "n.txt") << context.params.at("value").get<int>();
    return {{{"number", "n.txt"}}, {{"value", context.params.at("value")}}};
  }

 private:
  OperationInfo info_;
};

/// Adds `amount` to a number.
class AddNumber final : public Operation {
 public:
  AddNumber() {
    info_.id = "add";
    info_.title = "Add";
    info_.inputs = {{"number", "number", ""}};
    info_.outputs = {{"number", "number", ""}};
    info_.parameters = {{"type", "object"},
                        {"properties", {{"amount", {{"type", "integer"}, {"minimum", 0}}}}},
                        {"required", {"amount"}}};
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }
  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    const int value =
        readNumber(context.inputs.at("number")) + context.params.at("amount").get<int>();
    std::ofstream(context.output_dir / "n.txt") << value;
    context.log("sum " + std::to_string(value));
    return {{{"number", "n.txt"}}, {{"value", value}}};
  }

 private:
  OperationInfo info_;
};

class Fail final : public Operation {
 public:
  Fail() {
    info_.id = "fail";
    info_.title = "Fail";
    info_.inputs = {{"number", "number", ""}};
    info_.outputs = {{"number", "number", ""}};
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }
  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    std::ofstream(context.output_dir / "partial.txt") << "x";
    throw std::runtime_error("broken on purpose");
  }

 private:
  OperationInfo info_;
};

class ProjectTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("voxelsieve_project_" +
            std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
    std::filesystem::remove_all(dir_);
    registry_.add(std::make_shared<MakeNumber>());
    registry_.add(std::make_shared<AddNumber>());
    registry_.add(std::make_shared<Fail>());
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  [[nodiscard]] int latestNumber(const Project& project) const {
    const auto ref = project.latest("number");
    return ref ? readNumber(project.resolve(*ref)) : -1;
  }

  std::filesystem::path dir_;
  OperationRegistry registry_;
};

TEST_F(ProjectTest, StepsChainThroughTheLatestOutputAndPersist) {
  auto project = Project::create(dir_ / "p", "Test");
  project.run(registry_, "make_number", {{"value", 2}});
  const Step added = project.run(registry_, "add", {{"amount", 3}});
  EXPECT_EQ(added.inputs.at("number").step, 1);
  EXPECT_EQ(added.status, "done");
  EXPECT_EQ(added.messages, std::vector<std::string>{"sum 5"});
  EXPECT_FALSE(added.started.empty());
  EXPECT_EQ(latestNumber(project), 5);

  const auto reopened = Project::open(dir_ / "p");
  EXPECT_EQ(reopened.name(), "Test");
  ASSERT_EQ(reopened.steps().size(), 2U);
  EXPECT_EQ(reopened.cursor(), 2U);
  EXPECT_EQ(reopened.steps()[1].params.at("amount"), 3);
  EXPECT_EQ(latestNumber(reopened), 5);
}

TEST_F(ProjectTest, UndoRedoMoveTheCursorAndANewStepDiscardsTheUndoneOnes) {
  auto project = Project::create(dir_ / "p", "Test");
  project.run(registry_, "make_number", {{"value", 1}});
  project.run(registry_, "add", {{"amount", 10}});
  const auto undone_dir = project.stepDir(project.steps()[1]);
  EXPECT_TRUE(project.undo());
  EXPECT_EQ(latestNumber(project), 1);
  EXPECT_TRUE(project.canRedo());
  EXPECT_TRUE(project.redo());
  EXPECT_EQ(latestNumber(project), 11);
  EXPECT_FALSE(project.redo());

  EXPECT_TRUE(project.undo());
  EXPECT_EQ(Project::open(dir_ / "p").cursor(), 1U);  // undo is saved too
  project.run(registry_, "add", {{"amount", 100}});
  ASSERT_EQ(project.steps().size(), 2U);
  EXPECT_EQ(project.steps()[1].id, 3);
  EXPECT_FALSE(std::filesystem::exists(undone_dir));
  EXPECT_EQ(latestNumber(project), 101);

  EXPECT_TRUE(project.undo());
  EXPECT_TRUE(project.undo());
  EXPECT_FALSE(project.undo());
  EXPECT_EQ(latestNumber(project), -1);
}

TEST_F(ProjectTest, FailedStepsAreRecordedButProvideNoOutputs) {
  auto project = Project::create(dir_ / "p", "Test");
  project.run(registry_, "make_number", {{"value", 7}});
  EXPECT_THROW(project.run(registry_, "fail"), std::runtime_error);
  ASSERT_EQ(project.steps().size(), 2U);
  const Step& failed = project.steps()[1];
  EXPECT_EQ(failed.status, "failed");
  EXPECT_EQ(failed.messages.back(), "broken on purpose");
  EXPECT_FALSE(std::filesystem::exists(project.stepDir(failed)));
  EXPECT_EQ(latestNumber(project), 7);

  // An explicit input from a failed or undone step is refused.
  EXPECT_THROW(project.run(registry_, "add", {{"amount", 1}}, {{"number", {2, "number"}}}),
               std::invalid_argument);
}

TEST_F(ProjectTest, InvalidRequestsLeaveTheProjectUnchanged) {
  auto project = Project::create(dir_ / "p", "Test");
  EXPECT_THROW(project.run(registry_, "add", {{"amount", 1}}), std::invalid_argument);  // no input
  project.run(registry_, "make_number");
  EXPECT_THROW(project.run(registry_, "nope"), std::invalid_argument);
  EXPECT_THROW(project.run(registry_, "add"), std::invalid_argument);  // missing amount
  EXPECT_THROW(project.run(registry_, "add", {{"amount", -1}}), std::invalid_argument);
  EXPECT_THROW(project.run(registry_, "add", {{"amount", "x"}}), std::invalid_argument);
  EXPECT_THROW(project.run(registry_, "add", {{"amount", 1}, {"other", 2}}), std::invalid_argument);
  EXPECT_THROW(project.run(registry_, "add", {{"amount", 1}}, {{"bogus", {1, "number"}}}),
               std::invalid_argument);
  EXPECT_EQ(project.steps().size(), 1U);
  EXPECT_EQ(latestNumber(project), 1);  // default value
}

TEST_F(ProjectTest, InterruptedStepIsMarkedFailedOnOpen) {
  auto project = Project::create(dir_ / "p", "Test");
  project.run(registry_, "make_number");
  Json json = project.toJson();
  json["steps"][0]["status"] = "running";
  std::ofstream(dir_ / "p" / "project.json") << json.dump();
  const auto reopened = Project::open(dir_ / "p");
  EXPECT_EQ(reopened.steps()[0].status, "failed");
  EXPECT_FALSE(reopened.latest("number").has_value());
}

TEST_F(ProjectTest, SaveAsContinuesInTheCopy) {
  auto project = Project::create(dir_ / "p", "Test");
  project.run(registry_, "make_number", {{"value", 4}});
  project.saveAs(dir_ / "copy");
  project.run(registry_, "add", {{"amount", 1}});
  EXPECT_EQ(Project::open(dir_ / "p").steps().size(), 1U);
  EXPECT_EQ(Project::open(dir_ / "copy").steps().size(), 2U);
  EXPECT_EQ(latestNumber(Project::open(dir_ / "copy")), 5);
  EXPECT_THROW(project.saveAs(dir_ / "p"), std::invalid_argument);
  EXPECT_THROW((void)Project::create(dir_ / "copy", "x"), std::invalid_argument);
}

TEST_F(ProjectTest, ViewStateAndSavedViewsPersistOutsideTheProtocol) {
  auto project = Project::create(dir_ / "p", "Test");
  project.run(registry_, "make_number", {{"value", 2}});
  const nlohmann::json state = {{"stage", "view"}, {"camera", {{"yaw", 0.5}}}};
  project.setView(state);
  const std::vector<std::uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n', 1, 2, 3};
  const int first = project.saveView("Übersicht", state, png).id;
  const int second = project.saveView("Ohne Bild", {{"stage", "report"}}).id;
  EXPECT_NE(first, second);
  EXPECT_TRUE(std::filesystem::exists(project.viewImage(first)));
  project.renameView(second, "Report");
  // Views are no steps: undo leaves them alone.
  EXPECT_TRUE(project.undo());
  EXPECT_EQ(project.savedViews().size(), 2U);

  auto reopened = Project::open(dir_ / "p");
  EXPECT_EQ(reopened.view(), state);
  ASSERT_EQ(reopened.savedViews().size(), 2U);
  EXPECT_EQ(reopened.savedView(first).name, "Übersicht");
  EXPECT_TRUE(reopened.savedView(first).has_image);
  EXPECT_EQ(reopened.savedView(second).name, "Report");
  EXPECT_FALSE(reopened.savedView(second).has_image);

  reopened.deleteView(first);
  EXPECT_FALSE(std::filesystem::exists(reopened.viewImage(first)));
  EXPECT_THROW((void)reopened.savedView(first), std::invalid_argument);
  // Ids are not reused after a delete.
  EXPECT_GT(reopened.saveView("Neu", state).id, second);
}

TEST_F(ProjectTest, InvalidViewsAreRejected) {
  auto project = Project::create(dir_ / "p", "Test");
  EXPECT_THROW(project.setView(nlohmann::json::array()), std::invalid_argument);
  EXPECT_THROW((void)project.saveView("", {}), std::invalid_argument);
  const std::vector<std::uint8_t> not_png = {'G', 'I', 'F', '8', '9', 'a', 0, 0, 0};
  EXPECT_THROW((void)project.saveView("Bild", nlohmann::json::object(), not_png),
               std::invalid_argument);
  EXPECT_THROW(project.setView({{"big", std::string(std::size_t{300} << 10U, 'x')}}),
               std::invalid_argument);
  EXPECT_THROW(project.renameView(7, "x"), std::invalid_argument);
  EXPECT_TRUE(project.savedViews().empty());
  EXPECT_FALSE(std::filesystem::exists(dir_ / "p" / "views"));
}

TEST_F(ProjectTest, AdoptViewsKeepsStepsAndTakesTheViews) {
  auto project = Project::create(dir_ / "p", "Test");
  Project working = project;  // an operation runs on a copy ...
  working.run(registry_, "make_number", {{"value", 4}});
  project.setView({{"stage", "view"}});  // ... while the UI saves its view
  (void)project.saveView("Während", {{"stage", "view"}});
  working.adoptViews(project);
  const auto reopened = Project::open(dir_ / "p");
  EXPECT_EQ(reopened.steps().size(), 1U);
  EXPECT_EQ(reopened.view(), nlohmann::json({{"stage", "view"}}));
  ASSERT_EQ(reopened.savedViews().size(), 1U);
  EXPECT_EQ(reopened.savedViews()[0].name, "Während");
}

TEST(ParameterTest, DefaultsTypesAndRanges) {
  const Json schema = {
      {"type", "object"},
      {"properties",
       {{"n", {{"type", "integer"}, {"minimum", 1}, {"maximum", 9}, {"default", 3}}},
        {"mode", {{"type", "string"}, {"enum", {"a", "b"}}}},
        {"dims",
         {{"type", "array"}, {"items", {{"type", "integer"}}}, {"minItems", 3}, {"maxItems", 3}}}}},
      {"required", {"mode"}}};
  EXPECT_EQ(validateParameters(schema, {{"mode", "a"}}).at("n"), 3);
  EXPECT_EQ(validateParameters(schema, {{"mode", "b"}, {"n", 9.0}}).at("n"), 9.0);
  EXPECT_THROW((void)validateParameters(schema, {{"mode", "c"}}), std::invalid_argument);
  EXPECT_THROW((void)validateParameters(schema, {{"mode", "a"}, {"n", 10}}), std::invalid_argument);
  EXPECT_THROW((void)validateParameters(schema, {{"mode", "a"}, {"n", 2.5}}),
               std::invalid_argument);
  EXPECT_THROW((void)validateParameters(schema, {{"mode", "a"}, {"dims", {1, 2}}}),
               std::invalid_argument);
  EXPECT_THROW((void)validateParameters(schema, {{"mode", "a"}, {"dims", {1, 2, "x"}}}),
               std::invalid_argument);
  EXPECT_NO_THROW((void)validateParameters(schema, {{"mode", "a"}, {"dims", {1, 2, 3}}}));
  EXPECT_THROW((void)validateParameters(schema, Json::array()), std::invalid_argument);
}

TEST_F(ProjectTest, BuiltinOperationsRunThePipeline) {
  OperationRegistry registry;
  registerBuiltinOperations(registry);
  EXPECT_EQ(registry.all().size(), 16U);
  EXPECT_NE(registry.find("import_tiff"), nullptr);

  SyntheticSpec spec;
  spec.noise_sigma = 300.0;
  spec.lunker_count = 2;
  spec.lunker_radius_mm = 0.5;
  const SyntheticScan scan(boxMesh({8.0, 6.0, 4.0}), spec);
  std::filesystem::create_directories(dir_);
  writeRaw(dir_ / "scan.raw", scan);
  writeJson(dir_ / "scan.json", scan.toJson());

  auto project = Project::create(dir_ / "p", "Pipeline");
  const Step imported = project.run(registry, "import_raw",
                                    {{"path", (dir_ / "scan.raw").string()}, {"brick_size", 32}});
  EXPECT_EQ(imported.summary.at("dims"), Json(scan.dims()));
  const Step porosity = project.run(registry, "porosity");
  EXPECT_EQ(porosity.summary.at("pores"), 2);
  const Json order = {{"part", {{"name", "Box"}}},
                      {"acceptance", {{"zones", {{{"name", "all"}, {"max_pore_size_mm", 0.1}}}}}}};
  const Step report = project.run(registry, "report", {{"order", order}});
  EXPECT_EQ(report.summary.at("passed"), false);
  const auto dir = project.resolve({report.id, "report"});
  EXPECT_TRUE(std::filesystem::exists(dir / "report.html"));
  std::ifstream in(dir / "report.json");
  EXPECT_EQ(Json::parse(in).at("part").at("name"), "Box");

  // A second project can reference the dataset of the first without copying it.
  auto other = Project::create(dir_ / "q", "Reference");
  const auto dataset = project.resolve({imported.id, "dataset"});
  const Step opened = other.run(registry, "open_dataset", {{"path", dataset.string()}});
  EXPECT_EQ(other.resolve({opened.id, "dataset"}), dataset);
  EXPECT_THROW(other.run(registry, "open_dataset", {{"path", (dir_ / "missing").string()}}),
               std::exception);
}

TEST_F(ProjectTest, PluginsAddOperations) {
  OperationRegistry registry;
  registerBuiltinOperations(registry);
  const auto messages = registry.loadPlugins(VOXELSIEVE_TEST_PLUGIN_DIR);
  ASSERT_EQ(messages.size(), 1U);
  EXPECT_NE(messages.front().find("loaded"), std::string::npos) << messages.front();
  ASSERT_NE(registry.find("histogram"), nullptr);

  std::filesystem::create_directories(dir_ / "bad");
  std::ofstream(dir_ / "bad" / (std::string("not_a_plugin") + kPluginExtension)) << "text";
  const auto bad = registry.loadPlugins(dir_ / "bad");
  ASSERT_EQ(bad.size(), 1U);
  EXPECT_EQ(bad.front().find("loaded"), std::string::npos);

  const SyntheticSpec spec;
  const SyntheticScan scan(boxMesh({4.0, 4.0, 4.0}), spec);
  std::filesystem::create_directories(dir_);
  writeRaw(dir_ / "scan.raw", scan);
  writeJson(dir_ / "scan.json", scan.toJson());
  auto project = Project::create(dir_ / "p", "Plugin");
  project.run(registry, "import_raw", {{"path", (dir_ / "scan.raw").string()}});
  const Step histogram = project.run(registry, "histogram", {{"bins", 16}, {"level", 0}});
  std::ifstream in(project.resolve({histogram.id, "histogram"}));
  const Json json = Json::parse(in);
  std::uint64_t total = 0;
  for (const Json& count : json.at("counts")) {
    total += count.get<std::uint64_t>();
  }
  EXPECT_EQ(total, histogram.summary.at("voxels").get<std::uint64_t>());
  EXPECT_GT(total, 0U);
}

}  // namespace
}  // namespace voxelsieve
