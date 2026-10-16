#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <map>
#include <numbers>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "voxelsieve/dataset.hpp"
#include "voxelsieve/gui.hpp"
#include "voxelsieve/io.hpp"
#include "voxelsieve/mcp.hpp"
#include "voxelsieve/mesh.hpp"
#include "voxelsieve/model.hpp"
#include "voxelsieve/slice.hpp"
#include "voxelsieve/synthetic.hpp"

namespace voxelsieve {
namespace {

using Json = nlohmann::json;

/// Reports progress until it is cancelled, like a long import of a large scan.
class Spin final : public Operation {
 public:
  Spin() {
    info_.id = "spin";
    info_.title = "Spin";
    info_.outputs = {{"table", "table", ""}};
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }
  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    for (;;) {  // progress throws once the operation is cancelled
      context.progress(0.5);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  }

 private:
  OperationInfo info_;
};

class GuiTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("voxelsieve_gui_" +
            std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
    SyntheticSpec spec;
    spec.lunker_count = 2;
    spec.lunker_radius_mm = 0.5;
    const SyntheticScan scan(boxMesh({6.0, 5.0, 4.0}), spec);
    writeRaw(dir_ / "scan.raw", scan);
    writeJson(dir_ / "scan.json", scan.toJson());
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  std::filesystem::path dir_;
};

TEST_F(GuiTest, ObjectsAreAddedSelectedAndMovedThroughTheApi) {
  Gui gui;
  gui.call("project_create", {{"path", (dir_ / "p").string()}});
  writeStl(dir_ / "nominal.stl", boxMesh({6.0, 5.0, 4.0}));

  const Json imported = gui.call(
      "run_import_raw",
      {{"path", (dir_ / "scan.raw").string()}, {"brick_size", 32}, {"object_name", "Scan"}});
  EXPECT_EQ(imported.at("object"), "o1");
  const Json dataset = gui.call(
      "object_add", {{"path", (dir_ / "p" / "steps" / "1-import_raw" / "dataset.vsieve").string()},
                     {"name", "Again"}});
  EXPECT_EQ(dataset.at("operation"), "open_dataset");
  const Json mesh = gui.call("object_add", {{"path", (dir_ / "nominal.stl").string()}});
  EXPECT_EQ(mesh.at("operation"), "add_mesh");
  EXPECT_THROW(gui.call("object_add", {{"path", (dir_ / "scan.json").string()}}),
               std::invalid_argument);

  Json objects = gui.call("objects", {});
  ASSERT_EQ(objects.at("objects").size(), 3U);
  EXPECT_EQ(objects.at("objects")[0].at("name"), "Scan");
  EXPECT_EQ(objects.at("objects")[1].at("name"), "Again");
  EXPECT_EQ(objects.at("objects")[2].at("name"), "nominal");
  EXPECT_EQ(objects.at("objects")[2].at("kind"), "mesh");
  EXPECT_EQ(objects.at("active_object"), "");

  // Steps take their inputs from the chosen object; a mesh has no dataset.
  EXPECT_EQ(gui.call("object_select", {{"id", "o1"}}).at("active_object"), "o1");
  const Json porosity = gui.call("run_porosity", {});
  EXPECT_EQ(porosity.at("inputs").at("dataset").at("step"), imported.at("id"));
  EXPECT_EQ(porosity.at("object"), "o1");
  EXPECT_THROW(gui.call("run_porosity", {{"object", "o3"}}), std::invalid_argument);
  EXPECT_THROW(gui.call("object_select", {{"id", "o9"}}), std::invalid_argument);

  const Json moved =
      gui.call("run_move", {{"objects", {"o2", "o3"}}, {"translation_mm", {10.0, 0.0, -2.5}}});
  EXPECT_EQ(moved.at("summary").at("moved_objects"), Json({"o2", "o3"}));
  objects = gui.call("project_status", {}).at("objects");
  EXPECT_EQ(objects[0].at("pose")[3], 0.0);
  EXPECT_EQ(objects[2].at("pose")[3], 10.0);
  EXPECT_EQ(objects[2].at("pose")[11], -2.5);
  EXPECT_EQ(objects[2].at("moved_by"), Json::array({moved.at("id")}));
  gui.call("undo", {});
  EXPECT_EQ(gui.call("objects", {}).at("objects")[2].at("pose")[3], 0.0);

  // What the 3D view draws: the box a volume fills until it has a surface, a mesh as it is.
  Gui::ObjectShape shape = gui.objectShape("o1", 100000);
  EXPECT_EQ(shape.shape, "box");
  EXPECT_EQ(shape.mesh->triangles.size(), 12U);
  EXPECT_NEAR(shape.scale[2], 0.1, 1e-12);
  EXPECT_EQ(gui.objectShape("o3", 100000).shape, "mesh");
  gui.call("run_surface", {{"object", "o1"}});
  shape = gui.objectShape("o1", 100000);
  EXPECT_EQ(shape.shape, "surface");
  EXPECT_EQ(gui.objectShape("o2", 100000).shape, "box");  // the surface is o1's
  float low = 1e9F;
  float high = -1e9F;
  for (const auto& p : shape.mesh->points) {
    low = std::min(low, p[0]);
    high = std::max(high, p[0]);
  }
  EXPECT_NEAR((high - low) * shape.scale[0], 6.0, 0.2);  // the 6 mm of the part along x

  const Json view = gui.call("view_objects", {{"width", 200}, {"height", 150}});
  ASSERT_EQ(view.at("objects").size(), 3U);
  EXPECT_EQ(view.at("objects")[0].at("shape"), "surface");
  EXPECT_EQ(view.at("objects")[2].at("shape"), "mesh");
  EXPECT_NE(view.at("objects")[0].at("color"), view.at("objects")[2].at("color"));
  EXPECT_NEAR(view.at("bounds_min_mm")[0].get<double>(), -3.0, 1e-6);  // the mesh, centred
  EXPECT_EQ(view.at("width"), 200);
  EXPECT_EQ(view.at("image").at("mime_type"), "image/png");
  EXPECT_THROW(gui.call("view_objects", {{"objects", {"o9"}}}), std::invalid_argument);

  const auto names = [&gui] {
    std::vector<std::string> list;
    for (const GuiMethod& method : gui.methods()) {
      list.push_back(method.name);
    }
    return list;
  }();
  for (const char* method :
       {"objects", "object_add", "object_select", "view_objects", "run_move", "run_add_mesh",
        "run_align_points", "run_align_surfaces", "run_compare_objects"}) {
    EXPECT_NE(std::ranges::find(names, method), names.end()) << method;
  }
}

TEST_F(GuiTest, SliceViewShowsOtherObjectsInThePlane) {
  Gui gui;
  gui.call("project_create", {{"path", (dir_ / "p").string()}});
  writeStl(dir_ / "nominal.stl", boxMesh({6.0, 5.0, 4.0}));
  const Json imported =
      gui.call("run_import_raw", {{"path", (dir_ / "scan.raw").string()}, {"brick_size", 32}});
  const int step = imported.at("id").get<int>();
  gui.call("object_add",
           {{"path", (dir_ / "p" / "steps" / "1-import_raw" / "dataset.vsieve").string()}});
  gui.call("object_add", {{"path", (dir_ / "nominal.stl").string()}});
  // The copy three voxels further along x.
  constexpr std::int64_t kShift = 3;
  const Dataset dataset = Dataset::open(dir_ / "p" / "steps" / "1-import_raw" / "dataset.vsieve");
  const double pitch = dataset.info().voxel_size[0];
  gui.call("run_move", {{"objects", {"o2"}},
                        {"translation_mm", {static_cast<double>(kShift) * pitch, 0.0, 0.0}}});

  SliceRequest request;
  request.axis = 2;
  request.index = dataset.info().dims[2] / 2;
  request.size = {dataset.info().dims[0], dataset.info().dims[1]};
  const SliceImage base = readSlice(dataset, request);
  const PlaneImage copy = gui.objectSliceTile(step, request, "o2");
  ASSERT_EQ(copy.width, base.width);
  ASSERT_EQ(copy.height, base.height);
  int compared = 0;
  for (std::int64_t y = 0; y < base.height; ++y) {
    for (std::int64_t x = 0; x < base.width; ++x) {
      const auto i = static_cast<std::size_t>(y * base.width + x);
      if (x < kShift) {
        EXPECT_EQ(copy.inside[i], 0);
        continue;
      }
      const auto j = i - static_cast<std::size_t>(kShift);
      ASSERT_EQ(copy.grey[i], base.grey[j]) << x << ", " << y;
      compared += copy.inside[i] == kPlaneMaterial ? 1 : 0;
    }
  }
  EXPECT_GT(compared, 0);
  EXPECT_THROW((void)gui.objectSliceTile(step, request, "o3"), std::invalid_argument);

  // The CAD box is centred on the origin of the project, where the scan starts: in voxels of the
  // scan its outline runs from -30 to 30 along x and -25 to 25 along y.
  const std::vector<float> cuts = gui.objectCutLines(step, 2, 0, "o3");
  ASSERT_FALSE(cuts.empty());
  double length = 0.0;
  for (std::size_t s = 0; s + 3 < cuts.size(); s += 4) {
    length += std::hypot(cuts[s + 2] - cuts[s], cuts[s + 3] - cuts[s + 1]);
    for (std::size_t k = 0; k < 4; k += 2) {
      EXPECT_LE(std::abs(cuts[s + k]), 30.0F + 1e-3F);
      EXPECT_LE(std::abs(cuts[s + k + 1]), 25.0F + 1e-3F);
    }
  }
  EXPECT_NEAR(length, 2.0 * (60.0 + 50.0), 1e-2);
  EXPECT_TRUE(gui.objectCutLines(step, 2, 100, "o3").empty());  // 10 mm above the box
  EXPECT_THROW((void)gui.objectCutLines(step, 2, 0, "o9"), std::invalid_argument);

  // At z = 1 mm both show: the copy blended in its colour and the box as lines.
  const Json slice = gui.call(
      "view_slice", {{"step", step}, {"axis", "z"}, {"index", 10}, {"objects", {"o2", "o3"}}});
  ASSERT_EQ(slice.at("objects").size(), 2U);
  EXPECT_EQ(slice.at("objects")[0].at("id"), "o2");
  EXPECT_TRUE(slice.at("objects")[0].at("volume_blended").get<bool>());
  EXPECT_FALSE(slice.at("objects")[1].at("volume_blended").get<bool>());
  EXPECT_GT(slice.at("objects")[1].at("cut_segments").get<int>(), 0);
  EXPECT_NE(slice.at("objects")[0].at("color"), slice.at("objects")[1].at("color"));

  // Without a step, the views show the data of the active object, else the latest.
  EXPECT_NE(gui.call("view_slice", {{"axis", "z"}}).at("step"), step);
  gui.call("object_select", {{"id", "o1"}});
  EXPECT_EQ(gui.call("view_slice", {{"axis", "z"}}).at("step"), step);
}

TEST_F(GuiTest, ApiRunsTheWorkflowAndReportsTheProtocol) {
  Gui gui({VOXELSIEVE_TEST_PLUGIN_DIR});
  EXPECT_FALSE(gui.call("project_status", {}).at("open"));
  EXPECT_THROW(gui.call("run_porosity", {}), std::invalid_argument);  // no project yet

  const Json created = gui.call("project_create", {{"path", (dir_ / "p").string()}});
  EXPECT_EQ(created.at("name"), "p");
  EXPECT_FALSE(created.at("can_undo"));

  const Json imported =
      gui.call("run_import_raw", {{"path", (dir_ / "scan.raw").string()}, {"brick_size", 32}});
  EXPECT_EQ(imported.at("status"), "done");
  EXPECT_GT(imported.at("size_bytes").get<std::uint64_t>(), 0U);

  const Json info = gui.call("dataset_info", {});
  EXPECT_EQ(info.at("step"), imported.at("id"));
  EXPECT_FALSE(info.at("levels").empty());

  std::vector<double> progress;
  const Json porosity =
      gui.call("run_porosity", {}, [&progress](double value) { progress.push_back(value); });
  EXPECT_EQ(porosity.at("summary").at("pores"), 2);
  EXPECT_FALSE(progress.empty());

  // A slice as PNG with the overlay of the porosity step, at a level that fits max_pixels.
  const Json slice = gui.call("view_slice", {{"axis", "x"}, {"max_pixels", 64}});
  EXPECT_EQ(slice.at("porosity_step"), porosity.at("id"));
  EXPECT_EQ(slice.at("right"), "y");
  EXPECT_EQ(slice.at("down"), "z");
  EXPECT_LE(std::max(slice.at("width").get<int>(), slice.at("height").get<int>()), 64);
  EXPECT_GT(slice.at("level").get<int>(), 0);
  EXPECT_TRUE(slice.at("image").at("base64").get<std::string>().starts_with("iVBORw0KGgo"));
  EXPECT_TRUE(
      gui.call("view_slice", {{"overlay", false}, {"index", 3}}).at("porosity_step").is_null());
  EXPECT_THROW(gui.call("view_slice", {{"index", 100000}}), std::invalid_argument);

  const Json files = gui.call("list_files", {});
  bool has_json = false;
  for (const Json& file : files.at("files")) {
    has_json = has_json || file.at("file") == "porosity.json";
  }
  EXPECT_TRUE(has_json);
  const Json text = gui.call("read_file", {{"file", "porosity.json"}});
  EXPECT_EQ(Json::parse(text.at("text").get<std::string>()).at("pores").size(), 2U);
  EXPECT_THROW(gui.call("read_file", {{"file", "../../../project.json"}}), std::invalid_argument);
  // Pictures come back as images; other binary files are refused.
  EXPECT_EQ(gui.call("read_file", {{"file", "projection_z.png"}}).at("image").at("mime_type"),
            "image/png");
  EXPECT_THROW(gui.call("read_file", {{"file", "analysis.vdb"}}), std::invalid_argument);

  // The surface of the box: 6 x 5 x 4 mm, lunkers inside, as mask, images and mesh.
  const Json surface = gui.call("run_surface", {{"stl", true}});
  EXPECT_EQ(surface.at("status"), "done");
  EXPECT_NEAR(surface.at("summary").at("surface_volume_mm3").get<double>(), 120.0, 1.5);
  EXPECT_GT(surface.at("summary").at("compression_vs_raw").get<double>(), 10.0);
  const Json surface_files = gui.call("list_files", {});
  std::vector<std::string> names;
  for (const Json& file : surface_files.at("files")) {
    names.push_back(file.at("file").get<std::string>());
  }
  for (const char* name : {"surface.vss", "surface.json", "surface.stl", "surface_z.png"}) {
    EXPECT_NE(std::find(names.begin(), names.end(), name), names.end()) << name;
  }

  // Nominal-actual comparison with the box as CAD model, placed elsewhere.
  writeStl(dir_ / "cad.stl", boxMesh({6.0, 5.0, 4.0}, {10.0, -3.0, 2.0}));
  const Json compared = gui.call("run_compare_cad", {{"cad_path", (dir_ / "cad.stl").string()}});
  EXPECT_EQ(compared.at("status"), "done");
  EXPECT_GT(compared.at("summary").at("within_tolerance_percent").get<double>(), 90.0);
  EXPECT_LT(std::abs(compared.at("summary").at("deviation_mean_mm").get<double>()), 0.02);
  const auto deviation = gui.deviationMesh(std::nullopt);
  EXPECT_FALSE(deviation->mesh.triangles.empty());
  EXPECT_EQ(deviation->deviation_mm.size(), deviation->mesh.points.size());
  EXPECT_DOUBLE_EQ(deviation->tolerance_mm, 0.1);
  EXPECT_EQ(gui.deviationMesh(std::nullopt), deviation);  // cached
  EXPECT_TRUE(deviation->displacement_mm.empty());
  EXPECT_FALSE(compared.at("summary").contains("displacement_max_mm"));

  // The same with a non-rigid registration: the box keeps its shape, so it hardly moves.
  const Json bent =
      gui.call("run_compare_cad", {{"cad_path", (dir_ / "cad.stl").string()}, {"non_rigid", true}});
  EXPECT_EQ(bent.at("status"), "done");
  EXPECT_LT(bent.at("summary").at("displacement_max_mm").get<double>(), 0.05);
  EXPECT_GT(bent.at("summary").at("within_tolerance_percent").get<double>(), 90.0);
  const auto bent_view = gui.deviationMesh(std::nullopt);
  EXPECT_EQ(bent_view->displacement_mm.size(), bent_view->mesh.points.size());
  EXPECT_GT(bent_view->displacement_range_mm, 0.0);
  EXPECT_THROW((void)gui.call("run_compare_cad", {{"cad_path", (dir_ / "none.stl").string()}}),
               std::exception);

  // The report picks up the surface and the comparison for its 3D views.
  const Json reported = gui.call("run_report", {});
  EXPECT_EQ(reported.at("status"), "done");
  EXPECT_EQ(reported.at("inputs").at("surface").at("step"), surface.at("id"));
  EXPECT_EQ(reported.at("inputs").at("comparison").at("step"), bent.at("id"));
  std::ifstream report_file(dir_ / "p" / "steps" /
                            (std::to_string(reported.at("id").get<int>()) + "-report") / "report" /
                            "report.html");
  const std::string html{std::istreambuf_iterator<char>(report_file),
                         std::istreambuf_iterator<char>()};
  EXPECT_NE(html.find("Nominal-actual comparison"), std::string::npos);
  EXPECT_NE(html.find("then non-rigid registration"), std::string::npos);
  EXPECT_NE(html.find("Displacement of the non-rigid registration"), std::string::npos);
  EXPECT_NE(html.find("Pores (red)"), std::string::npos);

  // Explicit inputs by step id.
  const Json histogram = gui.call(
      "run_histogram", {{"bins", 8}, {"inputs", {{"dataset", {{"step", imported.at("id")}}}}}});
  EXPECT_EQ(histogram.at("inputs").at("dataset").at("step"), imported.at("id"));

  // Every analysis is a result of the scanned object, with its files; the newer comparison
  // replaces the older one, and the report names what it builds on.
  const Json results = gui.call("results", {}).at("results");
  ASSERT_EQ(results.size(), 6U);
  std::vector<std::string> types;
  for (const Json& result : results) {
    types.push_back(result.at("type").get<std::string>());
    EXPECT_EQ(result.at("object"), imported.at("object"));
    EXPECT_EQ(result.at("source").at("step"), imported.at("id"));
    EXPECT_FALSE(result.at("files").empty());
  }
  EXPECT_EQ(types, (std::vector<std::string>{"porosity", "surface", "comparison", "comparison",
                                             "report", "table"}));
  EXPECT_EQ(results[0].at("summary").at("pores"), 2);
  EXPECT_FALSE(results[2].at("current"));
  EXPECT_TRUE(results[3].at("current"));
  EXPECT_EQ(results[4].at("based_on").size(), 3U);
  EXPECT_EQ(gui.call("results", {{"current_only", true}}).at("results").size(), 5U);
  EXPECT_TRUE(gui.call("results", {{"object", "o9"}}).at("results").empty());
  EXPECT_EQ(gui.call("project_status", {}).at("results").size(), 6U);

  const Json undone = gui.call("undo", {});
  EXPECT_TRUE(undone.at("changed"));
  EXPECT_FALSE(undone.at("steps").back().at("active"));
  EXPECT_TRUE(gui.call("redo", {}).at("can_undo"));

  const Json reopened = [&] {
    Gui other;
    return other.call("project_open", {{"path", (dir_ / "p").string()}});
  }();
  EXPECT_EQ(reopened.at("steps").size(), 8U);

  EXPECT_THROW(gui.call("nope", {}), std::invalid_argument);
  EXPECT_THROW(gui.call("undo", Json::array()), std::invalid_argument);
  EXPECT_THROW(gui.call("project_open", {{"path", 3}}), std::invalid_argument);
}

TEST_F(GuiTest, ShowsCurrentTitlesForStepsOfOlderProjects) {
  {
    Gui gui;
    (void)gui.call("project_create", {{"path", (dir_ / "p").string()}});
    (void)gui.call("run_import_raw", {{"path", (dir_ / "scan.raw").string()}, {"brick_size", 32}});
  }
  // Projects saved before the UI was English stored German step titles.
  const auto project_file = dir_ / "p" / "project.json";
  Json project;
  {
    std::ifstream in(project_file);
    project = Json::parse(in);
  }
  project.at("steps").at(0).at("title") = "Rohdaten importieren";
  writeJson(project_file, project);

  Gui gui;
  const Json status = gui.call("project_open", {{"path", (dir_ / "p").string()}});
  EXPECT_EQ(status.at("steps").at(0).at("title"), "Import raw volume");
}

TEST_F(GuiTest, SegmentsMaterialsAndShowsThemInSlices) {
  Gui gui({VOXELSIEVE_TEST_PLUGIN_DIR});
  (void)gui.call("project_create", {{"path", (dir_ / "p").string()}});
  const Json imported =
      gui.call("run_import_raw", {{"path", (dir_ / "scan.raw").string()}, {"brick_size", 32}});
  // Name and colour as the histogram of the GUI defines them.
  const Json segmented =
      gui.call("run_segment_materials", {{"materials", 1},
                                         {"material_names", Json::array({"Aluminium"})},
                                         {"material_colors", Json::array({"#20a0ff"})}});
  ASSERT_EQ(segmented.at("status"), "done");
  // One material: the box of 6 x 5 x 4 mm with two lunkers of 0.5 mm radius.
  const Json& material = segmented.at("summary").at("materials").at(0);
  EXPECT_NEAR(material.at("volume_mm3").get<double>(),
              120.0 - 2 * 4.0 / 3.0 * std::numbers::pi * 0.125, 3.0);
  EXPECT_EQ(material.at("name"), "Aluminium");
  EXPECT_EQ(material.at("color"), Json::array({32, 160, 255}));

  const Json slice = gui.call("view_slice", {});
  EXPECT_EQ(slice.at("materials_step"), segmented.at("id"));
  EXPECT_EQ(slice.at("materials").size(), 1U);

  const Json info = gui.call("dataset_info", {});
  SliceRequest request;
  request.index = info.at("dims").at(2).get<std::int64_t>() / 2;
  request.size = {info.at("dims").at(0).get<std::int64_t>(),
                  info.at("dims").at(1).get<std::int64_t>()};
  const SliceImage tile = gui.sliceTile(imported.at("id").get<int>(), std::nullopt, request,
                                        segmented.at("id").get<int>());
  const auto material_pixels = std::count(tile.overlay.begin(), tile.overlay.end(),
                                          static_cast<std::uint8_t>(SliceOverlay::kMaterial) + 1);
  EXPECT_GT(material_pixels, 0);
  EXPECT_LT(material_pixels, static_cast<std::ptrdiff_t>(tile.overlay.size()));
}

TEST_F(GuiTest, HistogramSuggestsTheNumberOfMaterials) {
  Gui gui;
  (void)gui.call("project_create", {{"path", (dir_ / "p").string()}});
  (void)gui.call("run_import_raw", {{"path", (dir_ / "scan.raw").string()}, {"brick_size", 32}});
  const Json histogram = gui.call("histogram", {{"bins", 64}});
  EXPECT_EQ(histogram.at("level"), 0);
  ASSERT_EQ(histogram.at("counts").size(), 64U);
  std::uint64_t total = 0;
  for (const Json& count : histogram.at("counts")) {
    total += count.get<std::uint64_t>();
  }
  EXPECT_EQ(total, histogram.at("voxels").get<std::uint64_t>());
  EXPECT_EQ(total, gui.call("dataset_info", {}).at("active_voxels").get<std::uint64_t>());
  // The scan is one material, a box with lunkers.
  EXPECT_EQ(histogram.at("suggested_materials"), 1);
  EXPECT_TRUE(histogram.at("material_thresholds").empty());
  EXPECT_LE(histogram.at("clear_peaks").get<int>(), 1);
  ASSERT_EQ(histogram.at("mixture").size(), 1U);
  EXPECT_NEAR(histogram.at("mixture").at(0).at("share").get<double>(), 1.0, 0.01);
  EXPECT_GE(histogram.at("ambiguous_fraction").get<double>(), 0.0);
  EXPECT_EQ(histogram.at("image").at("mime_type"), "image/png");
  EXPECT_EQ(gui.call("histogram", {{"level", 1}}).at("level"), 1);

  // A preview of two classes split at the material's peak, before segmenting.
  const auto peak = histogram.at("material_peaks").at(0).get<float>();
  const Json preview =
      gui.call("view_slice", {{"preview_thresholds", Json::array({peak})}, {"max_pixels", 64}});
  ASSERT_EQ(preview.at("preview").size(), 2U);
  EXPECT_EQ(preview.at("preview").at(1).at("lower").get<float>(), peak);
  EXPECT_TRUE(preview.at("materials_step").is_null());
  const Json plain = gui.call("view_slice", {{"max_pixels", 64}});
  EXPECT_NE(preview.at("image").at("base64"), plain.at("image").at("base64"));
  EXPECT_THROW((void)gui.call("view_slice", {{"preview_thresholds", Json::array({2.0, 1.0})}}),
               std::invalid_argument);
  EXPECT_THROW((void)gui.call("histogram", {{"level", 99}}), std::invalid_argument);

  const Json segmented = gui.call("run_segment_materials", {{"materials", 0}});
  ASSERT_EQ(segmented.at("status"), "done");
  EXPECT_EQ(segmented.at("summary").at("materials").size(), 1U);
  EXPECT_EQ(segmented.at("summary").at("materials_from_histogram"), true);
}

TEST_F(GuiTest, ShowsAndExportsTheMaterialsIn3D) {
  Gui gui;
  (void)gui.call("project_create", {{"path", (dir_ / "p").string()}});
  (void)gui.call("run_import_raw", {{"path", (dir_ / "scan.raw").string()}, {"brick_size", 32}});
  const Json plain = gui.call("view_objects", {{"materials", true}});
  EXPECT_EQ(plain.at("objects").at(0).at("shape"), "box");  // no segmentation yet
  (void)gui.call("run_segment_materials",
                 {{"materials", 1}, {"material_colors", Json::array({"#20a0ff"})}});
  const Json view =
      gui.call("view_objects", {{"materials", true}, {"width", 200}, {"height", 160}});
  const Json& object = view.at("objects").at(0);
  EXPECT_EQ(object.at("shape"), "materials");
  ASSERT_EQ(object.at("materials").size(), 1U);
  EXPECT_EQ(object.at("materials").at(0).at("color"), Json::array({32, 160, 255}));
  EXPECT_GT(object.at("triangles").get<int>(), 0);
  // The box of 6 x 5 x 4 mm from the origin.
  EXPECT_NEAR(
      view.at("bounds_max_mm").at(0).get<double>() - view.at("bounds_min_mm").at(0).get<double>(),
      6.0, 0.3);
  // Cut open along x: about half of the triangles stay, the cut ones clipped at the plane.
  const Json cut = gui.call("view_objects", {{"materials", true},
                                             {"width", 200},
                                             {"height", 160},
                                             {"cut_axis", "x"},
                                             {"cut_keep", "above"}});
  EXPECT_LT(cut.at("triangles_shown").get<double>(),
            0.7 * view.at("triangles_shown").get<double>());
  EXPECT_GT(cut.at("triangles_shown").get<double>(),
            0.3 * view.at("triangles_shown").get<double>());
  EXPECT_THROW((void)gui.call("view_objects", {{"cut_axis", "x"}, {"cut_mm", -1e6}}),
               std::invalid_argument);
  EXPECT_THROW((void)gui.call("view_objects", {{"materials", true}, {"material_ids", {7}}}),
               std::invalid_argument);

  // Moved first, so the mesh objects show where the volume is.
  (void)gui.call("run_move", {{"objects", {"o1"}}, {"translation_mm", {10.0, 0.0, 0.0}}});
  const Json exported = gui.call("run_material_meshes", {});
  ASSERT_EQ(exported.at("status"), "done");
  // Each mesh became an object in its material's colour, at the volume's pose.
  ASSERT_EQ(exported.at("followed_by").size(), 1U);
  EXPECT_EQ(exported.at("followed_by").at(0).at("operation"), "add_mesh");
  const Json objects = gui.call("objects", {}).at("objects");
  ASSERT_EQ(objects.size(), 2U);
  EXPECT_EQ(objects.at(1).at("kind"), "mesh");
  EXPECT_EQ(objects.at(1).at("color"), "#20a0ff");
  EXPECT_EQ(objects.at(1).at("name"), exported.at("summary").at("meshes").at(0).at("name"));
  EXPECT_EQ(objects.at(1).at("pose"), objects.at(0).at("pose"));
  EXPECT_DOUBLE_EQ(objects.at(1).at("pose").at(3).get<double>(), 10.0);
  EXPECT_TRUE(objects.at(0).at("color").is_null());
  const Json shown = gui.call("view_objects", {{"objects", {objects.at(1).at("id")}}});
  EXPECT_EQ(shown.at("objects").at(0).at("color"), Json::array({32, 160, 255}));
  const Json& mesh = exported.at("summary").at("meshes").at(0);
  const Json files = gui.call("list_files", {{"step", exported.at("id")}});
  const auto stl = std::filesystem::path(files.at("path").get<std::string>()) /
                   mesh.at("file").get<std::string>();
  EXPECT_NEAR(meshVolumeMm3(readStl(stl)), 120.0 - 2 * 4.0 / 3.0 * std::numbers::pi * 0.125, 4.0);
  EXPECT_THROW((void)gui.call("run_material_meshes", {{"material_ids", {5}}}),
               std::invalid_argument);
}

TEST_F(GuiTest, SegmentsWithALearnedModel) {
  Gui gui;
  (void)gui.call("project_create", {{"path", (dir_ / "p").string()}});
  (void)gui.call("run_import_raw", {{"path", (dir_ / "scan.raw").string()}, {"brick_size", 32}});
  const auto threshold = gui.call("dataset_info", {}).at("threshold").get<float>();
  // A one-layer model: material where the grey value is above the dataset's threshold.
  Material material;
  material.id = 1;
  material.name = "Guss";
  material.lower = threshold;
  ModelSpec spec;
  spec.name = "Schwelle";
  spec.input_scale = 1e-3F;
  spec.materials = {material};
  spec.layers = {
      {"conv", "scores", {"input"}, 1, 2, 1, false, {0.0F, 1.0F}, {0.0F, -threshold * 1e-3F}}};
  writeModel(dir_ / "schwelle.vsm", spec);

  const Json segmented =
      gui.call("run_segment_model", {{"model_path", (dir_ / "schwelle.vsm").string()}});
  ASSERT_EQ(segmented.at("status"), "done");
  EXPECT_EQ(segmented.at("summary").at("model"), "Schwelle");
  const Json& result = segmented.at("summary").at("materials").at(0);
  EXPECT_EQ(result.at("name"), "Guss");
  EXPECT_NEAR(result.at("volume_mm3").get<double>(),
              120.0 - 2 * 4.0 / 3.0 * std::numbers::pi * 0.125, 3.0);
  const Json slice = gui.call("view_slice", {});
  EXPECT_EQ(slice.at("materials_step"), segmented.at("id"));
  EXPECT_THROW((void)gui.call("run_segment_model", {}), std::invalid_argument);
}

TEST_F(GuiTest, DenoisesIntoANewDatasetOfTheSameObject) {
  SyntheticSpec spec;
  spec.noise_sigma = 800.0;
  spec.noise_correlation_mm = 0.07;
  const SyntheticScan scan(boxMesh({6.0, 5.0, 4.0}), spec);
  writeRaw(dir_ / "noisy.raw", scan);
  writeJson(dir_ / "noisy.json", scan.toJson());
  Gui gui;
  (void)gui.call("project_create", {{"path", (dir_ / "p").string()}});
  const Json imported =
      gui.call("run_import_raw", {{"path", (dir_ / "noisy.raw").string()}, {"brick_size", 32}});
  const Json denoised = gui.call("run_denoise", {});
  ASSERT_EQ(denoised.at("status"), "done");
  EXPECT_EQ(denoised.at("object"), imported.at("object"));
  const Json& summary = denoised.at("summary");
  EXPECT_EQ(summary.at("method"), "non_local_means");
  EXPECT_NEAR(summary.at("noise_sigma").get<double>(), 800.0, 120.0);
  EXPECT_LT(summary.at("noise_sigma_after").get<double>(),
            0.5 * summary.at("noise_sigma").get<double>());
  EXPECT_GT(summary.at("noise_correlation").at(0).get<double>(), 0.4);
  EXPECT_EQ(summary.at("active_voxels"), imported.at("summary").at("active_voxels"));
  EXPECT_EQ(gui.call("objects", {}).at("objects").size(), 1U);
  // Later steps of the object take the denoised dataset.
  const Json porosity = gui.call("run_porosity", {});
  EXPECT_EQ(porosity.at("inputs").at("dataset").at("step"), denoised.at("id"));
  EXPECT_THROW((void)gui.call("run_denoise", {{"method", "model"}}), std::invalid_argument);
}

TEST_F(GuiTest, IsolatesThePartIntoANewDatasetOfTheSameObject) {
  Gui gui;
  (void)gui.call("project_create", {{"path", (dir_ / "p").string()}});
  const Json imported =
      gui.call("run_import_raw", {{"path", (dir_ / "scan.raw").string()}, {"brick_size", 32}});
  const Json dims = gui.call("dataset_info", {}).at("dims");
  const Json isolated =
      gui.call("run_isolate_part",
               {{"region_max", Json::array({dims.at(0).get<int>() / 2, dims.at(1), dims.at(2)})}});
  ASSERT_EQ(isolated.at("status"), "done");
  EXPECT_EQ(isolated.at("object"), imported.at("object"));
  const Json& summary = isolated.at("summary");
  EXPECT_EQ(summary.at("objects_found"), 1);
  EXPECT_GT(summary.at("removed_voxels").get<std::int64_t>(), 0);
  EXPECT_LT(summary.at("active_voxels").get<std::int64_t>(),
            imported.at("summary").at("active_voxels").get<std::int64_t>());
  // Later steps of the object take the isolated dataset.
  const Json porosity = gui.call("run_porosity", {});
  EXPECT_EQ(porosity.at("inputs").at("dataset").at("step"), isolated.at("id"));
  // Planes, a cylinder and separation as parameters.
  const Json cut =
      gui.call("run_isolate_part",
               {{"cut_planes", Json::array({{{"point", {0, 0, 10}}, {"normal", {0, 0, -1}}}})},
                {"cylinder", {{"axis", "z"}, {"center", {30, 25}}, {"radius", 1000}}},
                {"separate_voxels", 1}});
  ASSERT_EQ(cut.at("status"), "done");
  EXPECT_LT(cut.at("summary").at("active_voxels").get<std::int64_t>(),
            summary.at("active_voxels").get<std::int64_t>());
}

TEST_F(GuiTest, ViewsAreRestoredWhenTheProjectIsOpenedAgain) {
  const auto project = (dir_ / "p").string();
  const Json state = {{"stage", "view"}, {"mode", "3d"}, {"transfer", {{"points", Json::array()}}}};
  {
    Gui gui;
    (void)gui.call("project_create", {{"path", project}});
    (void)gui.call("view_set", {{"state", state}});
    // "iVBORw0KGgo=" is the PNG signature.
    const Json saved =
        gui.call("view_save", {{"name", "Aufsicht"}, {"image_base64", "iVBORw0KGgo="}});
    EXPECT_EQ(saved.at("state"), state);  // the current view by default
    EXPECT_THROW((void)gui.call("view_save", {{"name", "x"}, {"image_base64", "#?"}}),
                 std::invalid_argument);
  }
  Gui gui;
  const Json status = gui.call("project_open", {{"path", project}});
  EXPECT_EQ(status.at("view"), state);
  const Json views = gui.call("view_list", {}).at("views");
  ASSERT_EQ(views.size(), 1U);
  const Json image = gui.call("view_image", {{"id", views[0].at("id")}});
  EXPECT_EQ(image.at("image").at("mime_type"), "image/png");
  EXPECT_EQ(image.at("image").at("base64"), "iVBORw0KGgo=");
  const Json renamed = gui.call("view_rename", {{"id", views[0].at("id")}, {"name", "Seite"}});
  EXPECT_EQ(renamed.at("name"), "Seite");
}

TEST_F(GuiTest, EveryOperationIsAMethodWithItsSchema) {
  const Gui gui({VOXELSIEVE_TEST_PLUGIN_DIR});
  bool found = false;
  for (const GuiMethod& method : gui.methods()) {
    EXPECT_EQ(method.parameters.at("type"), "object") << method.name;
    if (method.name == "run_porosity") {
      found = true;
      EXPECT_TRUE(method.parameters.at("properties").contains("zone_sigma"));
      EXPECT_TRUE(method.parameters.at("properties").contains("inputs"));
      EXPECT_NE(method.description.find("dataset"), std::string::npos);
    }
  }
  EXPECT_TRUE(found);
}

Json request(int id, const std::string& method, const Json& params = Json::object()) {
  return {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}};
}

TEST_F(GuiTest, McpServesToolsOverJsonRpc) {
  Gui gui;
  std::vector<Json> sent;
  McpServer server(gui, [&sent](const Json& message) { sent.push_back(message); });
  const auto handle = [&server](const Json& message) {
    return server.handle(message).value_or(Json());
  };

  const auto init = handle(request(1, "initialize", {{"protocolVersion", "2024-11-05"}}));
  EXPECT_EQ(init.at("result").at("protocolVersion"), "2024-11-05");
  EXPECT_TRUE(init.at("result").at("capabilities").contains("tools"));
  EXPECT_EQ(handle(request(2, "initialize", {{"protocolVersion", "1999-01-01"}}))
                .at("result")
                .at("protocolVersion"),
            "2025-06-18");
  EXPECT_FALSE(
      server.handle({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}).has_value());

  // The analysis profile leaves out the six methods of the UI's saved views, list_operations and
  // object_add.
  const auto list = handle(request(3, "tools/list"));
  EXPECT_EQ(list.at("result").at("tools").size(), gui.methods().size() - 8);
  for (const Json& tool : list.at("result").at("tools")) {
    EXPECT_TRUE(tool.contains("inputSchema"));
    EXPECT_NE(tool.at("name"), "view_set");
    EXPECT_NE(tool.at("name"), "view_delete");
    EXPECT_NE(tool.at("name"), "list_operations");
    EXPECT_NE(tool.at("name"), "object_add");
    EXPECT_FALSE(tool.at("annotations").at("openWorldHint"));
    if (tool.at("name") == "dataset_info") {
      EXPECT_TRUE(tool.at("annotations").at("readOnlyHint"));
    }
    if (tool.at("name") == "run_porosity") {
      EXPECT_FALSE(tool.at("annotations").at("readOnlyHint"));
      EXPECT_FALSE(tool.at("annotations").at("destructiveHint"));
    }
  }
  const Json hidden =
      handle(request(30, "tools/call", {{"name", "view_set"}, {"arguments", {{"state", {}}}}}));
  EXPECT_TRUE(hidden.at("result").at("isError"));
  McpServer all(gui, [](const Json&) {}, McpTools::kAll);
  EXPECT_EQ(all.handle(request(31, "tools/list")).value_or(Json()).at("result").at("tools").size(),
            gui.methods().size());

  const Json created = handle(
      request(4, "tools/call",
              {{"name", "project_create"}, {"arguments", {{"path", (dir_ / "p").string()}}}}));
  EXPECT_FALSE(created.at("result").at("isError"));
  EXPECT_TRUE(created.at("result").at("structuredContent").at("open"));
  EXPECT_EQ(Json::parse(created.at("result").at("content")[0].at("text").get<std::string>()),
            created.at("result").at("structuredContent"));

  const Json imported = handle(
      request(5, "tools/call",
              {{"name", "run_import_raw"},
               {"arguments", {{"path", (dir_ / "scan.raw").string()}, {"brick_size", 32}}}}));
  EXPECT_FALSE(imported.at("result").at("isError"));
  const auto porosity = handle(request(6, "tools/call",
                                       {{"name", "run_porosity"},
                                        {"arguments", Json::object()},
                                        {"_meta", {{"progressToken", "t"}}}}));
  EXPECT_FALSE(porosity.at("result").at("isError"));
  ASSERT_FALSE(sent.empty());
  EXPECT_EQ(sent.front().at("method"), "notifications/progress");
  EXPECT_EQ(sent.front().at("params").at("progressToken"), "t");

  // Images come back as image content next to the JSON text.
  const Json slice =
      handle(request(11, "tools/call", {{"name", "view_slice"}, {"arguments", Json::object()}}));
  const Json& content = slice.at("result").at("content");
  ASSERT_EQ(content.size(), 2U);
  EXPECT_EQ(content[0].at("type"), "image");
  EXPECT_EQ(content[0].at("mimeType"), "image/png");
  EXPECT_FALSE(slice.at("result").at("structuredContent").contains("image"));

  // Failures are tool errors with the message, so a model can correct the call.
  const Json failed = handle(
      request(7, "tools/call", {{"name", "run_porosity"}, {"arguments", {{"zone_sigma", -1}}}}));
  EXPECT_TRUE(failed.at("result").at("isError"));
  EXPECT_NE(failed.at("result").at("content")[0].at("text").get<std::string>().find("zone_sigma"),
            std::string::npos);

  EXPECT_EQ(handle(request(8, "tools/call")).at("error").at("code"), -32602);
  EXPECT_EQ(handle(request(9, "sampling/createMessage")).at("error").at("code"), -32601);
  EXPECT_EQ(handle(Json::array()).at("error").at("code"), -32600);
  EXPECT_EQ(handle(request(10, "ping")).at("result"), Json::object());
}

// A whole session as a client such as Claude Desktop runs it, each request after the response to
// the one before: the porosity_check prompt's steps, then the results read back as resources. The
// pore count is scored against the ground truth of the synthetic scan.
TEST_F(GuiTest, McpSessionFindsThePoresOfASyntheticScan) {
  SyntheticSpec spec;
  spec.noise_sigma = 500.0;
  spec.lunker_count = 3;
  spec.lunker_radius_mm = 0.5;
  const SyntheticScan scan(boxMesh({10.0, 6.0, 4.0}), spec);
  ASSERT_EQ(scan.defects().size(), 3U);
  writeRaw(dir_ / "lunkers.raw", scan);
  writeJson(dir_ / "lunkers.json", scan.toJson());

  Gui gui;
  gui.setAllowedRoots({dir_});
  const auto call = [](int id, const std::string& tool, const Json& arguments) {
    return request(id, "tools/call", {{"name", tool}, {"arguments", arguments}});
  };
  McpServer server(gui, [](const Json&) {});
  const auto handle = [&server](const Json& message) {
    return server.handle(message).value_or(Json());
  };
  std::map<int, Json> responses;
  for (const Json& message : {
           request(1, "initialize", {{"protocolVersion", "2025-06-18"}}),
           Json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}},
           request(2, "prompts/get",
                   {{"name", "porosity_check"}, {"arguments", {{"scan", "lunkers.raw"}}}}),
           call(3, "project_create", {{"path", (dir_ / "p").string()}}),
           call(4, "run_import_raw", {{"path", (dir_ / "lunkers.raw").string()}}),
           call(5, "run_porosity", Json::object()),
           call(6, "run_report", Json::object()),
           request(7, "resources/list"),
       }) {
    if (const Json response = handle(message); !response.is_null()) {
      responses[response.at("id").get<int>()] = response;
    }
  }
  ASSERT_EQ(responses.size(), 7U);
  for (const auto& [id, response] : responses) {
    ASSERT_TRUE(response.contains("result")) << response.dump();
  }
  EXPECT_TRUE(responses[1].at("result").at("capabilities").contains("resources"));
  const std::string recipe =
      responses[2].at("result").at("messages")[0].at("content").at("text").get<std::string>();
  EXPECT_NE(recipe.find("lunkers.raw"), std::string::npos);
  EXPECT_NE(recipe.find("run_porosity"), std::string::npos);
  for (int id = 3; id <= 6; ++id) {
    EXPECT_FALSE(responses[id].at("result").at("isError")) << responses[id].dump();
  }
  EXPECT_EQ(responses[5].at("result").at("structuredContent").at("summary").at("pores"),
            scan.defects().size());

  // The results are resources: porosity.json with every pore, the report, the pictures.
  const Json& resources = responses[7].at("result").at("resources");
  const auto find = [&resources](const std::string& name) {
    const auto it = std::ranges::find_if(
        resources, [&name](const Json& resource) { return resource.at("name") == name; });
    return it == resources.end() ? Json() : *it;
  };
  ASSERT_FALSE(find("porosity.json").is_null()) << resources.dump(2);
  ASSERT_FALSE(find("report.html").is_null()) << resources.dump(2);
  ASSERT_FALSE(find("projection_z.png").is_null()) << resources.dump(2);
  EXPECT_EQ(find("report.html").at("mimeType"), "text/html");
  const Json porosity =
      handle(request(8, "resources/read", {{"uri", find("porosity.json").at("uri")}}));
  ASSERT_TRUE(porosity.contains("result")) << porosity.dump();
  const Json& content = porosity.at("result").at("contents")[0];
  EXPECT_EQ(content.at("mimeType"), "application/json");
  EXPECT_EQ(Json::parse(content.at("text").get<std::string>()).at("pores").size(),
            scan.defects().size());
  const Json picture =
      handle(request(9, "resources/read", {{"uri", find("projection_z.png").at("uri")}}));
  ASSERT_TRUE(picture.contains("result")) << picture.dump().substr(0, 300);
  EXPECT_FALSE(picture.at("result").at("contents")[0].at("blob").get<std::string>().empty());

  // Unknown resources and prompts, and prompts without their required arguments, are errors.
  for (const std::string uri :
       {"voxelsieve://step/99/porosity/porosity.json", "voxelsieve://step/x/a/b.json",
        "voxelsieve://step/3/porosity/../../project.json", "file:///etc/passwd"}) {
    EXPECT_EQ(handle(request(10, "resources/read", {{"uri", uri}})).at("error").at("code"), -32002)
        << uri;
  }
  EXPECT_EQ(handle(request(11, "prompts/get", {{"name", "nothing"}})).at("error").at("code"),
            -32602);
  EXPECT_EQ(
      handle(request(12, "prompts/get", {{"name", "compare_with_cad"}})).at("error").at("code"),
      -32602);
  EXPECT_EQ(handle(request(13, "prompts/list")).at("result").at("prompts").size(), 3U);
}

