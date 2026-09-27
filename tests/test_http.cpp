#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <filesystem>
#include <future>
#include <string>
#include <thread>

#include "voxelsieve/http_server.hpp"
#include "voxelsieve/io.hpp"
#include "voxelsieve/mesh.hpp"
#include "voxelsieve/studio.hpp"
#include "voxelsieve/synthetic.hpp"

namespace voxelsieve {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using Json = nlohmann::json;

struct Reply {
  unsigned status = 0;
  std::string content_type;
  std::string body;
  [[nodiscard]] Json json() const { return Json::parse(body); }
};

Reply request(unsigned short port, http::verb verb, const std::string& target,
              const std::string& body = "", const std::string& content_type = "application/json",
              const std::string& host = "localhost") {
  asio::io_context io;
  asio::ip::tcp::socket socket(io);
  socket.connect({asio::ip::make_address("127.0.0.1"), port});
  http::request<http::string_body> message(verb, target, 11);
  message.set(http::field::host, host);
  if (!content_type.empty()) {
    message.set(http::field::content_type, content_type);
  }
  message.body() = body;
  message.prepare_payload();
  http::write(socket, message);
  beast::flat_buffer buffer;
  http::response<http::string_body> response;
  http::read(socket, buffer, response);
  return {response.result_int(), std::string(response[http::field::content_type]), response.body()};
}

/// Blocks until released, to observe the studio while an operation runs.
class Wait final : public Operation {
 public:
  explicit Wait(std::shared_future<void> release) : release_(std::move(release)) {
    info_.id = "wait";
    info_.title = "Wait";
    info_.outputs = {{"table", "table", ""}};
  }
  [[nodiscard]] const OperationInfo& info() const override { return info_; }
  [[nodiscard]] OperationResult run(const OperationContext& context) const override {
    context.progress(0.5);
    release_.wait();
    writeJson(context.output_dir / "t.json", Json::object());
    return {{{"table", "t.json"}}, {}};
  }

 private:
  OperationInfo info_;
  std::shared_future<void> release_;
};

class HttpTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("voxelsieve_http_" +
            std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
    studio_.registry().add(std::make_shared<Wait>(release_.get_future().share()));
    server_ = std::make_unique<HttpServer>(studio_, "127.0.0.1", 0);
    thread_ = std::thread([this] { server_->run(); });
  }
  void TearDown() override {
    server_->stop();
    thread_.join();
    std::filesystem::remove_all(dir_);
  }

  [[nodiscard]] Reply call(const std::string& method, const Json& params = Json::object()) const {
    return request(server_->port(), http::verb::post, "/api/call/" + method, params.dump());
  }

  std::filesystem::path dir_;
  std::promise<void> release_;
  Studio studio_;
  std::unique_ptr<HttpServer> server_;
  std::thread thread_;
};

TEST_F(HttpTest, ServesTheUi) {
  const Reply index = request(server_->port(), http::verb::get, "/");
  EXPECT_EQ(index.status, 200U);
  EXPECT_NE(index.body.find("VoxelSieve"), std::string::npos);
  EXPECT_NE(index.content_type.find("text/html"), std::string::npos);
  const Reply script = request(server_->port(), http::verb::get, "/app.js");
  EXPECT_EQ(script.status, 200U);
  EXPECT_NE(script.content_type.find("javascript"), std::string::npos);
  EXPECT_EQ(script.body, uiResource("app.js"));
  EXPECT_EQ(request(server_->port(), http::verb::get, "/app.css").status, 200U);
  EXPECT_EQ(request(server_->port(), http::verb::get, "/nothing").status, 404U);
  const Reply methods = request(server_->port(), http::verb::get, "/api/methods");
  bool has_porosity = false;
  for (const Json& method : methods.json()) {
    has_porosity = has_porosity || method.at("name") == "run_porosity";
  }
  EXPECT_TRUE(has_porosity);
}

TEST_F(HttpTest, RefusesForeignHostsAndNonJsonCalls) {
  EXPECT_EQ(request(server_->port(), http::verb::get, "/", "", "", "evil.example").status, 403U);
  EXPECT_EQ(request(server_->port(), http::verb::get, "/", "", "", "localhost:8410").status, 200U);
  EXPECT_EQ(request(server_->port(), http::verb::get, "/", "", "", "127.0.0.1").status, 200U);
  EXPECT_EQ(
      request(server_->port(), http::verb::post, "/api/call/project_status", "{}", "text/plain")
          .status,
      415U);
  EXPECT_EQ(call("project_status").status, 200U);
  EXPECT_EQ(request(server_->port(), http::verb::post, "/api/call/project_status", "{").status,
            400U);
  const Reply unknown = call("nope");
  EXPECT_EQ(unknown.status, 400U);
  EXPECT_NE(unknown.json().at("error").get<std::string>().find("nope"), std::string::npos);
}

