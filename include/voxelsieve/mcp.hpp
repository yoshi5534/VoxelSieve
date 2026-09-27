#pragma once

#include <functional>
#include <iosfwd>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

#include "voxelsieve/studio.hpp"

namespace voxelsieve {

/// Model Context Protocol server over the studio API (ADR 0008): JSON-RPC 2.0 messages, one per
/// line, as in the MCP stdio transport. Every studio method is a tool; results come back as JSON
/// text and as structured content.
class McpServer {
 public:
  /// `send` writes one message (a notification such as progress) to the client.
  McpServer(Studio& studio, std::function<void(const nlohmann::json&)> send);

  /// Handles one message and returns the response, or nothing for notifications.
  std::optional<nlohmann::json> handle(const nlohmann::json& message);

  /// Reads messages line by line from `in` until end of input and writes responses to `out`.
  static void serve(Studio& studio, std::istream& in, std::ostream& out);

 private:
  nlohmann::json callTool(const nlohmann::json& params);

  Studio& studio_;
  std::function<void(const nlohmann::json&)> send_;
};

}  // namespace voxelsieve
