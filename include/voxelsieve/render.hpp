#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <vector>

#include "voxelsieve/surface.hpp"

namespace voxelsieve {

/// Software renderer for report and documentation images: a shaded, anti-aliased view of a
/// triangle mesh and spheres (pores, zones) with an orthographic camera, ambient occlusion and a
/// light studio background. Runs without a GPU, so every step can write its images.

struct RenderSphere {
  std::array<double, 3> center{};
  double radius = 0.0;
  std::array<std::uint8_t, 3> color{200, 40, 40};
};

struct RenderScene {
  /// The part; may be empty. Any unit, the camera fits the scene.
  const IndexedMesh* mesh = nullptr;
  /// One colour per mesh vertex, or empty for `surface_color`.
  std::vector<std::array<std::uint8_t, 3>> vertex_colors;
  std::array<std::uint8_t, 3> surface_color{196, 200, 207};
  /// Below 1 the mesh is drawn as glass: spheres inside it shine through, and faces seen
  /// edge-on stay more opaque, so the outline of the part remains readable.
  double surface_opacity = 1.0;
  std::vector<RenderSphere> spheres;
};

struct RenderView {
  /// Direction from the scene towards the camera: azimuth about z from +x, and elevation above
  /// the xy plane, in degrees. z is up in the image.
  double azimuth_degrees = -60.0;
  double elevation_degrees = 25.0;
  int width = 1200;
  int height = 900;
  /// Samples per pixel along each axis, 1 to 4.
  int supersampling = 3;
  /// Free border around the scene as a fraction of the image.
  double margin = 0.06;
  /// Transparent background (RGBA output) instead of the studio gradient.
  bool transparent = false;
};

struct RenderImage {
  int width = 0;
  int height = 0;
  int channels = 3;  // 3: RGB, 4: RGBA
  std::vector<std::uint8_t> pixels;
};

[[nodiscard]] RenderImage render(const RenderScene& scene, const RenderView& view = {});

void writePng(const RenderImage& image, const std::filesystem::path& file);

}  // namespace voxelsieve
