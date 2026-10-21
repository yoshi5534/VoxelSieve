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
    "run operations as steps; the project used last is open again when the server restarts "
    "(project_status shows which). Typical order: run_import_raw (or run_open_dataset for an "
    "existing dataset), run_porosity, run_report with the inspection order. A first article "
    "inspection against a drawing: run_surface, run_compare_cad, cad_features to find the "
    "nominal elements, run_measure_dimensions with the inspection plan, "
    "run_first_article_report. A battery cell: run_battery_inspection (run_battery_images for a "
    "cell given as section images), run_battery_report with the order. Inputs are taken from the "
    "latest step with the right output type unless given. project_status shows the step "
    "protocol; undo and redo move through it. Datasets can be hundreds of GB: never read them "
    "with read_file, use dataset_info and the summaries of the steps. Imports and analyses of "
    "large scans can take minutes to hours: give run_ tools background: true and call wait "
    "until done is true; cancel stops the operation. pipeline runs several operations in one call. "
    "The result files of the steps (report, "
    "porosity results, their pictures) are also resources.";

/// Methods hidden in the analysis profile, so a model chooses among fewer tools: those that only
/// keep the browser UI's view state, list_operations (tools/list already carries every
/// operation's schema) and object_add (run_open_dataset and run_add_mesh do the same).
constexpr std::array<std::string_view, 8> kHiddenAnalysisMethods = {
    "view_set",    "view_save",   "view_list",       "view_image",
    "view_rename", "view_delete", "list_operations", "object_add"};

/// Methods that change nothing: no project, no file, no state of the GUI.
constexpr std::array<std::string_view, 17> kReadOnlyMethods = {
    "project_status", "objects",     "results",    "step_telemetry", "list_operations",
    "dataset_info",   "histogram",   "list_files", "view_slice",     "view_objects",
    "view_list",      "view_image",  "browse",     "read_file",      "import_preview",
    "wait",           "cad_features"};

/// Methods that remove something for good. Everything else adds steps, which undo takes back.
constexpr std::array<std::string_view, 1> kDestructiveMethods = {"view_delete"};

/// Methods whose second call with the same arguments changes nothing more.
constexpr std::array<std::string_view, 4> kIdempotentMethods = {"project_open", "object_select",
                                                                "view_set", "cancel"};

/// Output types whose files are resources: results small enough to read, unlike datasets.
constexpr std::array<std::string_view, 8> kResourceOutputTypes = {"report",
                                                                  "porosity",
                                                                  "comparison",
                                                                  "pose",
                                                                  "measurements",
                                                                  "dimensional_measurement",
                                                                  "fibre_orientation",
                                                                  "battery"};

template <std::size_t N>
bool contains(const std::array<std::string_view, N>& names, std::string_view name) {
  return std::ranges::find(names, name) != names.end();
}

/// Words that a tool title writes in capitals.
constexpr std::array<std::string_view, 6> kTitleAcronyms = {"cad",  "dicom", "diconde",
                                                            "tiff", "xml",   "raw"};

/// Human-readable title of a tool, from its name: "run_compare_cad" becomes "Compare CAD".
std::string toolTitle(const std::string& name) {
  std::string_view rest = name;
  if (rest.starts_with("run_")) {
    rest.remove_prefix(4);
  }
  std::string title;
  while (!rest.empty()) {
    const std::size_t end = std::min(rest.find('_'), rest.size());
    std::string word(rest.substr(0, end));
    if (contains(kTitleAcronyms, word)) {
      std::ranges::transform(word, word.begin(),
                             [](char c) { return static_cast<char>(c - 'a' + 'A'); });
    } else if (title.empty() && word[0] >= 'a' && word[0] <= 'z') {
      word[0] = static_cast<char>(word[0] - 'a' + 'A');
    }
    title += title.empty() ? word : " " + word;
    rest.remove_prefix(std::min(end + 1, rest.size()));
  }
  return title;
}

