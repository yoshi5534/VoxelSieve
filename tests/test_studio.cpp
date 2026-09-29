#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "voxelsieve/io.hpp"
#include "voxelsieve/mcp.hpp"
#include "voxelsieve/mesh.hpp"
#include "voxelsieve/model.hpp"
#include "voxelsieve/studio.hpp"
#include "voxelsieve/synthetic.hpp"

namespace voxelsieve {
namespace {

using Json = nlohmann::json;

class StudioTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("voxelsieve_studio_" +
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

TEST_F(StudioTest, ApiRunsTheWorkflowAndReportsTheProtocol) {
  Studio studio({VOXELSIEVE_TEST_PLUGIN_DIR});
  EXPECT_FALSE(studio.call("project_status", {}).at("open"));
  EXPECT_THROW(studio.call("run_porosity", {}), std::invalid_argument);  // no project yet

  const Json created = studio.call("project_create", {{"path", (dir_ / "p").string()}});
  EXPECT_EQ(created.at("name"), "p");
  EXPECT_FALSE(created.at("can_undo"));

  const Json imported =
      studio.call("run_import_raw", {{"path", (dir_ / "scan.raw").string()}, {"brick_size", 32}});
  EXPECT_EQ(imported.at("status"), "done");
  EXPECT_GT(imported.at("size_bytes").get<std::uint64_t>(), 0U);

  const Json info = studio.call("dataset_info", {});
  EXPECT_EQ(info.at("step"), imported.at("id"));
  EXPECT_FALSE(info.at("levels").empty());

  std::vector<double> progress;
  const Json porosity =
      studio.call("run_porosity", {}, [&progress](double value) { progress.push_back(value); });
  EXPECT_EQ(porosity.at("summary").at("pores"), 2);
  EXPECT_FALSE(progress.empty());

  // A slice as PNG with the overlay of the porosity step, at a level that fits max_pixels.
  const Json slice = studio.call("view_slice", {{"axis", "x"}, {"max_pixels", 64}});
  EXPECT_EQ(slice.at("porosity_step"), porosity.at("id"));
  EXPECT_EQ(slice.at("right"), "y");
  EXPECT_EQ(slice.at("down"), "z");
  EXPECT_LE(std::max(slice.at("width").get<int>(), slice.at("height").get<int>()), 64);
  EXPECT_GT(slice.at("level").get<int>(), 0);
  EXPECT_TRUE(slice.at("image").at("base64").get<std::string>().starts_with("iVBORw0KGgo"));
  EXPECT_TRUE(
      studio.call("view_slice", {{"overlay", false}, {"index", 3}}).at("porosity_step").is_null());
  EXPECT_THROW(studio.call("view_slice", {{"index", 100000}}), std::invalid_argument);

  const Json files = studio.call("list_files", {});
  bool has_json = false;
  for (const Json& file : files.at("files")) {
    has_json = has_json || file.at("file") == "porosity.json";
  }
  EXPECT_TRUE(has_json);
  const Json text = studio.call("read_file", {{"file", "porosity.json"}});
  EXPECT_EQ(Json::parse(text.at("text").get<std::string>()).at("pores").size(), 2U);
  EXPECT_THROW(studio.call("read_file", {{"file", "../../../project.json"}}),
               std::invalid_argument);
  EXPECT_THROW(studio.call("read_file", {{"file", "projection_z.png"}}), std::invalid_argument);

  // The surface of the box: 6 x 5 x 4 mm, lunkers inside, as mask, images and mesh.
  const Json surface = studio.call("run_surface", {{"stl", true}});
  EXPECT_EQ(surface.at("status"), "done");
  EXPECT_NEAR(surface.at("summary").at("surface_volume_mm3").get<double>(), 120.0, 1.5);
  EXPECT_GT(surface.at("summary").at("compression_vs_raw").get<double>(), 10.0);
  const Json surface_files = studio.call("list_files", {});
  std::vector<std::string> names;
  for (const Json& file : surface_files.at("files")) {
    names.push_back(file.at("file").get<std::string>());
  }
  for (const char* name : {"surface.vss", "surface.json", "surface.stl", "surface_z.png"}) {
    EXPECT_NE(std::find(names.begin(), names.end(), name), names.end()) << name;
  }

  // Nominal-actual comparison with the box as CAD model, placed elsewhere.
  writeStl(dir_ / "cad.stl", boxMesh({6.0, 5.0, 4.0}, {10.0, -3.0, 2.0}));
  const Json compared = studio.call("run_compare_cad", {{"cad_path", (dir_ / "cad.stl").string()}});
  EXPECT_EQ(compared.at("status"), "done");
  EXPECT_GT(compared.at("summary").at("within_tolerance_percent").get<double>(), 90.0);
  EXPECT_LT(std::abs(compared.at("summary").at("deviation_mean_mm").get<double>()), 0.02);
  const auto deviation = studio.deviationMesh(std::nullopt);
  EXPECT_FALSE(deviation->mesh.triangles.empty());
  EXPECT_EQ(deviation->deviation_mm.size(), deviation->mesh.points.size());
  EXPECT_DOUBLE_EQ(deviation->tolerance_mm, 0.1);
  EXPECT_EQ(studio.deviationMesh(std::nullopt), deviation);  // cached
  EXPECT_THROW((void)studio.call("run_compare_cad", {{"cad_path", (dir_ / "none.stl").string()}}),
               std::exception);

  // The report picks up the surface and the comparison for its 3D views.
  const Json reported = studio.call("run_report", {});
  EXPECT_EQ(reported.at("status"), "done");
  EXPECT_EQ(reported.at("inputs").at("surface").at("step"), surface.at("id"));
  EXPECT_EQ(reported.at("inputs").at("comparison").at("step"), compared.at("id"));
  std::ifstream report_file(dir_ / "p" / "steps" /
                            (std::to_string(reported.at("id").get<int>()) + "-report") / "report" /
                            "report.html");
  const std::string html{std::istreambuf_iterator<char>(report_file),
                         std::istreambuf_iterator<char>()};
  EXPECT_NE(html.find("Soll-Ist-Vergleich"), std::string::npos);
  EXPECT_NE(html.find("Poren (rot)"), std::string::npos);

  // Explicit inputs by step id.
  const Json histogram = studio.call(
      "run_histogram", {{"bins", 8}, {"inputs", {{"dataset", {{"step", imported.at("id")}}}}}});
  EXPECT_EQ(histogram.at("inputs").at("dataset").at("step"), imported.at("id"));

  const Json undone = studio.call("undo", {});
  EXPECT_TRUE(undone.at("changed"));
  EXPECT_FALSE(undone.at("steps").back().at("active"));
  EXPECT_TRUE(studio.call("redo", {}).at("can_undo"));

  const Json reopened = [&] {
    Studio other;
    return other.call("project_open", {{"path", (dir_ / "p").string()}});
  }();
  EXPECT_EQ(reopened.at("steps").size(), 7U);

  EXPECT_THROW(studio.call("nope", {}), std::invalid_argument);
  EXPECT_THROW(studio.call("undo", Json::array()), std::invalid_argument);
  EXPECT_THROW(studio.call("project_open", {{"path", 3}}), std::invalid_argument);
}

TEST_F(StudioTest, SegmentsMaterialsAndShowsThemInSlices) {
  Studio studio({VOXELSIEVE_TEST_PLUGIN_DIR});
  (void)studio.call("project_create", {{"path", (dir_ / "p").string()}});
  const Json imported =
      studio.call("run_import_raw", {{"path", (dir_ / "scan.raw").string()}, {"brick_size", 32}});
  const Json segmented = studio.call("run_segment_materials", {{"materials", 1}});
  ASSERT_EQ(segmented.at("status"), "done");
  // One material: the box of 6 x 5 x 4 mm with two lunkers of 0.5 mm radius.
  const Json& material = segmented.at("summary").at("materials").at(0);
  EXPECT_NEAR(material.at("volume_mm3").get<double>(), 120.0 - 2 * 4.0 / 3.0 * M_PI * 0.125, 3.0);

  const Json slice = studio.call("view_slice", {});
  EXPECT_EQ(slice.at("materials_step"), segmented.at("id"));
  EXPECT_EQ(slice.at("materials").size(), 1U);

  const Json info = studio.call("dataset_info", {});
  SliceRequest request;
  request.index = info.at("dims").at(2).get<std::int64_t>() / 2;
  request.size = {info.at("dims").at(0).get<std::int64_t>(),
                  info.at("dims").at(1).get<std::int64_t>()};
  const SliceImage tile = studio.sliceTile(imported.at("id").get<int>(), std::nullopt, request,
                                           segmented.at("id").get<int>());
  const auto material_pixels = std::count(tile.overlay.begin(), tile.overlay.end(),
                                          static_cast<std::uint8_t>(SliceOverlay::kMaterial) + 1);
  EXPECT_GT(material_pixels, 0);
  EXPECT_LT(material_pixels, static_cast<std::ptrdiff_t>(tile.overlay.size()));
}

TEST_F(StudioTest, SegmentsWithALearnedModel) {
  Studio studio;
  (void)studio.call("project_create", {{"path", (dir_ / "p").string()}});
  (void)studio.call("run_import_raw", {{"path", (dir_ / "scan.raw").string()}, {"brick_size", 32}});
  const auto threshold = studio.call("dataset_info", {}).at("threshold").get<float>();
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
      studio.call("run_segment_model", {{"model_path", (dir_ / "schwelle.vsm").string()}});
  ASSERT_EQ(segmented.at("status"), "done");
  EXPECT_EQ(segmented.at("summary").at("model"), "Schwelle");
  const Json& result = segmented.at("summary").at("materials").at(0);
  EXPECT_EQ(result.at("name"), "Guss");
  EXPECT_NEAR(result.at("volume_mm3").get<double>(), 120.0 - 2 * 4.0 / 3.0 * M_PI * 0.125, 3.0);
  const Json slice = studio.call("view_slice", {});
  EXPECT_EQ(slice.at("materials_step"), segmented.at("id"));
  EXPECT_THROW((void)studio.call("run_segment_model", {}), std::invalid_argument);
}

TEST_F(StudioTest, ViewsAreRestoredWhenTheProjectIsOpenedAgain) {
  const auto project = (dir_ / "p").string();
  const Json state = {{"stage", "view"}, {"mode", "3d"}, {"transfer", {{"points", Json::array()}}}};
  {
    Studio studio;
    (void)studio.call("project_create", {{"path", project}});
    (void)studio.call("view_set", {{"state", state}});
    // "iVBORw0KGgo=" is the PNG signature.
    const Json saved =
        studio.call("view_save", {{"name", "Aufsicht"}, {"image_base64", "iVBORw0KGgo="}});
    EXPECT_EQ(saved.at("state"), state);  // the current view by default
    EXPECT_THROW((void)studio.call("view_save", {{"name", "x"}, {"image_base64", "#?"}}),
                 std::invalid_argument);
  }
  Studio studio;
  const Json status = studio.call("project_open", {{"path", project}});
  EXPECT_EQ(status.at("view"), state);
  const Json views = studio.call("view_list", {}).at("views");
  ASSERT_EQ(views.size(), 1U);
  const Json image = studio.call("view_image", {{"id", views[0].at("id")}});
  EXPECT_EQ(image.at("image").at("mime_type"), "image/png");
  EXPECT_EQ(image.at("image").at("base64"), "iVBORw0KGgo=");
  const Json renamed = studio.call("view_rename", {{"id", views[0].at("id")}, {"name", "Seite"}});
  EXPECT_EQ(renamed.at("name"), "Seite");
}

TEST_F(StudioTest, EveryOperationIsAMethodWithItsSchema) {
  const Studio studio({VOXELSIEVE_TEST_PLUGIN_DIR});
  bool found = false;
  for (const StudioMethod& method : studio.methods()) {
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

TEST_F(StudioTest, McpServesToolsOverJsonRpc) {
  Studio studio;
  std::vector<Json> sent;
  McpServer server(studio, [&sent](const Json& message) { sent.push_back(message); });
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

  const auto list = handle(request(3, "tools/list"));
  EXPECT_EQ(list.at("result").at("tools").size(), studio.methods().size());
  for (const Json& tool : list.at("result").at("tools")) {
    EXPECT_TRUE(tool.contains("inputSchema"));
  }

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
  EXPECT_EQ(handle(request(9, "resources/list")).at("error").at("code"), -32601);
  EXPECT_EQ(handle(Json::array()).at("error").at("code"), -32600);
  EXPECT_EQ(handle(request(10, "ping")).at("result"), Json::object());
}

TEST_F(StudioTest, McpServeReadsLines) {
  Studio studio;
  std::istringstream in(request(1, "ping").dump() + "\n\nnot json\n" +
                        Json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}.dump() +
                        "\n" + request(2, "tools/list").dump() + "\n");
  std::ostringstream out;
  McpServer::serve(studio, in, out);
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
}  // namespace voxelsieve
