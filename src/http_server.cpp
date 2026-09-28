#include "voxelsieve/http_server.hpp"

#include <sys/socket.h>

#include <bit>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <cstring>
#include <iostream>
#include <list>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace voxelsieve {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;
using Json = nlohmann::json;
using Request = http::request<http::string_body>;
constexpr std::uint64_t kMaxBodyBytes = std::uint64_t{64} << 20U;

std::string percentDecode(std::string_view text) {
  std::string decoded;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%' && i + 2 < text.size()) {
      const std::string hex(text.substr(i + 1, 2));
      decoded.push_back(static_cast<char>(std::stoi(hex, nullptr, 16)));
      i += 2;
    } else {
      decoded.push_back(text[i]);
    }
  }
  return decoded;
}

std::string_view contentType(const std::filesystem::path& path) {
  const std::string extension = path.extension().string();
  if (extension == ".html") {
    return "text/html; charset=utf-8";
  }
  if (extension == ".js") {
    return "text/javascript; charset=utf-8";
  }
  if (extension == ".css") {
    return "text/css; charset=utf-8";
  }
  if (extension == ".json") {
    return "application/json";
  }
  if (extension == ".png") {
    return "image/png";
  }
  if (extension == ".txt" || extension == ".csv") {
    return "text/plain; charset=utf-8";
  }
  return "application/octet-stream";
}

/// Host name of a Host header value, without the port.
std::string hostName(std::string_view host) {
  if (host.starts_with("[")) {
    return std::string(host.substr(0, host.find(']') + 1));
  }
  return std::string(host.substr(0, host.find(':')));
}

}  // namespace

struct HttpServer::Impl {
  struct Connection {
    tcp::socket socket;
    std::thread thread;
    std::atomic<bool> done = false;
    explicit Connection(tcp::socket s) : socket(std::move(s)) {}
  };

  Impl(Studio& s, const std::string& host, unsigned short requested_port)
      : studio(s), acceptor(io), bound_host(host) {
    const tcp::endpoint endpoint(asio::ip::make_address(host), requested_port);
    acceptor.open(endpoint.protocol());
    acceptor.set_option(asio::socket_base::reuse_address(true));
    acceptor.bind(endpoint);
    acceptor.listen();
    port = acceptor.local_endpoint().port();
  }

  [[nodiscard]] bool allowedHost(std::string_view header) const {
    const std::string name = hostName(header);
    return name == "localhost" || name == "127.0.0.1" || name == "[::1]" || name == bound_host;
  }

  void accept() {
    acceptor.async_accept([this](const boost::system::error_code& error, tcp::socket socket) {
      if (!error) {
        start(std::move(socket));
      }
      if (acceptor.is_open()) {
        accept();
      }
    });
  }

  void start(tcp::socket socket) {
    const std::scoped_lock lock(mutex);
    if (stopped) {
      return;
    }
    for (auto it = connections.begin(); it != connections.end();) {
      if ((*it)->done) {
        (*it)->thread.join();
        it = connections.erase(it);
      } else {
        ++it;
      }
    }
    auto& connection = connections.emplace_back(std::make_unique<Connection>(std::move(socket)));
    Connection* raw = connection.get();
    raw->thread = std::thread([this, raw] {
      serve(raw->socket);
      raw->done = true;
    });
  }

  void serve(tcp::socket& socket) {
    beast::flat_buffer buffer;
    boost::system::error_code error;
    while (true) {
      // Saved views carry a picture of the canvas, so bodies may be larger than Beast's 1 MB.
      http::request_parser<http::string_body> parser;
      parser.body_limit(kMaxBodyBytes);
      http::read(socket, buffer, parser, error);
      const Request request = parser.release();
      if (error) {
        break;
      }
      const bool keep_alive = request.keep_alive();
      beast::write(socket, handle(request), error);
      if (error || !keep_alive) {
        break;
      }
    }
    [[maybe_unused]] const auto shut = socket.shutdown(tcp::socket::shutdown_send, error);
  }

  static http::message_generator text(const Request& request, http::status status,
                                      std::string_view type, std::string body) {
    http::response<http::string_body> response(status, request.version());
    response.set(http::field::content_type, type);
    response.set(http::field::cache_control, "no-store");
    response.set("X-Content-Type-Options", "nosniff");
    response.keep_alive(request.keep_alive());
    response.body() = std::move(body);
    response.prepare_payload();
    return response;
  }

