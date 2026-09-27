#include "voxelsieve/mesh.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

namespace voxelsieve {
namespace {

using Vertex = std::array<float, 3>;
using Triangle = std::array<Vertex, 3>;

constexpr std::size_t kBinaryHeaderBytes = 80;
constexpr std::size_t kBinaryTriangleBytes = 50;  // normal, 3 vertices, attribute count

Mesh readBinaryStl(const std::string& bytes) {
  std::uint32_t count = 0;
  std::memcpy(&count, bytes.data() + kBinaryHeaderBytes, sizeof(count));
  Mesh mesh;
  mesh.triangles.resize(count);
  const char* data = bytes.data() + kBinaryHeaderBytes + sizeof(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    // Skip the stored normal; the winding defines the orientation.
    std::memcpy(mesh.triangles[i].data(), data + std::size_t{i} * kBinaryTriangleBytes + 12,
                sizeof(Triangle));
  }
  return mesh;
}

Mesh readAsciiStl(const std::string& text, const std::filesystem::path& path) {
  Mesh mesh;
  std::istringstream in(text);
  std::string word;
  Triangle triangle{};
  std::size_t vertex = 0;
  while (in >> word) {
    if (word != "vertex") {
      continue;
    }
    Vertex& v = triangle[vertex];
    if (!(in >> v[0] >> v[1] >> v[2])) {
      throw std::runtime_error("Malformed vertex in " + path.string());
    }
    if (++vertex == 3) {
      mesh.triangles.push_back(triangle);
      vertex = 0;
    }
  }
  if (vertex != 0) {
    throw std::runtime_error("Incomplete triangle in " + path.string());
  }
  return mesh;
}

}  // namespace

Mesh readStl(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("Cannot open " + path.string());
  }
  std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

  // Binary STL is identified by its size; ASCII files may also start with "solid".
  if (bytes.size() >= kBinaryHeaderBytes + 4) {
    std::uint32_t count = 0;
    std::memcpy(&count, bytes.data() + kBinaryHeaderBytes, sizeof(count));
    if (bytes.size() == kBinaryHeaderBytes + 4 + std::size_t{count} * kBinaryTriangleBytes) {
      return readBinaryStl(bytes);
    }
  }
  if (bytes.starts_with("solid")) {
    Mesh mesh = readAsciiStl(bytes, path);
    if (!mesh.triangles.empty()) {
      return mesh;
    }
  }
  throw std::runtime_error("Not an STL file: " + path.string());
}

void writeStl(const std::filesystem::path& path, const Mesh& mesh) {
  std::ofstream out(path, std::ios::binary);
  const std::string header(kBinaryHeaderBytes, ' ');
  out.write(header.data(), static_cast<std::streamsize>(header.size()));
  const auto count = static_cast<std::uint32_t>(mesh.triangles.size());
  out.write(reinterpret_cast<const char*>(&count), sizeof(count));
  const std::array<float, 3> normal{0.0F, 0.0F, 0.0F};
  const std::uint16_t attributes = 0;
  for (const Triangle& triangle : mesh.triangles) {
    out.write(reinterpret_cast<const char*>(normal.data()), sizeof(normal));
    out.write(reinterpret_cast<const char*>(triangle.data()), sizeof(Triangle));
    out.write(reinterpret_cast<const char*>(&attributes), sizeof(attributes));
  }
  if (!out) {
    throw std::runtime_error("Write failed: " + path.string());
  }
}

Bounds meshBounds(const Mesh& mesh) {
  if (mesh.triangles.empty()) {
    return {};
  }
  Bounds bounds;
  bounds.min.fill(std::numeric_limits<double>::infinity());
  bounds.max.fill(-std::numeric_limits<double>::infinity());
  for (const Triangle& triangle : mesh.triangles) {
    for (const Vertex& v : triangle) {
      for (std::size_t i = 0; i < 3; ++i) {
        bounds.min[i] = std::min(bounds.min[i], static_cast<double>(v[i]));
        bounds.max[i] = std::max(bounds.max[i], static_cast<double>(v[i]));
      }
    }
  }
  return bounds;
}

double meshVolumeMm3(const Mesh& mesh) {
  double volume = 0.0;
  for (const Triangle& t : mesh.triangles) {
    const auto& [a, b, c] = t;
    // a . (b x c) / 6, the signed volume of the tetrahedron with the origin.
    const double cx = double{b[1]} * c[2] - double{b[2]} * c[1];
    const double cy = double{b[2]} * c[0] - double{b[0]} * c[2];
    const double cz = double{b[0]} * c[1] - double{b[1]} * c[0];
    volume += a[0] * cx + a[1] * cy + a[2] * cz;
  }
  return volume / 6.0;
}

Mesh boxMesh(const std::array<double, 3>& size_mm, const std::array<double, 3>& center_mm) {
  std::array<Vertex, 8> corners{};
  for (std::size_t i = 0; i < 8; ++i) {
    for (std::size_t axis = 0; axis < 3; ++axis) {
      const double sign = ((i >> axis) & 1U) != 0 ? 0.5 : -0.5;
      corners[i][axis] = static_cast<float>(center_mm[axis] + sign * size_mm[axis]);
    }
  }
  // Corner index bits: x = 1, y = 2, z = 4. Faces as quads with outward counter-clockwise winding.
  constexpr std::array<std::array<std::size_t, 4>, 6> kFaces{{
      {0, 2, 3, 1},  // z-
      {4, 5, 7, 6},  // z+
      {0, 1, 5, 4},  // y-
      {2, 6, 7, 3},  // y+
      {0, 4, 6, 2},  // x-
      {1, 3, 7, 5},  // x+
  }};
  Mesh mesh;
  for (const auto& f : kFaces) {
    mesh.triangles.push_back({corners[f[0]], corners[f[1]], corners[f[2]]});
    mesh.triangles.push_back({corners[f[0]], corners[f[2]], corners[f[3]]});
  }
  return mesh;
}

}  // namespace voxelsieve
