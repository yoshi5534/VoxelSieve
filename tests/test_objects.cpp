// Objects of a project and their poses in the global coordinate system (ADR 0018).

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "voxelsieve/mesh.hpp"
#include "voxelsieve/operation.hpp"
#include "voxelsieve/project.hpp"

namespace voxelsieve {
namespace {

using Json = nlohmann::json;
using Vec = std::array<double, 3>;

/// Stands in for an import: writes a file as a dataset output, so no volume is needed.
class FakeImport final : public Operation {
 public:
  FakeImport() {
    info_.id = "fake_import";
    info_.title = "Fake import";
    info_.outputs = {{"dataset", artifact::kDataset, ""}};
    info_.parameters = {{"type", "object"},
                        {"properties", {{"path", {{"type", "string"}}}}},
                        {"required", {"path"}}};
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }
  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    std::ofstream(context.output_dir / "data") << context.params.at("path").get<std::string>();
    return {{{"dataset", "data"}}, {}};
  }

 private:
  OperationInfo info_;
};

/// Works on a dataset and records which one, and the objects it was shown.
class Analyse final : public Operation {
 public:
  Analyse() {
    info_.id = "analyse";
    info_.title = "Analyse";
    info_.inputs = {{"dataset", artifact::kDataset, ""}};
    info_.outputs = {{"result", "result", ""}};
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }
  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    std::ifstream in(context.inputs.at("dataset"));
    std::string source;
    in >> source;
    std::ofstream(context.output_dir / "r") << source;
    return {{{"result", "r"}}, {{"source", source}, {"objects", context.objects.size()}}};
  }

 private:
  OperationInfo info_;
};

void expectNear(const Vec& actual, const Vec& expected) {
  for (std::size_t i = 0; i < 3; ++i) {
    EXPECT_NEAR(actual[i], expected[i], 1e-9) << "axis " << i;
  }
}

class ObjectsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("voxelsieve_objects_" +
            std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
    registerBuiltinOperations(registry_);
    registry_.add(std::make_shared<FakeImport>());
    registry_.add(std::make_shared<Analyse>());
    writeStl(dir_ / "nominal.stl", boxMesh({4.0, 2.0, 1.0}));
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  std::filesystem::path dir_;
  OperationRegistry registry_;
};

TEST_F(ObjectsTest, StepsThatMakeDataOfNoObjectCreateObjects) {
  auto project = Project::create(dir_ / "p", "Objects");
  EXPECT_TRUE(project.objects().empty());
  project.run(registry_, "fake_import", {{"path", "/scans/part_a.raw"}});
  project.run(registry_, "add_mesh", {{"path", (dir_ / "nominal.stl").string()}}, {}, {}, nullptr,
              {.object = {}, .name = "CAD"});
  project.run(registry_, "fake_import", {{"path", "/scans/part_b.vsieve/"}});

  const auto objects = project.objects();
  ASSERT_EQ(objects.size(), 3U);
  EXPECT_EQ(objects[0].id, "o1");
  EXPECT_EQ(objects[0].name, "part_a");
  EXPECT_EQ(objects[0].kind, kVolumeObject);
  EXPECT_EQ(objects[0].source.step, 1);
  EXPECT_EQ(objects[1].name, "CAD");
  EXPECT_EQ(objects[1].kind, kMeshObject);
  EXPECT_EQ(project.steps()[1].summary.at("triangles"), 12);
  EXPECT_EQ(objects[2].name, "part_b");
  for (const ProjectObject& object : objects) {
    EXPECT_EQ(object.pose.matrix(), RigidTransform().matrix());  // placed at the origin
  }

  // A step on data belongs to the object of its input; it creates no object.
  const Step analysed = project.run(registry_, "analyse");
  EXPECT_EQ(analysed.object, "o3");
  EXPECT_EQ(analysed.summary.at("source"), "/scans/part_b.vsieve/");
  EXPECT_EQ(analysed.summary.at("objects"), 3);
  EXPECT_EQ(project.objects().size(), 3U);
  EXPECT_THROW(project.run(registry_, "analyse", {}, {}, {}, nullptr, {.object = {}, .name = "x"}),
               std::invalid_argument);

  // Objects survive reopening; undoing the creating step removes the object.
  auto reopened = Project::open(dir_ / "p");
  EXPECT_EQ(reopened.objects().size(), 3U);
  EXPECT_EQ(reopened.object("o2").name, "CAD");
  reopened.undo();
  reopened.undo();
  EXPECT_EQ(reopened.objects().size(), 2U);
  EXPECT_THROW((void)reopened.object("o3"), std::invalid_argument);
  // A new history never reuses the id of a discarded object.
  reopened.run(registry_, "fake_import", {{"path", "c.raw"}});
  EXPECT_EQ(reopened.objects().back().id, "o4");
}

