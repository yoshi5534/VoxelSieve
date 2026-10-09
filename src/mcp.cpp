#include "voxelsieve/mcp.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <istream>
#include <list>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace voxelsieve {
namespace {

using Json = nlohmann::json;

constexpr std::array<const char*, 3> kProtocolVersions = {"2025-06-18", "2025-03-26", "2024-11-05"};
constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;
constexpr int kInternalError = -32603;
constexpr int kResourceNotFound = -32002;

constexpr std::string_view kResourceScheme = "voxelsieve://step/";
constexpr std::uintmax_t kMaxTextResourceBytes = 1U << 20U;
constexpr std::uintmax_t kMaxImageResourceBytes = 8U << 20U;

constexpr const char* kInstructions =
    "VoxelSieve analyses industrial CT scans. Work happens in a project: create or open one, then "
    "run operations as steps. Typical order: run_import_raw (or run_open_dataset for an existing "
    "dataset), run_porosity, run_report with the inspection order. Inputs are taken from the "
    "latest step with the right output type unless given. project_status shows the step "
    "protocol; undo and redo move through it. Datasets can be hundreds of GB: never read them "
    "with read_file, use dataset_info and the summaries of the steps. The result files of the "
    "steps (report, porosity results, their pictures) are also resources.";

/// Methods that only keep the browser UI's view state; hidden in the analysis profile.
constexpr std::array<std::string_view, 6> kViewStateMethods = {
    "view_set", "view_save", "view_list", "view_image", "view_rename", "view_delete"};

/// Methods that change nothing: no project, no file, no state of the studio.
constexpr std::array<std::string_view, 12> kReadOnlyMethods = {
    "project_status", "objects",    "step_telemetry", "list_operations",
    "dataset_info",   "list_files", "view_slice",     "view_objects",
    "view_list",      "view_image", "browse",         "read_file"};

/// Methods that remove something for good. Everything else adds steps, which undo takes back.
constexpr std::array<std::string_view, 1> kDestructiveMethods = {"view_delete"};

/// Methods whose second call with the same arguments changes nothing more.
constexpr std::array<std::string_view, 3> kIdempotentMethods = {"project_open", "object_select",
                                                                "view_set"};

/// Output types whose files are resources: results small enough to read, unlike datasets.
constexpr std::array<std::string_view, 4> kResourceOutputTypes = {"report", "porosity",
                                                                  "comparison", "pose"};

template <std::size_t N>
bool contains(const std::array<std::string_view, N>& names, std::string_view name) {
  return std::ranges::find(names, name) != names.end();
}

Json toolAnnotations(const std::string& name) {
  const bool read_only = contains(kReadOnlyMethods, name);
  Json annotations = {{"readOnlyHint", read_only}, {"openWorldHint", false}};
  if (!read_only) {
    annotations["destructiveHint"] = contains(kDestructiveMethods, name);
    annotations["idempotentHint"] = contains(kIdempotentMethods, name);
  }
  return annotations;
}

/// MIME type of a result file that is offered as a resource, or "" for other files.
std::string resourceMimeType(const std::filesystem::path& file) {
  const std::string extension = file.extension().string();
  if (extension == ".json") {
    return "application/json";
  }
  if (extension == ".html") {
    return "text/html";
  }
  if (extension == ".csv") {
    return "text/csv";
  }
  if (extension == ".txt") {
    return "text/plain";
  }
  if (extension == ".md") {
    return "text/markdown";
  }
  if (extension == ".png") {
    return "image/png";
  }
  return "";
}

struct Prompt {
  std::string name;
  std::string description;
  Json arguments;    // [{name, description, required}]
  std::string text;  // {argument} is replaced by its value
};

const std::vector<Prompt>& prompts() {
  static const std::vector<Prompt> all = {
      {"porosity_check",
       "Porosity inspection of a CT scan against the acceptance limits of an inspection order "
       "(BDG P 202), with a report.",
       Json::array({{{"name", "scan"},
                     {"description", "Raw volume with JSON sidecar, TIFF stack or .vsieve dataset"},
                     {"required", true}},
                    {{"name", "order"},
                     {"description", "Inspection order (JSON) with the acceptance limits"},
                     {"required", false}},
                    {{"name", "project"},
                     {"description", "Project directory; default: next to the scan"},
                     {"required", false}}}),
       "Check the porosity of the CT scan {scan} with VoxelSieve.\n"
       "1. Open the project {project} with project_open, or create it with project_create when "
       "it does not exist (choose a directory next to the scan if none is given).\n"
       "2. Import the scan: run_import_raw for a raw volume, run_import_tiff for a TIFF stack, "
       "run_import_dicom for DICOM slices, run_import_vgl for a VGStudio project (.vgl), "
       "run_open_dataset for a .vsieve dataset. Large scans take a while; report progress.\n"
       "3. Run run_porosity and summarise the result: pores, largest pore, porosity in percent, "
       "loosened zones.\n"
       "4. Look at the worst region with view_slice through the largest pore.\n"
       "5. Run run_report with the inspection order {order} (order_path) and state for each "
       "acceptance limit whether it is met, quoting the values from the report, not estimates.\n"
       "Say clearly that the report from VoxelSieve is the result, not this summary."},
      {"compare_with_cad",
       "Nominal-actual comparison of a CT scan with its CAD model: surface, alignment and "
       "deviations.",
       Json::array(
           {{{"name", "scan"},
             {"description", "Raw volume with JSON sidecar, TIFF stack or .vsieve dataset"},
             {"required", true}},
            {{"name", "cad"}, {"description", "CAD model as STL in mm"}, {"required", true}},
            {{"name", "project"},
             {"description", "Project directory; default: next to the scan"},
             {"required", false}}}),
       "Compare the CT scan {scan} with the CAD model {cad} using VoxelSieve.\n"
       "1. Open or create the project {project} (next to the scan if none is given).\n"
       "2. Import the scan (run_import_raw, run_import_tiff, run_import_dicom, run_import_vgl or "
       "run_open_dataset).\n"
       "3. Extract the surface with run_surface, then run run_compare_cad with cad_path {cad}; "
       "it aligns the scan to the model.\n"
       "4. Check the alignment with view_objects and report the deviations: share within "
       "tolerance, largest deviations and where they are.\n"
       "Quote the numbers of the comparison step; do not estimate them."},
      {"first_look", "A first look at a CT scan: size, voxel size, grey values and three slices.",
       Json::array({{{"name", "scan"},
                     {"description", "Raw volume with JSON sidecar, TIFF stack or .vsieve dataset"},
                     {"required", true}},
                    {{"name", "project"},
                     {"description", "Project directory; default: next to the scan"},
                     {"required", false}}}),
       "Take a first look at the CT scan {scan} with VoxelSieve.\n"
       "1. Open or create the project {project} (next to the scan if none is given) and import "
       "the scan.\n"
       "2. Report dimensions, voxel size and memory use from dataset_info.\n"
       "3. Show the middle slices along z, y and x with view_slice and describe the part, "
       "visible voids and artefacts such as rings or streaks.\n"
       "4. Suggest which analysis fits next (porosity, comparison with CAD, materials)."},
  };
  return all;
}

Json promptMessages(const Prompt& prompt, const Json& arguments) {
  std::string text = prompt.text;
  for (const Json& argument : prompt.arguments) {
    const std::string name = argument.at("name").get<std::string>();
    std::string value = "(not given)";
    if (arguments.contains(name) && arguments.at(name).is_string() &&
        !arguments.at(name).get<std::string>().empty()) {
      value = arguments.at(name).get<std::string>();
    } else if (argument.at("required").get<bool>()) {
      throw std::invalid_argument("Prompt '" + prompt.name + "' needs the argument '" + name + "'");
    }
    const std::string placeholder = "{" + name + "}";
    for (std::size_t at = text.find(placeholder); at != std::string::npos;
         at = text.find(placeholder, at + value.size())) {
      text.replace(at, placeholder.size(), value);
    }
  }
  return Json::array({{{"role", "user"}, {"content", {{"type", "text"}, {"text", text}}}}});
}

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

McpServer::McpServer(Studio& studio, std::function<void(const Json&)> send, McpTools tools)
    : studio_(studio), send_(std::move(send)), tools_(tools) {}

bool McpServer::offered(const std::string& tool) const {
  return tools_ == McpTools::kAll || !contains(kViewStateMethods, tool);
}

Json McpServer::listResources() const {
  Json resources = Json::array();
  Json status;
  try {
    status = studio_.call("project_status", Json::object());
  } catch (const std::invalid_argument&) {
    return resources;  // no project open
  }
  for (const Json& step : status.at("steps")) {
    if (!step.value("active", false) || step.value("status", "") != "done") {
      continue;
    }
    const int id = step.at("id").get<int>();
    for (const auto& [output, info] : step.at("outputs").items()) {
      if (!contains(kResourceOutputTypes, info.at("type").get<std::string>())) {
        continue;
      }
      const Json files = studio_.call("list_files", {{"step", id}, {"output", output}});
      for (const Json& entry : files.at("files")) {
        std::string file = entry.at("file").get<std::string>();
        std::replace(file.begin(), file.end(), '\\', '/');
        const std::string mime_type = resourceMimeType(file);
        const auto size = entry.at("size_bytes").get<std::uintmax_t>();
        if (mime_type.empty() ||
            size > (mime_type == "image/png" ? kMaxImageResourceBytes : kMaxTextResourceBytes)) {
          continue;
        }
        std::string uri(kResourceScheme);
        uri.append(std::to_string(id)).append("/").append(output).append("/").append(file);
        std::string title = step.value("title", step.at("operation").get<std::string>());
        title.append(" (step ").append(std::to_string(id)).append("): ").append(file);
        resources.push_back({{"uri", uri},
                             {"name", file},
                             {"title", title},
                             {"mimeType", mime_type},
                             {"size", size}});
      }
    }
  }
  return {{"resources", resources}};
}

Json McpServer::readResource(const Json& params) const {
  if (!params.contains("uri") || !params.at("uri").is_string()) {
    throw std::invalid_argument("resources/read needs a uri");
  }
  const std::string uri = params.at("uri").get<std::string>();
  // voxelsieve://step/<id>/<output>/<file>
  const auto not_found = [&uri] { return std::out_of_range("Resource not found: " + uri); };
  if (!uri.starts_with(kResourceScheme)) {
    throw not_found();
  }
  const std::string rest = uri.substr(kResourceScheme.size());
  const std::size_t id_end = rest.find('/');
  const std::size_t output_end =
      id_end == std::string::npos ? std::string::npos : rest.find('/', id_end + 1);
  if (output_end == std::string::npos || output_end + 1 >= rest.size()) {
    throw not_found();
  }
  const std::string step_text = rest.substr(0, id_end);
  if (step_text.empty() || step_text.size() > 9 ||
      !std::ranges::all_of(step_text, [](char c) { return c >= '0' && c <= '9'; })) {
    throw not_found();
  }
  const int step = std::stoi(step_text);
  const std::string file = rest.substr(output_end + 1);
  const std::string mime_type = resourceMimeType(file);
  if (mime_type.empty()) {
    throw not_found();
  }
  Json read;
  try {
    read = studio_.call("read_file", {{"step", step},
                                      {"output", rest.substr(id_end + 1, output_end - id_end - 1)},
                                      {"file", file}});
  } catch (const std::invalid_argument& error) {
    throw std::out_of_range(std::string(error.what()));
  }
  Json content = {{"uri", uri}, {"mimeType", mime_type}};
  if (read.contains("image")) {
    content["blob"] = read.at("image").at("base64");
  } else {
    content["text"] = read.at("text");
  }
  return {{"contents", Json::array({content})}};
}

bool McpServer::cancelled(const Json& id) const {
  const std::scoped_lock lock(mutex_);
  return cancelled_.contains(id.dump());
}

Json McpServer::callTool(const Json& id, const Json& params) {
  if (!params.contains("name") || !params.at("name").is_string()) {
    throw std::invalid_argument("tools/call needs a tool name");
  }
  const std::string name = params.at("name").get<std::string>();
  if (!offered(name)) {
    return toolResult(
        Json{{"error", "The tool '" + name + "' is not offered; start vs-studio with --tools all"}},
        true);
  }
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
  {
    const std::scoped_lock lock(mutex_);
    running_.insert(id.dump());
  }
  struct Done {
    McpServer& server;
    std::string id;
    ~Done() {
      const std::scoped_lock lock(server.mutex_);
      server.running_.erase(id);
    }
  } const done{*this, id.dump()};
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
    if (method == "notifications/cancelled" && message.contains("params") &&
        message.at("params").contains("requestId")) {
      const std::string id = message.at("params").at("requestId").dump();
      const std::scoped_lock lock(mutex_);
      if (running_.contains(id)) {
        cancelled_.insert(id);
        studio_.cancel();  // one operation runs at a time: the one of this call
      }
    }
    return std::nullopt;  // other notifications: initialized, ...
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
               {"capabilities",
                {{"tools", Json::object()},
                 {"resources", Json::object()},
                 {"prompts", Json::object()}}},
               {"serverInfo", {{"name", "voxelsieve"}, {"version", VOXELSIEVE_VERSION}}},
               {"instructions", kInstructions}}));
    }
    if (method == "ping") {
      return std::make_optional(resultResponse(id, Json::object()));
    }
    if (method == "tools/list") {
      Json tools = Json::array();
      for (const StudioMethod& tool : studio_.methods()) {
        if (offered(tool.name)) {
          tools.push_back({{"name", tool.name},
                           {"description", tool.description},
                           {"inputSchema", tool.parameters},
                           {"annotations", toolAnnotations(tool.name)}});
        }
      }
      return std::make_optional(resultResponse(id, {{"tools", tools}}));
    }
    if (method == "tools/call") {
      return std::make_optional(resultResponse(id, callTool(id, params)));
    }
    if (method == "resources/list") {
      return std::make_optional(resultResponse(id, listResources()));
    }
    if (method == "resources/templates/list") {
      return std::make_optional(resultResponse(id, {{"resourceTemplates", Json::array()}}));
    }
    if (method == "resources/read") {
      try {
        return std::make_optional(resultResponse(id, readResource(params)));
      } catch (const std::out_of_range& error) {
        Json response = errorResponse(id, kResourceNotFound, error.what());
        response["error"]["data"] = {{"uri", params.value("uri", "")}};
        return std::make_optional(response);
      }
    }
    if (method == "prompts/list") {
      Json list = Json::array();
      for (const Prompt& prompt : prompts()) {
        list.push_back({{"name", prompt.name},
                        {"description", prompt.description},
                        {"arguments", prompt.arguments}});
      }
      return std::make_optional(resultResponse(id, {{"prompts", list}}));
    }
    if (method == "prompts/get") {
      const std::string name = params.value("name", "");
      const auto prompt = std::ranges::find(prompts(), name, &Prompt::name);
      if (prompt == prompts().end()) {
        throw std::invalid_argument("Unknown prompt '" + name + "'");
      }
      return std::make_optional(resultResponse(
          id, {{"description", prompt->description},
               {"messages", promptMessages(*prompt, params.value("arguments", Json::object()))}}));
    }
    return std::make_optional(errorResponse(id, kMethodNotFound, "Method not found: " + method));
  } catch (const std::invalid_argument& error) {
    return std::make_optional(errorResponse(id, kInvalidParams, error.what()));
  } catch (const std::exception& error) {
    return std::make_optional(errorResponse(id, kInternalError, error.what()));
  }
}

void McpServer::serve(Studio& studio, std::istream& in, std::ostream& out, McpTools tools) {
  std::mutex out_mutex;
  const auto send = [&out, &out_mutex](const Json& message) {
    const std::string text = message.dump(-1, ' ', false, Json::error_handler_t::replace);
    const std::scoped_lock lock(out_mutex);
    out << text << '\n' << std::flush;
  };
  McpServer server(studio, send, tools);
  std::list<std::future<void>> calls;
  std::string line;
  while (std::getline(in, line)) {
    std::erase_if(calls, [](const std::future<void>& call) {
      return call.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    });
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
    if (message.is_object() && message.contains("id") &&
        message.value("method", "") == "tools/call") {
      calls.push_back(std::async(std::launch::async, [&server, &send, message] {
        if (const auto response = server.handle(message);
            response && !server.cancelled(message.at("id"))) {
          send(*response);
        }
      }));
      continue;
    }
    if (const auto response = server.handle(message)) {
      send(*response);
    }
  }
  // The client is gone: stop what still runs.
  if (!calls.empty()) {
    studio.cancel();
  }
  calls.clear();  // waits for each call
}

}  // namespace voxelsieve