  static http::message_generator json(const Request& request, http::status status,
                                      const Json& value) {
    return text(request, status, "application/json",
                value.dump(-1, ' ', false, Json::error_handler_t::replace));
  }

  static http::message_generator error(const Request& request, http::status status,
                                       const std::string& message) {
    return json(request, status, {{"error", message}});
  }

  http::message_generator handle(const Request& request) {
    if (!allowedHost(request[http::field::host])) {
      return error(request, http::status::forbidden, "Host not allowed");
    }
    const std::string_view target = request.target();
    const std::string_view path = target.substr(0, target.find('?'));
    try {
      if (request.method() == http::verb::get) {
        return get(request, path);
      }
      if (request.method() == http::verb::post && path.starts_with("/api/call/")) {
        if (!request[http::field::content_type].starts_with("application/json")) {
          return error(request, http::status::unsupported_media_type, "Send application/json");
        }
        const std::string method(path.substr(std::string_view("/api/call/").size()));
        const Json params = request.body().empty() ? Json::object() : Json::parse(request.body());
        return json(request, http::status::ok, studio.call(method, params));
      }
      return error(request, http::status::not_found, "Not found");
    } catch (const Json::parse_error& failure) {
      return error(request, http::status::bad_request, failure.what());
    } catch (const std::invalid_argument& failure) {
      return error(request, http::status::bad_request, failure.what());
    } catch (const std::exception& failure) {
      return error(request, http::status::internal_server_error, failure.what());
    }
  }

  static std::string_view target(const Request& request) {
    const std::string_view text = request.target();
    const auto query = text.find('?');
    return query == std::string_view::npos ? std::string_view() : text.substr(query + 1);
  }

  /// Integer query parameters.
  static std::map<std::string, std::int64_t> queryValues(std::string_view query) {
    std::map<std::string, std::int64_t> values;
    while (!query.empty()) {
      const auto end = query.find('&');
      const std::string_view pair = query.substr(0, end);
      const auto equals = pair.find('=');
      if (equals != std::string_view::npos) {
        values[std::string(pair.substr(0, equals))] =
            std::stoll(std::string(pair.substr(equals + 1)));
      }
      query = end == std::string_view::npos ? std::string_view() : query.substr(end + 1);
    }
    return values;
  }

  static http::message_generator binary(const Request& request, std::string body,
                                        const std::map<std::string, std::string>& headers) {
    http::response<http::string_body> response(http::status::ok, request.version());
    response.set(http::field::content_type, "application/octet-stream");
    response.set(http::field::cache_control, "no-store");
    for (const auto& [name, value] : headers) {
      response.set(name, value);
    }
    response.keep_alive(request.keep_alive());
    response.body() = std::move(body);
    response.prepare_payload();
    return response;
  }

  /// GET /api/tile?axis=2&index=100&level=1&u=0&v=0&size=256[&step=1][&porosity=2]
  /// Body: width * height float32 grey values (little endian, u fastest), then width * height
  /// overlay bytes (SliceOverlay). Headers X-Width and X-Height give the size.
  http::message_generator tile(const Request& request, std::string_view query) {
    const auto values = queryValues(query);
    const auto value = [&values](const std::string& name, std::int64_t fallback) {
      const auto it = values.find(name);
      return it == values.end() ? fallback : it->second;
    };
    SliceRequest slice;
    slice.axis = static_cast<int>(value("axis", 2));
    slice.index = value("index", 0);
    slice.level = static_cast<int>(value("level", 0));
    slice.origin = {value("u", 0), value("v", 0)};
    const std::int64_t size = value("size", 256);
    slice.size = {size, size};
    const auto optional = [&values](const std::string& name) -> std::optional<int> {
      const auto it = values.find(name);
      return it == values.end() ? std::nullopt : std::optional<int>(static_cast<int>(it->second));
    };
    const SliceImage image = studio.sliceTile(optional("step"), optional("porosity"), slice);
    std::string body(image.grey.size() * sizeof(float) + image.overlay.size(), '\0');
    static_assert(std::endian::native == std::endian::little, "tiles are sent little endian");
    std::memcpy(body.data(), image.grey.data(), image.grey.size() * sizeof(float));
    std::memcpy(body.data() + image.grey.size() * sizeof(float), image.overlay.data(),
                image.overlay.size());
    return binary(
        request, std::move(body),
        {{"X-Width", std::to_string(image.width)}, {"X-Height", std::to_string(image.height)}});
  }

