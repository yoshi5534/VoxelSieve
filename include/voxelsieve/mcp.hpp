#pragma once

#include <cstdint>
#include <functional>
#include <iosfwd>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>

#include "voxelsieve/studio.hpp"

namespace voxelsieve {

/// Which studio methods an MCP client sees as tools.
enum class McpTools : std::uint8_t {
  kAnalysis,  ///< everything but the methods that keep the browser UI's view state
  kAll,       ///< every studio method
};

/// Model Context Protocol server over the studio API (ADR 0008): JSON-RPC 2.0 messages, one per
/// line, as in the MCP stdio transport. Studio methods are tools with annotations (read-only,
/// destructive); results come back as JSON text and as structured content. The result files of
/// the open project's steps (reports, porosity results, their pictures) are resources, and a few
/// prompts walk through typical inspections. A client cancels a running operation with
/// notifications/cancelled; the operation stops at its next progress report.
class McpServer {
 public:
  /// `send` writes one message (a notification such as progress) to the client.
  McpServer(Studio& studio, std::function<void(const nlohmann::json&)> send,
            McpTools tools = McpTools::kAnalysis);

  /// Handles one message and returns the response, or nothing for notifications.
  std::optional<nlohmann::json> handle(const nlohmann::json& message);

  /// Reads messages line by line from `in` until end of input and writes responses to `out`.
  /// Tool calls run on their own threads, so cancellations and queries are read while an
  /// operation runs; at the end of input running operations are cancelled and awaited.
  static void serve(Studio& studio, std::istream& in, std::ostream& out,
                    McpTools tools = McpTools::kAnalysis);

  /// True once the request with this id was cancelled; its response is then not sent.
  [[nodiscard]] bool cancelled(const nlohmann::json& id) const;

 private:
  [[nodiscard]] bool offered(const std::string& tool) const;
  nlohmann::json callTool(const nlohmann::json& id, const nlohmann::json& params);
  nlohmann::json listResources() const;
  nlohmann::json readResource(const nlohmann::json& params) const;

  Studio& studio_;
  std::function<void(const nlohmann::json&)> send_;
  McpTools tools_;
  mutable std::mutex mutex_;
  std::set<std::string> running_;    // ids (dumped) of tool calls in progress
  std::set<std::string> cancelled_;  // ids (dumped) of cancelled calls
};

}  // namespace voxelsieve