TEST_F(ObjectsTest, InputsComeFromTheTargetOrActiveObject) {
  auto project = Project::create(dir_ / "p", "Objects");
  project.run(registry_, "fake_import", {{"path", "a.raw"}});
  project.run(registry_, "fake_import", {{"path", "b.raw"}});
  project.run(registry_, "add_mesh", {{"path", (dir_ / "nominal.stl").string()}});

  // Without a choice, the latest dataset of any object.
  EXPECT_EQ(project.run(registry_, "analyse").summary.at("source"), "b.raw");
  // An explicit object.
  const Step on_a =
      project.run(registry_, "analyse", {}, {}, {}, nullptr, {.object = "o1", .name = {}});
  EXPECT_EQ(on_a.summary.at("source"), "a.raw");
  EXPECT_EQ(on_a.object, "o1");
  EXPECT_THROW(project.run(registry_, "analyse", {}, {}, {}, nullptr, {.object = "o3", .name = {}}),
               std::invalid_argument);  // a mesh has no dataset
  EXPECT_THROW(project.run(registry_, "analyse", {}, {}, {}, nullptr, {.object = "o9", .name = {}}),
               std::invalid_argument);

  // The active object comes first; one without the input falls back to the latest.
  project.selectObject("o1");
  EXPECT_EQ(project.activeObject(), "o1");
  EXPECT_EQ(project.run(registry_, "analyse").summary.at("source"), "a.raw");
  project.selectObject("o3");
  EXPECT_EQ(project.run(registry_, "analyse").summary.at("source"), "b.raw");
  EXPECT_THROW(project.selectObject("o9"), std::invalid_argument);
  EXPECT_EQ(Project::open(dir_ / "p").activeObject(), "o3");
}

TEST_F(ObjectsTest, MovesApplyInOrderAndUndoTakesThemBack) {
  auto project = Project::create(dir_ / "p", "Objects");
  project.run(registry_, "fake_import", {{"path", "a.raw"}});
  project.run(registry_, "add_mesh", {{"path", (dir_ / "nominal.stl").string()}});

  const Vec t{1.0, 2.0, 3.0};
  const Step first =
      project.run(registry_, "move", {{"objects", {"o1"}}, {"translation_mm", {t[0], t[1], t[2]}}});
  EXPECT_TRUE(first.object.empty());
  EXPECT_TRUE(std::filesystem::exists(project.resolve({first.id, "pose"})));
  // Both together: 90 degrees about z through (1, 0, 0).
  project.run(registry_, "move",
              {{"objects", {"o1", "o2"}}, {"rotation_deg", 90.0}, {"center_mm", {1.0, 0.0, 0.0}}});
  const auto turn = [](const Vec& p) { return Vec{1.0 - p[1], p[0] - 1.0, p[2]}; };

  const Vec p{0.5, -0.25, 2.0};
  auto objects = project.objects();
  expectNear(objects[0].pose.apply(p), turn({p[0] + t[0], p[1] + t[1], p[2] + t[2]}));
  expectNear(objects[1].pose.apply(p), turn(p));
  EXPECT_EQ(objects[0].moved_by, (std::vector<int>{3, 4}));
  EXPECT_EQ(objects[1].moved_by, (std::vector<int>{4}));

  // A pose places one object absolutely, on top of what came before.
  const RigidTransform placed = RigidTransform::fromAxisAngle({1.0, 1.0, 0.0}, 30.0, {5, 6, 7});
  const auto matrix = placed.matrix();
  project.run(registry_, "move",
              {{"objects", {"o2"}}, {"pose", std::vector<double>(matrix.begin(), matrix.end())}});
  expectNear(project.object("o2").pose.apply(p), placed.apply(p));
  expectNear(project.object("o1").pose.apply(p), objects[0].pose.apply(p));

  project.undo();
  project.undo();
  objects = project.objects();
  expectNear(objects[0].pose.apply(p), {p[0] + t[0], p[1] + t[1], p[2] + t[2]});
  expectNear(objects[1].pose.apply(p), p);
  // Poses are derived from the steps, so they survive reopening.
  project.redo();
  expectNear(Project::open(dir_ / "p").object("o2").pose.apply(p), turn(p));

  EXPECT_THROW(project.run(registry_, "move", {{"objects", {"o7"}}}), std::invalid_argument);
  EXPECT_THROW(project.run(registry_, "move", {{"objects", {"o1", "o1"}}}), std::invalid_argument);
  EXPECT_THROW(project.run(registry_, "move",
                           {{"objects", {"o1", "o2"}},
                            {"pose", std::vector<double>(matrix.begin(), matrix.end())}}),
               std::invalid_argument);
}

TEST_F(ObjectsTest, ProjectsWithoutObjectsAreMigrated) {
  // A format 1 project: import, porosity, a second import, a report on the first.
  const auto p = dir_ / "old";
  std::filesystem::create_directories(p / "steps");
  const auto step = [](int id, const std::string& operation, const Json& params, const Json& inputs,
                       const std::string& type) {
    return Json{{"id", id},
                {"operation", operation},
                {"params", params},
                {"inputs", inputs},
                {"outputs", {{"out", {{"path", "out"}, {"type", type}}}}},
                {"status", "done"}};
  };
  const Json from = {{"in", {{"step", 1}, {"output", "out"}}}};
  std::ofstream(p / "project.json") << Json{
      {"format", 1},
      {"name", "old"},
      {"cursor", 4},
      {"steps",
       {step(1, "import_raw", {{"path", "/data/housing.raw"}}, Json::object(), "dataset"),
        step(2, "porosity", Json::object(), from, "porosity"),
        step(3, "open_dataset", {{"path", "/data/bracket.vsieve"}}, Json::object(), "dataset"),
        step(4, "report", Json::object(), {{"in", {{"step", 2}, {"output", "out"}}}},
             "report")}}}.dump();

  const auto project = Project::open(p);
  const auto objects = project.objects();
  ASSERT_EQ(objects.size(), 2U);
  EXPECT_EQ(objects[0].name, "housing");
  EXPECT_EQ(objects[1].name, "bracket");
  EXPECT_EQ(project.steps()[1].object, "o1");
  EXPECT_EQ(project.steps()[3].object, "o1");
  EXPECT_EQ(project.steps()[2].object, "o2");
  std::ifstream saved(p / "project.json");
  EXPECT_EQ(Json::parse(saved).at("format"), 2);
}

}  // namespace
}  // namespace voxelsieve