  /// GET /api/volume?max=256[&step=1][&porosity=2]
  /// Body: the volume preview (readVolumePreview) as one grey byte per voxel, then one overlay
  /// byte per voxel, x fastest. Headers give dims, level, voxel size and window.
  http::message_generator volume(const Request& request, std::string_view query) {
    const auto values = queryValues(query);
    const auto optional = [&values](const std::string& name) -> std::optional<int> {
      const auto it = values.find(name);
      return it == values.end() ? std::nullopt : std::optional<int>(static_cast<int>(it->second));
    };
    const auto max = values.contains("max") ? values.at("max") : 256;
    if (max < 16 || max > 512) {
      throw std::invalid_argument("max must be between 16 and 512");
    }
    const VolumePreview preview = studio.volumePreview(optional("step"), optional("porosity"), max);
    std::string body(preview.grey.begin(), preview.grey.end());
    body.append(preview.overlay.begin(), preview.overlay.end());
    return binary(
        request, std::move(body),
        {{"X-Dims", std::to_string(preview.dims[0]) + "," + std::to_string(preview.dims[1]) + "," +
                        std::to_string(preview.dims[2])},
         {"X-Level", std::to_string(preview.level)},
         {"X-Voxel-Size", std::to_string(preview.voxel_size_mm)},
         {"X-Window", std::to_string(preview.low) + "," + std::to_string(preview.high)}});
  }

  /// GET /api/surface?max_triangles=1500000[&step=3]
  /// Body: the display mesh of a surface step (Studio::surfaceMesh), the points as three float32
  /// per vertex in level-0 voxel coordinates, then three uint32 indices per triangle.
  http::message_generator surface(const Request& request, std::string_view query) {
    const auto values = queryValues(query);
    const std::optional<int> step = values.contains("step")
                                        ? std::optional<int>(static_cast<int>(values.at("step")))
                                        : std::nullopt;
    const auto max = values.contains("max_triangles") ? values.at("max_triangles") : 1500000;
    if (max < 1000 || max > 20000000) {
      throw std::invalid_argument("max_triangles must be between 1000 and 20000000");
    }
    const auto mesh = studio.surfaceMesh(step, static_cast<std::size_t>(max));
    const std::size_t point_bytes = mesh->points.size() * sizeof(mesh->points[0]);
    const std::size_t triangle_bytes = mesh->triangles.size() * sizeof(mesh->triangles[0]);
    std::string body(point_bytes + triangle_bytes, '\0');
    std::memcpy(body.data(), mesh->points.data(), point_bytes);
    std::memcpy(body.data() + point_bytes, mesh->triangles.data(), triangle_bytes);
    return binary(request, std::move(body),
                  {{"X-Vertices", std::to_string(mesh->points.size())},
                   {"X-Triangles", std::to_string(mesh->triangles.size())}});
  }

  /// GET /api/deviation[?step=5]
  /// Body: the compared surface of a nominal-actual comparison step (Studio::deviationMesh): three
  /// float32 per vertex in level-0 voxel coordinates, one float32 deviation in mm per vertex, then
  /// three uint32 indices per triangle.
  http::message_generator deviation(const Request& request, std::string_view query) {
    const auto values = queryValues(query);
    const std::optional<int> step = values.contains("step")
                                        ? std::optional<int>(static_cast<int>(values.at("step")))
                                        : std::nullopt;
    const auto view = studio.deviationMesh(step);
    const auto& mesh = view->mesh;
    const std::size_t point_bytes = mesh.points.size() * sizeof(mesh.points[0]);
    const std::size_t deviation_bytes = view->deviation_mm.size() * sizeof(float);
    const std::size_t triangle_bytes = mesh.triangles.size() * sizeof(mesh.triangles[0]);
    std::string body(point_bytes + deviation_bytes + triangle_bytes, '\0');
    std::memcpy(body.data(), mesh.points.data(), point_bytes);
    std::memcpy(body.data() + point_bytes, view->deviation_mm.data(), deviation_bytes);
    std::memcpy(body.data() + point_bytes + deviation_bytes, mesh.triangles.data(), triangle_bytes);
    return binary(request, std::move(body),
                  {{"X-Vertices", std::to_string(mesh.points.size())},
                   {"X-Triangles", std::to_string(mesh.triangles.size())},
                   {"X-Tolerance", std::to_string(view->tolerance_mm)},
                   {"X-Range", std::to_string(view->range_mm)}});
  }

