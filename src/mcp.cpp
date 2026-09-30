#include "voxelsieve/mcp.hpp"

#include <algorithm>
#include <array>
#include <istream>
#include <ostream>
#include <stdexcept>

namespace voxelsieve {
namespace {

using Json = nlohmann::json;

constexpr std::array<const char*, 3> kProtocolVersions = {"2025-06-18", "2025-03-26", "2024-11-05"};
constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;
constexpr int kInternalError = -32603;

constexpr const char* kInstructions =
    "VoxelSieve analyses industrial CT scans. Work happens in a project: create or open one, then "
    "run operations as steps. Typical order: run_import_raw (or run_open_dataset for an existing "
    "dataset), run_porosity, run_report with the inspection order. Inputs are taken from the "
    "latest step with the right output type unless given. project_status shows the step "
    "protocol; undo and redo move through it. Datasets can be hundreds of GB: never read them "
    "with read_file, use dataset_info and the summaries of the steps.";

Json errorResponse(const Json& id, int code, const std::string& message) {
  return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

Json resultResponse(const Json& id, Json result) {
  return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

Json toolResult(Json value, bool is_error) {
  // An "image" member ({mime_type, base64}) becomes image content next to the JSON text.
  Json image;
  if (value.is_object() && value.contains("image") && value.at("image").is_object()) {
    image = {{"type", "image"},
             {"data", value.at("image").at("base64")},
             {"mimeType", value.at("image").at("mime_type")}};
    value.erase("image");
  }
  Json result = {{"content", {{{"type", "text"}, {"text", value.dump(2)}}}}, {"isError", is_error}};
  if (!image.is_null()) {
    result["content"].insert(result["content"].begin(), image);
  }
  if (!is_error) {
    result["structuredContent"] = value;
  }
  return result;
}

}  // namespace

McpServer::McpServer(Studio& studio, std::function<void(const Json&)> send)
    : studio_(studio), send_(std::move(send)) {}

Json McpServer::callTool(const Json& params) {
  if (!params.contains("name") || !params.at("name").is_string()) {
    throw std::invalid_argument("tools/call needs a tool name");
  }
  const std::string name = params.at("name").get<std::string>();
  const Json arguments = params.value("arguments", Json::object());
  std::function<void(double)> progress;
  if (params.contains("_meta") && params.at("_meta").contains("progressToken")) {
    progress = [this, token = params.at("_meta").at("progressToken"),
                last = 0.0](double fraction) mutable {
      fraction = std::clamp(fraction, 0.0, 1.0);
      if (fraction <= last) {
        return;  // progress must increase
      }
      last = fraction;
      send_({{"jsonrpc", "2.0"},
             {"method", "notifications/progress"},
             {"params", {{"progressToken", token}, {"progress", fraction}, {"total", 1.0}}}});
    };
  }
  try {
    return toolResult(studio_.call(name, arguments, progress), false);
  } catch (const std::exception& error) {
    // Failures are tool results, so the model sees the message and can react.
    return toolResult(Json{{"error", error.what()}}, true);
  }
}

std::optional<Json> McpServer::handle(const Json& message) {
  if (!message.is_object() || !message.contains("method") || !message.at("method").is_string()) {
    if (message.is_object() && (message.contains("result") || message.contains("error"))) {
      return std::nullopt;  // a response to a request we never send
    }
    return std::make_optional(
        errorResponse(message.is_object() ? message.value("id", Json()) : Json(), kInvalidRequest,
                      "Invalid request"));
  }
  const std::string method = message.at("method").get<std::string>();
  if (!message.contains("id")) {
    return std::nullopt;  // notifications: initialized, cancelled, ...
  }
  const Json& id = message.at("id");
  const Json params = message.value("params", Json::object());
  try {
    if (method == "initialize") {
      std::string version = kProtocolVersions.front();
      const std::string requested = params.value("protocolVersion", "");
      if (std::find(kProtocolVersions.begin(), kProtocolVersions.end(), requested) !=
          kProtocolVersions.end()) {
        version = requested;
      }
      return std::make_optional(resultResponse(
          id, {{"protocolVersion", version},
               {"capabilities", {{"tools", Json::object()}}},
               {"serverInfo", {{"name", "voxelsieve"}, {"version", VOXELSIEVE_VERSION}}},
               {"instructions", kInstructions}}));
    }
    if (method == "ping") {
      return std::make_optional(resultResponse(id, Json::object()));
    }
    if (method == "tools/list") {
      Json tools = Json::array();
      for (const StudioMethod& tool : studio_.methods()) {
        tools.push_back({{"name", tool.name},
                         {"description", tool.description},
                         {"inputSchema", tool.parameters}});
      }
      return std::make_optional(resultResponse(id, {{"tools", tools}}));
    }
    if (method == "tools/call") {
      return std::make_optional(resultResponse(id, callTool(params)));
    }
    return std::make_optional(errorResponse(id, kMethodNotFound, "Method not found: " + method));
  } catch (const std::invalid_argument& error) {
    return std::make_optional(errorResponse(id, kInvalidParams, error.what()));
  } catch (const std::exception& error) {
    return std::make_optional(errorResponse(id, kInternalError, error.what()));
  }
}

void McpServer::serve(Studio& studio, std::istream& in, std::ostream& out) {
  const auto send = [&out](const Json& message) {
    out << message.dump(-1, ' ', false, Json::error_handler_t::replace) << '\n' << std::flush;
  };
  McpServer server(studio, send);
  std::string line;
  while (std::getline(in, line)) {
    if (line.find_first_not_of(" \t\r") == std::string::npos) {
      continue;
    }
    Json message;
    try {
      message = Json::parse(line);
    } catch (const Json::parse_error& error) {
      send(errorResponse(Json(), kParseError, error.what()));
      continue;
    }
    if (const auto response = server.handle(message)) {
      send(*response);
    }
  }
}

}  // namespace voxelsieve