Json toolAnnotations(const std::string& name) {
  const bool read_only = contains(kReadOnlyMethods, name);
  Json annotations = {
      {"title", toolTitle(name)}, {"readOnlyHint", read_only}, {"openWorldHint", false}};
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
       "run_import_dicom for DICOM slices, run_import_gzipped_xml for a gzipped XML project, "
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
       "2. Import the scan (run_import_raw, run_import_tiff, run_import_dicom, "
       "run_import_gzipped_xml or run_open_dataset).\n"
       "3. Extract the surface with run_surface, then run run_compare_cad with cad_path {cad}; "
       "it aligns the scan to the model.\n"
       "4. Check the alignment with view_objects and report the deviations: share within "
       "tolerance, largest deviations and where they are.\n"
       "Quote the numbers of the comparison step; do not estimate them."},
      {"first_article_inspection",
       "First article inspection of a CT scan against its drawing: characteristics measured on "
       "the scan with datums and tolerances, and the first article report.",
       Json::array(
           {{{"name", "scan"},
             {"description", "Raw volume with JSON sidecar, TIFF stack or .vsieve dataset"},
             {"required", true}},
            {{"name", "cad"},
             {"description", "CAD model as STL in mm, in the coordinates of the drawing"},
             {"required", true}},
            {{"name", "plan"},
             {"description",
              "Inspection plan (JSON): elements, datums and characteristics with tolerances; "
              "or the characteristics of the drawing in words"},
             {"required", false}},
            {{"name", "order"},
             {"description",
              "Order data (JSON): supplier, customer, part, drawing revision, reason"},
             {"required", false}},
            {{"name", "project"},
             {"description", "Project directory; default: next to the scan"},
             {"required", false}}}),
       "Make the first article inspection report of the CT scan {scan} against the CAD model "
       "{cad} with VoxelSieve.\n"
       "1. Open or create the project {project} (next to the scan if none is given) and import "
       "the scan; extract the surface with run_surface.\n"
       "2. Align the CAD model to the scan with run_compare_cad (cad_path {cad}) and check the "
       "alignment with view_objects.\n"
       "3. Inspection plan: {plan}. If there is none or it only lists the characteristics in "
       "words, call cad_features on {cad} and build the plan (see the description of "
       "run_measure_dimensions): one element per feature with its 'at' point, the datums of the "
       "drawing in "
       "order (alignment.datums), and every characteristic with its balloon number, nominal "
       "value and tolerance. Never invent tolerances: leave characteristics without a "
       "tolerance unrated and say so.\n"
       "4. Run run_measure_dimensions with the plan. Elements that could not be measured have a "
       "message; "
       "fix their 'at' point or search_mm and measure again.\n"
       "5. Run run_first_article_report with the order {order}; list the fields reported as "
       "missing so they can be filled in.\n"
       "6. Summarise the decision and every characteristic out of tolerance with nominal, "
       "actual value and deviation, quoting the measurement, not estimates. The report from "
       "VoxelSieve is the result; the final approval belongs to the customer."},
      {"battery_cell_inspection",
       "Inspection of a battery cell: housing, electrode layers, anode overhang, foreign "
       "particles and gas voids, and the test report.",
       Json::array(
           {{{"name", "scan"},
             {"description", "Raw volume with JSON sidecar, TIFF stack or .vsieve dataset"},
             {"required", true}},
            {{"name", "order"},
             {"description",
              "Order (JSON) with the requirements in battery: minimum overhang, layer count, "
              "dimensions, particles, voids"},
             {"required", false}},
            {{"name", "project"},
             {"description", "Project directory; default: next to the scan"},
             {"required", false}}}),
       "Inspect the battery cell in the CT scan {scan} with VoxelSieve.\n"
       "1. Open or create the project {project} (next to the scan if none is given) and import "
       "the scan.\n"
       "2. Look at the middle slices with view_slice: cylindrical or stacked (pouch, prismatic), "
       "is the cell axis along a volume axis, are anode and cathode layers told apart?\n"
       "3. Run run_battery_inspection; give cell_type or axis only when the detection is wrong, "
       "and cathode_brighter false when the anode layers are the brighter ones.\n"
       "4. Run run_battery_report with the order {order}; list the fields reported as missing.\n"
       "5. Summarise the smallest anode overhang per edge with its layer and position, the "
       "layer count, gaps, deformation of the housing, particles and voids, and every "
       "requirement that is not met, quoting the results of the steps, not estimates."},
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

McpServer::McpServer(Gui& gui, std::function<void(const Json&)> send, McpTools tools)
    : gui_(gui), send_(std::move(send)), tools_(tools) {}

bool McpServer::offered(const std::string& tool) const {
  return tools_ == McpTools::kAll || !contains(kHiddenAnalysisMethods, tool);
}

Json McpServer::listResources() const {
  Json resources = Json::array();
  Json status;
  try {
    status = gui_.call("project_status", Json::object());
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
      const Json files = gui_.call("list_files", {{"step", id}, {"output", output}});
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
    read = gui_.call("read_file", {{"step", step},
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
        Json{{"error", "The tool '" + name + "' is not offered; start vs-gui with --tools all"}},
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
  // Cancelling this call stops the operation only when the call runs it: not for one started in
  // the background, and not for other tools such as wait.
  const bool background = arguments.is_object() && arguments.contains("background") &&
                          arguments.at("background") == true;
  const bool runs_operation = (name.starts_with("run_") || name == "object_add") && !background;
  {
    const std::scoped_lock lock(mutex_);
    running_[id.dump()] = runs_operation;
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
    return toolResult(gui_.call(name, arguments, progress), false);
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
      if (const auto call = running_.find(id); call != running_.end()) {
        cancelled_.insert(id);
        if (call->second) {
          gui_.cancel();  // one operation runs at a time: the one of this call
        }
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
      for (const GuiMethod& tool : gui_.methods()) {
        if (offered(tool.name)) {
          tools.push_back({{"name", tool.name},
                           {"title", toolTitle(tool.name)},
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

void McpServer::serve(Gui& gui, std::istream& in, std::ostream& out, McpTools tools) {
  std::mutex out_mutex;
  const auto send = [&out, &out_mutex](const Json& message) {
    const std::string text = message.dump(-1, ' ', false, Json::error_handler_t::replace);
    const std::scoped_lock lock(out_mutex);
    out << text << '\n' << std::flush;
  };
  McpServer server(gui, send, tools);
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
    gui.cancel();
  }
  calls.clear();  // waits for each call
}

}  // namespace voxelsieve