  http::message_generator get(const Request& request, std::string_view path) {
    if (path == "/") {
      path = "/index.html";
    }
    if (const auto resource = uiResource(path.substr(1)); !resource.empty()) {
      return text(request, http::status::ok, contentType(std::string(path)), std::string(resource));
    }
    if (path == "/api/tile") {
      return tile(request, target(request));
    }
    if (path == "/api/volume") {
      return volume(request, target(request));
    }
    if (path == "/api/surface") {
      return surface(request, target(request));
    }
    if (path == "/api/deviation") {
      return deviation(request, target(request));
    }
    if (path == "/api/methods") {
      Json methods = Json::array();
      for (const StudioMethod& method : studio.methods()) {
        methods.push_back({{"name", method.name},
                           {"description", method.description},
                           {"parameters", method.parameters}});
      }
      return json(request, http::status::ok, methods);
    }
    if (path.starts_with("/views/") && path.ends_with(".png")) {
      // /views/<id>.png: the picture of a saved view
      const auto name = path.substr(std::string_view("/views/").size());
      const int id = std::stoi(std::string(name.substr(0, name.size() - 4)));
      return sendFile(request, studio.viewImage(id));
    }
    if (path.starts_with("/files/")) {
      // /files/<step>/<output>/<file...>
      const std::string rest = percentDecode(path.substr(std::string_view("/files/").size()));
      const auto first = rest.find('/');
      const auto second = rest.find('/', first == std::string::npos ? first : first + 1);
      if (first == std::string::npos) {
        return error(request, http::status::not_found, "Not found");
      }
      const int step = std::stoi(rest.substr(0, first));
      const std::string output = rest.substr(first + 1, second - first - 1);
      const std::string file = second == std::string::npos ? "" : rest.substr(second + 1);
      return sendFile(request, studio.outputFile(step, output, file));
    }
    return error(request, http::status::not_found, "Not found");
  }

  static http::message_generator sendFile(const Request& request,
                                          const std::filesystem::path& resolved) {
    http::response<http::file_body> response(http::status::ok, request.version());
    beast::error_code failure;
    response.body().open(resolved.c_str(), beast::file_mode::scan, failure);
    if (failure) {
      return error(request, http::status::not_found, failure.message());
    }
    response.set(http::field::content_type, contentType(resolved));
    response.set(http::field::cache_control, "no-store");
    response.set("X-Content-Type-Options", "nosniff");
    response.keep_alive(request.keep_alive());
    response.prepare_payload();
    return response;
  }

  void stop() {
    asio::post(io, [this] {
      boost::system::error_code ignored;
      [[maybe_unused]] const auto closed = acceptor.close(ignored);
    });
    std::list<std::unique_ptr<Connection>> open;
    {
      const std::scoped_lock lock(mutex);
      stopped = true;
      open.swap(connections);
    }
    for (const auto& connection : open) {
      // Wakes a thread blocked in read; the socket object itself stays with its thread.
      ::shutdown(connection->socket.native_handle(), SHUT_RDWR);
    }
    for (const auto& connection : open) {
      connection->thread.join();
    }
  }

  Studio& studio;
  asio::io_context io;
  tcp::acceptor acceptor;
  std::string bound_host;
  unsigned short port = 0;
  std::mutex mutex;
  std::list<std::unique_ptr<Connection>> connections;
  bool stopped = false;
};

HttpServer::HttpServer(Studio& studio, const std::string& host, unsigned short port)
    : impl_(std::make_unique<Impl>(studio, host, port)) {}

HttpServer::~HttpServer() {
  try {
    stop();
  } catch (...) {
    std::cerr << "voxelsieve: error while stopping the HTTP server\n";
  }
}

unsigned short HttpServer::port() const { return impl_->port; }

void HttpServer::run() {
  impl_->accept();
  impl_->io.run();
}

void HttpServer::stop() { impl_->stop(); }

}  // namespace voxelsieve
