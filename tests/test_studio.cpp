#include <gtest/gtest.h>

#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "voxelsieve/io.hpp"
#include "voxelsieve/mcp.hpp"
#include "voxelsieve/mesh.hpp"
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
  EXPECT_EQ(reopened.at("steps").size(), 3U);

  EXPECT_THROW(studio.call("nope", {}), std::invalid_argument);
  EXPECT_THROW(studio.call("undo", Json::array()), std::invalid_argument);
  EXPECT_THROW(studio.call("project_open", {{"path", 3}}), std::invalid_argument);
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