TEST_F(GuiTest, McpServeReadsLines) {
  Gui gui;
  std::istringstream in(request(1, "ping").dump() + "\n\nnot json\n" +
                        Json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}.dump() +
                        "\n" + request(2, "tools/list").dump() + "\n");
  std::ostringstream out;
  McpServer::serve(gui, in, out);
  std::istringstream lines(out.str());
  std::vector<Json> responses;
  for (std::string line; std::getline(lines, line);) {
    responses.push_back(Json::parse(line));
  }
  ASSERT_EQ(responses.size(), 3U);
  EXPECT_EQ(responses[0].at("id"), 1);
  EXPECT_EQ(responses[1].at("error").at("code"), -32700);
  EXPECT_EQ(responses[2].at("id"), 2);
}

}  // namespace
TEST_F(GuiTest, AllowedRootsKeepPathsInside) {
  Gui gui;
  std::filesystem::create_directories(dir_ / "data");
  std::filesystem::copy_file(dir_ / "scan.raw", dir_ / "data" / "scan.raw");
  std::filesystem::copy_file(dir_ / "scan.json", dir_ / "data" / "scan.json");
  gui.setAllowedRoots({dir_ / "data"});
  EXPECT_THROW(gui.setAllowedRoots({dir_ / "missing"}), std::invalid_argument);
  ASSERT_EQ(gui.allowedRoots().size(), 1U);

  // Browsing starts in the root and stays inside; "..", other directories and files are refused.
  EXPECT_EQ(std::filesystem::path(gui.call("browse", {}).at("path").get<std::string>()),
            std::filesystem::canonical(dir_ / "data"));
  EXPECT_THROW(gui.call("browse", {{"path", dir_.string()}}), std::invalid_argument);
  EXPECT_THROW(gui.call("browse", {{"path", (dir_ / "data" / "..").string()}}),
               std::invalid_argument);
  EXPECT_THROW(gui.call("project_create", {{"path", (dir_ / "p").string()}}),
               std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(dir_ / "p"));

  gui.call("project_create", {{"path", (dir_ / "data" / "p").string()}});
  EXPECT_THROW(gui.call("run_import_raw", {{"path", (dir_ / "scan.raw").string()}}),
               std::invalid_argument);
  const Json step = gui.call("run_import_raw",
                             {{"path", (dir_ / "data" / "scan.raw").string()}, {"brick_size", 32}});
  EXPECT_EQ(step.at("status"), "done");
}