TEST_F(HttpTest, RunsStepsAndServesTheirFiles) {
  const SyntheticScan scan(boxMesh({3.0, 3.0, 3.0}), SyntheticSpec{});
  writeRaw(dir_ / "scan.raw", scan);
  writeJson(dir_ / "scan.json", scan.toJson());
  EXPECT_EQ(call("project_create", {{"path", (dir_ / "p").string()}}).status, 200U);
  const Reply imported =
      call("run_import_raw", {{"path", (dir_ / "scan.raw").string()}, {"brick_size", 16}});
  ASSERT_EQ(imported.status, 200U) << imported.body;
  const int step = imported.json().at("id").get<int>();
  const std::string base = "/files/" + std::to_string(step) + "/dataset/";
  const Reply index = request(server_->port(), http::verb::get, base + "index.json");
  EXPECT_EQ(index.status, 200U);
  EXPECT_EQ(index.content_type, "application/json");
  EXPECT_TRUE(Json::parse(index.body).contains("levels"));
  EXPECT_EQ(request(server_->port(), http::verb::get, base + "..%2F..%2F..%2Fproject.json").status,
            400U);
  EXPECT_EQ(request(server_->port(), http::verb::get, base + "missing.json").status, 400U);
  EXPECT_EQ(request(server_->port(), http::verb::get, "/files/99/dataset/index.json").status, 400U);

  // Slice tiles: float32 grey values, then one overlay byte per pixel.
  const Reply tile = request(server_->port(), http::verb::get,
                             "/api/tile?axis=2&index=10&level=0&u=0&v=0&size=16");
  ASSERT_EQ(tile.status, 200U) << tile.body;
  EXPECT_EQ(tile.body.size(), 16U * 16U * 5U);
  EXPECT_EQ(request(server_->port(), http::verb::get, "/api/tile?axis=5&size=16").status, 400U);
  EXPECT_EQ(request(server_->port(), http::verb::get, "/api/tile?index=x").status, 400U);

  const Reply browsed = call("browse", {{"path", dir_.string()}});
  ASSERT_EQ(browsed.status, 200U) << browsed.body;
  const Json listing = browsed.json();
  std::map<std::string, std::string> kinds;
  for (const Json& entry : listing.at("entries")) {
    kinds[entry.at("name").get<std::string>()] = entry.at("kind").get<std::string>();
  }
  EXPECT_EQ(kinds["p"], "project");
  EXPECT_EQ(kinds["scan.raw"], "raw");
  EXPECT_EQ(kinds["scan.json"], "file");
}

TEST_F(HttpTest, AnswersWhileAnOperationRuns) {
  ASSERT_EQ(call("project_create", {{"path", (dir_ / "p").string()}}).status, 200U);
  std::thread runner([this] { EXPECT_EQ(call("run_wait").status, 200U); });
  Json status;
  for (int i = 0; i < 500; ++i) {
    status = call("project_status").json();
    if (!status.at("running").is_null()) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_EQ(status.at("running").at("operation"), "wait");
  EXPECT_TRUE(status.at("steps").empty());
  const Reply blocked = call("undo");
  EXPECT_EQ(blocked.status, 400U);
  EXPECT_NE(blocked.json().at("error").get<std::string>().find("running"), std::string::npos);
  EXPECT_EQ(call("run_wait").status, 400U);
  release_.set_value();
  runner.join();
  status = call("project_status").json();
  EXPECT_TRUE(status.at("running").is_null());
  EXPECT_EQ(status.at("steps").size(), 1U);
}

TEST_F(HttpTest, StopClosesOpenConnections) {
  asio::io_context io;
  asio::ip::tcp::socket idle(io);
  idle.connect({asio::ip::make_address("127.0.0.1"), server_->port()});
  EXPECT_EQ(call("project_status").status, 200U);
  // TearDown stops the server while `idle` is still connected and must not hang.
}

}  // namespace
}  // namespace voxelsieve
