#pragma once

#include <functional>
#include <iosfwd>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>

#include "voxelsieve/studio.hpp"

namespace voxelsieve {

/// Model Context Protocol server over the studio API (ADR 0008): JSON-RPC 2.0 messages, one per
/// line, as in the MCP stdio transport. Every studio method is a tool; results come back as JSON
/// text and as structured content. A client cancels a running operation with
/// notifications/cancelled; the operation stops at its next progress report.
class McpServer {
 public:
  /// `send` writes one message (a notification such as progress) to the client.
  McpServer(Studio& studio, std::function<void(const nlohmann::json&)> send);

  /// Handles one message and returns the response, or nothing for notifications.
  std::optional<nlohmann::json> handle(const nlohmann::json& message);

  /// Reads messages line by line from `in` until end of input and writes responses to `out`.
  /// Tool calls run on their own threads, so cancellations and queries are read while an
  /// operation runs; at the end of input running operations are cancelled and awaited.
  static void serve(Studio& studio, std::istream& in, std::ostream& out);

  /// True once the request with this id was cancelled; its response is then not sent.
  [[nodiscard]] bool cancelled(const nlohmann::json& id) const;

 private:
  nlohmann::json callTool(const nlohmann::json& id, const nlohmann::json& params);

  Studio& studio_;
  std::function<void(const nlohmann::json&)> send_;
  mutable std::mutex mutex_;
  std::set<std::string> running_;    // ids (dumped) of tool calls in progress
  std::set<std::string> cancelled_;  // ids (dumped) of cancelled calls
};

}  // namespace voxelsieve
