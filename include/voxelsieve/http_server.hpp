#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "voxelsieve/studio.hpp"

namespace voxelsieve {

/// Local HTTP server of the studio (ADR 0008): serves the browser UI and the studio API.
///
///   GET  /                                  the UI (index.html, app.js, app.css)
///   GET  /api/methods                       the API methods with parameter schemas
///   POST /api/call/<method>                 JSON parameters in, JSON result out (400: {error})
///   GET  /files/<step>/<output>/<file>      a file of a step output, such as report.html
///   GET  /api/tile?axis=&index=&level=&u=&v=&size=[&step=][&porosity=]
///                                           a slice tile for the viewer (see readSlice): float32
///                                           grey values, then one overlay byte per pixel
///
/// Requests must name the local host in the Host header and API calls must send JSON, which
/// keeps other web sites open in the same browser from using the server.
class HttpServer {
 public:
  /// Binds to `host`:`port`; port 0 picks a free port.
  HttpServer(Studio& studio, const std::string& host, unsigned short port);
  ~HttpServer();
  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;
  HttpServer(HttpServer&&) = delete;
  HttpServer& operator=(HttpServer&&) = delete;

  [[nodiscard]] unsigned short port() const;
  /// Accepts connections until `stop()`; each connection is served on its own thread.
  void run();
  /// Stops accepting, closes open connections and waits for their threads. Thread-safe.
  void stop();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// A file of the compiled-in browser UI ("index.html", "app.js", "viewer.js", "app.css"); empty
/// if unknown.
[[nodiscard]] std::string_view uiResource(std::string_view name);

}  // namespace voxelsieve