TEST_F(GuiTest, CancelledOperationsFailAtTheirNextProgressReport) {
  OperationRegistry registry;
  registerBuiltinOperations(registry);
  Project project = Project::create(dir_ / "p", "p");
  const std::atomic<bool> cancel = true;
  EXPECT_THROW(
      project.run(registry, "import_raw",
                  {{"path", (dir_ / "scan.raw").string()}, {"brick_size", 32}}, {}, {}, &cancel),
      std::runtime_error);
  ASSERT_EQ(project.steps().size(), 1U);
  EXPECT_EQ(project.steps().front().status, "failed");
}

TEST_F(GuiTest, OperationsRunInTheBackgroundUntilDoneOrCancelled) {
  Gui gui;
  gui.registry().add(std::make_shared<Spin>());
  gui.call("project_create", {{"path", (dir_ / "p").string()}});
  EXPECT_EQ(gui.call("wait", {{"seconds", 0}}).at("done"), true);  // nothing runs

  const Json started =
      gui.call("run_import_raw",
               {{"path", (dir_ / "scan.raw").string()}, {"brick_size", 32}, {"background", true}});
  EXPECT_TRUE(started.at("background"));
  EXPECT_EQ(started.at("operation"), "import_raw");
  Json waited = gui.call("wait", {{"seconds", 50}});
  for (int i = 0; i < 20 && !waited.at("done").get<bool>(); ++i) {
    waited = gui.call("wait", {{"seconds", 50}});
  }
  ASSERT_TRUE(waited.at("done"));
  EXPECT_EQ(waited.at("result").at("operation"), "import_raw");
  EXPECT_EQ(waited.at("result").at("status"), "done");
  EXPECT_EQ(gui.call("project_status", {}).at("steps").size(), 1U);
  EXPECT_THROW(gui.call("wait", {{"seconds", 3600}}), std::invalid_argument);
  EXPECT_THROW(
      gui.call("run_import_raw", {{"path", (dir_ / "scan.raw").string()}, {"background", "yes"}}),
      std::invalid_argument);

  // A running operation is reported by wait and blocks further operations until cancelled.
  gui.call("run_spin", {{"background", true}});
  waited = gui.call("wait", {{"seconds", 0.05}});
  EXPECT_FALSE(waited.at("done"));
  EXPECT_EQ(waited.at("running").at("operation"), "spin");
  EXPECT_TRUE(waited.at("result").is_null());
  EXPECT_THROW(gui.call("run_spin", {}), std::invalid_argument);
  EXPECT_THROW(gui.call("undo", {}), std::invalid_argument);
  EXPECT_EQ(gui.call("cancel", {}).at("cancelled"), "spin");
  waited = gui.call("wait", {{"seconds", 50}});
  ASSERT_TRUE(waited.at("done"));
  EXPECT_EQ(waited.at("result").at("status"), "failed");
  EXPECT_EQ(waited.at("result").at("operation"), "spin");
  EXPECT_TRUE(gui.call("cancel", {}).at("cancelled").is_null());
  const Json steps = gui.call("project_status", {}).at("steps");
  ASSERT_EQ(steps.size(), 2U);
  EXPECT_EQ(steps[1].at("status"), "failed");

  // A pipeline runs its steps one after the other, in the background as well.
  const Json pipeline = gui.call(
      "pipeline", {{"steps",
                    {{{"operation", "segment_materials"}, {"params", {{"materials", 1}}}},
                     {{"operation", "material_meshes"}, {"params", {{"add_objects", false}}}}}}});
  EXPECT_EQ(pipeline.at("status"), "done");
  ASSERT_EQ(pipeline.at("steps").size(), 2U);
  EXPECT_EQ(pipeline.at("steps").at(1).at("operation"), "material_meshes");
  EXPECT_EQ(gui.call("objects", {}).at("objects").size(), 1U);  // no mesh objects
  (void)gui.call("pipeline",
                 {{"steps",
                   {{{"operation", "segment_materials"}, {"params", {{"materials", 1}}}},
                    {{"operation", "material_meshes"}}}},
                  {"background", true}});
  waited = gui.call("wait", {{"seconds", 50}});
  for (int i = 0; i < 20 && !waited.at("done").get<bool>(); ++i) {
    waited = gui.call("wait", {{"seconds", 50}});
  }
  ASSERT_TRUE(waited.at("done"));
  EXPECT_EQ(waited.at("result").at("status"), "done");
  EXPECT_EQ(waited.at("result").at("steps").size(), 3U);  // with the mesh object
  // It stops at the first failure and gives the steps done until then.
  const Json failed = gui.call(
      "pipeline", {{"steps",
                    {{{"operation", "segment_materials"}, {"params", {{"materials", 1}}}},
                     {{"operation", "material_meshes"}, {"params", {{"material_ids", {9}}}}},
                     {{"operation", "segment_materials"}}}}});
  EXPECT_EQ(failed.at("status"), "failed");
  EXPECT_EQ(failed.at("failed_operation"), "material_meshes");
  EXPECT_EQ(failed.at("steps").size(), 1U);
  EXPECT_THROW(gui.call("pipeline", {{"steps", {{{"operation", "nope"}}}}}), std::invalid_argument);
  EXPECT_FALSE(gui.call("wait", {{"seconds", 0}}).contains("running"));

  // A GUI that goes away stops its background operation.
  {
    Gui leaving;
    leaving.registry().add(std::make_shared<Spin>());
    leaving.call("project_create", {{"path", (dir_ / "q").string()}});
    leaving.call("run_spin", {{"background", true}});
  }
}

