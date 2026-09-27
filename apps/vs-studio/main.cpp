// vs-studio: the VoxelSieve studio (ADR 0008). With --mcp it serves the studio API to AI systems
// over the Model Context Protocol on stdin/stdout.

#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "voxelsieve/mcp.hpp"
#include "voxelsieve/studio.hpp"

namespace {

constexpr std::string_view kUsage = R"(Usage: vs-studio --mcp [options]

Serves the VoxelSieve studio over the Model Context Protocol (JSON-RPC on stdin/stdout). Register
it with an MCP client as a stdio server, for example:
  {"command": "vs-studio", "args": ["--mcp", "--project", "/data/casting.vsproj"]}
Every studio function is a tool: projects, undo/redo, operations (run_<id>), dataset info and
reading result files.

Options:
  --mcp                 Serve MCP on stdin/stdout (the browser UI follows in a later version)
  --project <dir>       Open this project at start; created if the directory has no project
  --plugins <dir>       Load operation plugins (*.so) from <dir>; repeatable. Directories in
                        VOXELSIEVE_PLUGIN_PATH (colon-separated) are loaded too
  -h, --help            Show this help
)";

struct Options {
  bool mcp = false;
  std::optional<std::filesystem::path> project;
  std::vector<std::filesystem::path> plugins;
};

std::optional<Options> parse(int argc, char** argv) {
  Options options;
  const std::vector<std::string_view> args(argv + 1, argv + argc);
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string_view arg = args[i];
    const auto next = [&]() -> std::string {
      if (i + 1 >= args.size()) {
        throw std::invalid_argument("Missing value for " + std::string(arg));
      }
      return std::string(args[++i]);
    };
    if (arg == "-h" || arg == "--help") {
      return std::nullopt;
    }
    if (arg == "--mcp") {
      options.mcp = true;
    } else if (arg == "--project") {
      options.project = next();
    } else if (arg == "--plugins") {
      options.plugins.emplace_back(next());
    } else {
      throw std::invalid_argument("Unknown argument " + std::string(arg));
    }
  }
  return options;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto options = parse(argc, argv);
    if (!options) {
      std::cout << kUsage;
      return 0;
    }
    if (!options->mcp) {
      std::cerr << "vs-studio: only --mcp is available in this version\n\n" << kUsage;
      return 2;
    }
    auto plugin_dirs = voxelsieve::pluginPathFromEnvironment();
    plugin_dirs.insert(plugin_dirs.end(), options->plugins.begin(), options->plugins.end());
    voxelsieve::Studio studio(plugin_dirs);
    // stdout carries the protocol; everything else goes to stderr.
    for (const std::string& message : studio.pluginMessages()) {
      std::cerr << "vs-studio: " << message << "\n";
    }
    if (options->project) {
      const bool exists = std::filesystem::exists(*options->project / "project.json");
      (void)studio.call(exists ? "project_open" : "project_create",
                        {{"path", options->project->string()}});
    }
    voxelsieve::McpServer::serve(studio, std::cin, std::cout);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "vs-studio: " << error.what() << "\n";
    return 1;
  }
}
