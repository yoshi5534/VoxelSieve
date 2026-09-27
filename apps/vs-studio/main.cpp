// vs-studio: the VoxelSieve studio (ADR 0008). Serves the browser UI on a local port, or with
// --mcp the studio API to AI systems over the Model Context Protocol on stdin/stdout.

#include <pthread.h>

#include <csignal>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "voxelsieve/http_server.hpp"
#include "voxelsieve/mcp.hpp"
#include "voxelsieve/studio.hpp"

namespace {

constexpr std::string_view kUsage = R"(Usage: vs-studio [options]
       vs-studio --mcp [options]

Starts the VoxelSieve studio: open http://localhost:8410 in a browser for the wizard (choose a
dataset, run operations, create the report) with the step protocol and undo/redo.

With --mcp it serves the studio over the Model Context Protocol (JSON-RPC on stdin/stdout)
instead. Register it with an MCP client as a stdio server, for example:
  {"command": "vs-studio", "args": ["--mcp", "--project", "/data/casting.vsproj"]}
Every studio function is a tool: projects, undo/redo, operations (run_<id>), dataset info and
reading result files.

Options:
  --port <n>            Port of the browser UI (default 8410, 0 picks a free one)
  --host <address>      Address to listen on (default 127.0.0.1). The server has no
                        authentication; keep it on the local machine
  --mcp                 Serve MCP on stdin/stdout instead of the browser UI
  --project <dir>       Open this project at start; created if the directory has no project
  --plugins <dir>       Load operation plugins (*.so) from <dir>; repeatable. Directories in
                        VOXELSIEVE_PLUGIN_PATH (colon-separated) are loaded too
  -h, --help            Show this help
)";

struct Options {
  bool mcp = false;
  std::string host = "127.0.0.1";
  unsigned short port = 8410;
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
    } else if (arg == "--host") {
      options.host = next();
    } else if (arg == "--port") {
      const int port = std::stoi(next());
      if (port < 0 || port > 65535) {
        throw std::invalid_argument("Port out of range");
      }
      options.port = static_cast<unsigned short>(port);
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
    if (options->mcp) {
      voxelsieve::McpServer::serve(studio, std::cin, std::cout);
      return 0;
    }
    // Block the stop signals in all threads; this thread waits for them.
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);
    voxelsieve::HttpServer server(studio, options->host, options->port);
    std::cerr << "vs-studio: http://" << (options->host == "0.0.0.0" ? "localhost" : options->host)
              << ":" << server.port() << "/  (Ctrl+C to stop)\n";
    std::thread thread([&server] { server.run(); });
    int signal = 0;
    sigwait(&signals, &signal);
    server.stop();
    thread.join();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "vs-studio: " << error.what() << "\n";
    return 1;
  }
}