TEST_F(GuiTest, McpCancelsOnlyCallsThatRunTheOperation) {
  Gui gui;
  gui.registry().add(std::make_shared<Spin>());
  McpServer server(gui, [](const Json&) {});
  const auto call = [&server](int id, const std::string& tool, const Json& arguments) {
    return server.handle(request(id, "tools/call", {{"name", tool}, {"arguments", arguments}}))
        .value_or(Json());
  };
  call(1, "project_create", {{"path", (dir_ / "p").string()}});
  const Json started = call(2, "run_spin", {{"background", true}});
  ASSERT_FALSE(started.at("result").at("isError")) << started.dump();
  EXPECT_TRUE(started.at("result").at("structuredContent").at("background"));
  const Json again = call(3, "run_spin", Json::object());
  EXPECT_TRUE(again.at("result").at("isError"));
  EXPECT_NE(again.at("result").at("content")[0].at("text").get<std::string>().find("still running"),
            std::string::npos);

  // A client that gives up on a wait call cancels the call, not the operation.
  auto waiting =
      std::async(std::launch::async, [&call] { return call(4, "wait", {{"seconds", 1}}); });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_FALSE(server.handle(
      {{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"}, {"params", {{"requestId", 4}}}}));
  EXPECT_FALSE(waiting.get().at("result").at("structuredContent").at("done"));
  EXPECT_EQ(gui.call("project_status", {}).at("running").at("operation"), "spin");

  const Json cancelled = call(5, "cancel", Json::object());
  EXPECT_EQ(cancelled.at("result").at("structuredContent").at("cancelled"), "spin");
  const Json waited = call(6, "wait", {{"seconds", 50}});
  EXPECT_TRUE(waited.at("result").at("structuredContent").at("done"));
  EXPECT_EQ(waited.at("result").at("structuredContent").at("result").at("status"), "failed");
}

TEST_F(GuiTest, McpCancellationOnlyConcernsRunningCalls) {
  Gui gui;
  McpServer server(gui, [](const Json&) {});
  EXPECT_FALSE(server.handle(
      {{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"}, {"params", {{"requestId", 7}}}}));
  EXPECT_FALSE(server.cancelled(7));
}

}  // namespace voxelsieve
