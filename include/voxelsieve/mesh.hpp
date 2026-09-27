#pragma once

#include <array>
#include <filesystem>
#include <vector>

namespace voxelsieve {

/// Triangle soup in mm, as read from STL. Vertices are not shared between triangles.
struct Mesh {
  std::vector<std::array<std::array<float, 3>, 3>> triangles;
};

struct Bounds {
  std::array<double, 3> min{0.0, 0.0, 0.0};
  std::array<double, 3> max{0.0, 0.0, 0.0};
};

/// Reads binary or ASCII STL; coordinates are taken as mm.
[[nodiscard]] Mesh readStl(const std::filesystem::path& path);

/// Writes binary STL with zero normals (readers recompute them from the winding).
void writeStl(const std::filesystem::path& path, const Mesh& mesh);

[[nodiscard]] Bounds meshBounds(const Mesh& mesh);

/// Enclosed volume in mm^3 by the divergence theorem. Exact for a closed mesh with outward
/// (counter-clockwise) winding.
[[nodiscard]] double meshVolumeMm3(const Mesh& mesh);

/// Closed, outward-oriented box mesh with the given edge lengths, centred on `center_mm`.
[[nodiscard]] Mesh boxMesh(const std::array<double, 3>& size_mm,
                           const std::array<double, 3>& center_mm = {0.0, 0.0, 0.0});

}  // namespace voxelsieve
