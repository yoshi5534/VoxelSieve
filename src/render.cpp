#include "voxelsieve/render.hpp"

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>
#include <vector>

#include "detail/png.hpp"

namespace voxelsieve {
namespace {

using Vec = std::array<double, 3>;
constexpr double kFar = std::numeric_limits<double>::infinity();

Vec operator+(const Vec& a, const Vec& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec operator-(const Vec& a, const Vec& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec operator*(double s, const Vec& a) { return {s * a[0], s * a[1], s * a[2]}; }
double dot(const Vec& a, const Vec& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec cross(const Vec& a, const Vec& b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
Vec normalized(const Vec& a) {
  const double length = std::sqrt(dot(a, a));
  return length > 0.0 ? (1.0 / length) * a : Vec{0.0, 0.0, 1.0};
}
Vec toVec(const std::array<float, 3>& p) { return {p[0], p[1], p[2]}; }
Vec toColor(const std::array<std::uint8_t, 3>& c) {
  return {c[0] / 255.0, c[1] / 255.0, c[2] / 255.0};
}

/// 5x7 pixel font for labels: one row per entry, the highest of the five bits on the left.
using Glyph = std::array<std::uint8_t, 7>;

Glyph glyph(char c) {
  static constexpr std::array<Glyph, 10> kDigits{{
      {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},  // 0
      {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},  // 1
      {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},  // 2
      {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},  // 3
      {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},  // 4
      {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},  // 5
      {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},  // 6
      {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},  // 7
      {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},  // 8
      {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},  // 9
  }};
  static constexpr std::array<Glyph, 26> kLetters{{
      {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},  // A
      {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E},  // B
      {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E},  // C
      {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C},  // D
      {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F},  // E
      {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10},  // F
      {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F},  // G
      {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},  // H
      {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E},  // I
      {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C},  // J
      {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11},  // K
      {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F},  // L
      {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11},  // M
      {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11},  // N
      {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},  // O
      {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10},  // P
      {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D},  // Q
      {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11},  // R
      {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E},  // S
      {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},  // T
      {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},  // U
      {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04},  // V
      {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A},  // W
      {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11},  // X
      {0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04},  // Y
      {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F},  // Z
  }};
  if (c >= '0' && c <= '9') {
    return kDigits[static_cast<std::size_t>(c - '0')];
  }
  if (c >= 'a' && c <= 'z') {
    c = static_cast<char>(c - 'a' + 'A');
  }
  if (c >= 'A' && c <= 'Z') {
    return kLetters[static_cast<std::size_t>(c - 'A')];
  }
  if (c == '.') {
    return {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C};
  }
  if (c == '-') {
    return {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00};
  }
  return {};
}

/// Blends `color` with coverage `alpha` into pixel (x, y) when it lies in the image.
void blendPixel(RenderImage& image, int x, int y, const std::array<std::uint8_t, 3>& color,
                double alpha) {
  if (x < 0 || y < 0 || x >= image.width || y >= image.height || alpha <= 0.0) {
    return;
  }
  const auto i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) +
                  static_cast<std::size_t>(x)) *
                 static_cast<std::size_t>(image.channels);
  const double a = std::min(alpha, 1.0);
  for (std::size_t c = 0; c < 3; ++c) {
    image.pixels[i + c] = static_cast<std::uint8_t>(
        std::lround((1.0 - a) * image.pixels[i + c] + a * static_cast<double>(color[c])));
  }
  if (image.channels == 4) {
    image.pixels[i + 3] = static_cast<std::uint8_t>(
        std::lround(std::max(static_cast<double>(image.pixels[i + 3]), 255.0 * a)));
  }
}

/// Anti-aliased line of the given width.
void drawLine(RenderImage& image, double x0, double y0, double x1, double y1, double width,
              const std::array<std::uint8_t, 3>& color) {
  const double dx = x1 - x0;
  const double dy = y1 - y0;
  const double length2 = dx * dx + dy * dy;
  const int lo_x = static_cast<int>(std::floor(std::min(x0, x1) - width));
  const int hi_x = static_cast<int>(std::ceil(std::max(x0, x1) + width));
  const int lo_y = static_cast<int>(std::floor(std::min(y0, y1) - width));
  const int hi_y = static_cast<int>(std::ceil(std::max(y0, y1) + width));
  for (int y = lo_y; y <= hi_y; ++y) {
    for (int x = lo_x; x <= hi_x; ++x) {
      const double px = x + 0.5 - x0;
      const double py = y + 0.5 - y0;
      const double t = length2 > 0.0 ? std::clamp((px * dx + py * dy) / length2, 0.0, 1.0) : 0.0;
      const double d = std::hypot(px - t * dx, py - t * dy);
      blendPixel(image, x, y, color, 0.5 * width + 0.5 - d);
    }
  }
}

/// One layer of samples: nearest (or farthest) depth with the normal and colour there.
struct Layer {
  std::vector<double> depth;
  std::vector<Vec> normal;
  std::vector<Vec> color;
  bool keep_far = false;

  Layer(std::size_t samples, bool far) : keep_far(far) {
    depth.assign(samples, far ? -kFar : kFar);
    normal.resize(samples);
    color.resize(samples);
  }
  [[nodiscard]] bool covered(std::size_t i) const { return std::isfinite(depth[i]); }
  [[nodiscard]] bool wins(std::size_t i, double z) const {
    return keep_far ? z > depth[i] : z < depth[i];
  }
};

struct Camera {
  Vec right;
  Vec up;
  Vec forward;  // into the image
  Vec towards;  // from the scene to the camera
  Vec centre;
  double scale = 1.0;  // samples per scene unit
  double half_width = 0.0;
  double half_height = 0.0;

  /// Sample coordinates x, y (y down) and depth along the view direction.
  [[nodiscard]] Vec project(const Vec& p) const {
    const Vec d = p - centre;
    return {half_width + dot(d, right) * scale, half_height - dot(d, up) * scale, dot(d, forward)};
  }
};

Camera makeCamera(const RenderScene& scene, const RenderView& view, int width, int height) {
  const double azimuth = view.azimuth_degrees * std::numbers::pi / 180.0;
  const double elevation = view.elevation_degrees * std::numbers::pi / 180.0;
  Camera camera;
  camera.towards = {std::cos(elevation) * std::cos(azimuth),
                    std::cos(elevation) * std::sin(azimuth), std::sin(elevation)};
  camera.forward = -1.0 * camera.towards;
  const Vec side = cross(camera.forward, {0.0, 0.0, 1.0});
  camera.right = dot(side, side) > 1e-12 ? normalized(side) : Vec{1.0, 0.0, 0.0};
  camera.up = cross(camera.right, camera.forward);

  std::array<double, 2> lo{kFar, kFar};
  std::array<double, 2> hi{-kFar, -kFar};
  const auto extend = [&](const Vec& p, double radius) {
    const double x = dot(p, camera.right);
    const double y = dot(p, camera.up);
    lo = {std::min(lo[0], x - radius), std::min(lo[1], y - radius)};
    hi = {std::max(hi[0], x + radius), std::max(hi[1], y + radius)};
  };
  if (scene.mesh != nullptr) {
    for (const auto& p : scene.mesh->points) {
      extend(toVec(p), 0.0);
    }
  }
  for (const RenderSphere& sphere : scene.spheres) {
    extend(sphere.center, sphere.radius);
  }
  if (lo[0] > hi[0]) {
    lo = {-1.0, -1.0};
    hi = {1.0, 1.0};
  }
  const double w = std::max(hi[0] - lo[0], 1e-9);
  const double h = std::max(hi[1] - lo[1], 1e-9);
  const double usable = 1.0 - 2.0 * std::clamp(view.margin, 0.0, 0.45);
  camera.scale = std::min(width * usable / w, height * usable / h);
  camera.half_width = 0.5 * width;
  camera.half_height = 0.5 * height;
  camera.centre = (0.5 * (lo[0] + hi[0])) * camera.right + (0.5 * (lo[1] + hi[1])) * camera.up;
  return camera;
}

std::vector<Vec> vertexNormals(const IndexedMesh& mesh) {
  std::vector<Vec> normals(mesh.points.size(), Vec{0.0, 0.0, 0.0});
  for (const auto& t : mesh.triangles) {
    const Vec a = toVec(mesh.points[t[0]]);
    const Vec n = cross(toVec(mesh.points[t[1]]) - a, toVec(mesh.points[t[2]]) - a);
    for (const std::uint32_t v : t) {
      normals[v] = normals[v] + n;
    }
  }
  for (Vec& n : normals) {
    n = normalized(n);
  }
  return normals;
}

/// Rasterizes the mesh in horizontal bands, one task per band, so that no two tasks write the
/// same sample.
void rasterizeMesh(const RenderScene& scene, const Camera& camera, int width, int height,
                   const std::vector<Layer*>& layers) {
  const IndexedMesh& mesh = *scene.mesh;
  const std::vector<Vec> normals = vertexNormals(mesh);
  std::vector<Vec> projected(mesh.points.size());
  for (std::size_t i = 0; i < mesh.points.size(); ++i) {
    projected[i] = camera.project(toVec(mesh.points[i]));
  }
  const bool coloured = scene.vertex_colors.size() == mesh.points.size();
  const Vec uniform = toColor(scene.surface_color);
  constexpr int kBand = 64;
  tbb::parallel_for(0, (height + kBand - 1) / kBand, [&](int band) {
    const int band_y0 = band * kBand;
    const int band_y1 = std::min(height, band_y0 + kBand) - 1;
    for (const auto& t : mesh.triangles) {
      const Vec& s0 = projected[t[0]];
      const Vec& s1 = projected[t[1]];
      const Vec& s2 = projected[t[2]];
      const double area = (s1[0] - s0[0]) * (s2[1] - s0[1]) - (s2[0] - s0[0]) * (s1[1] - s0[1]);
      if (std::abs(area) < 1e-12) {
        continue;
      }
      const int x0 = std::max(0, static_cast<int>(std::floor(std::min({s0[0], s1[0], s2[0]}))));
      const int x1 =
          std::min(width - 1, static_cast<int>(std::ceil(std::max({s0[0], s1[0], s2[0]}))));
      const int y0 =
          std::max(band_y0, static_cast<int>(std::floor(std::min({s0[1], s1[1], s2[1]}))));
      const int y1 =
          std::min(band_y1, static_cast<int>(std::ceil(std::max({s0[1], s1[1], s2[1]}))));
      if (y0 > y1) {
        continue;
      }
      for (int y = y0; y <= y1; ++y) {
        const double py = y + 0.5;
        for (int x = x0; x <= x1; ++x) {
          const double px = x + 0.5;
          const double b0 = ((s1[0] - px) * (s2[1] - py) - (s2[0] - px) * (s1[1] - py)) / area;
          const double b1 = ((s2[0] - px) * (s0[1] - py) - (s0[0] - px) * (s2[1] - py)) / area;
          const double b2 = 1.0 - b0 - b1;
          if (b0 < 0.0 || b1 < 0.0 || b2 < 0.0) {
            continue;
          }
          const double z = b0 * s0[2] + b1 * s1[2] + b2 * s2[2];
          const auto i = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                         static_cast<std::size_t>(x);
          bool needed = false;
          for (const Layer* layer : layers) {
            needed = needed || layer->wins(i, z);
          }
          if (!needed) {
            continue;
          }
          const Vec n = normalized(b0 * normals[t[0]] + b1 * normals[t[1]] + b2 * normals[t[2]]);
          const Vec c = coloured ? b0 * toColor(scene.vertex_colors[t[0]]) +
                                       b1 * toColor(scene.vertex_colors[t[1]]) +
                                       b2 * toColor(scene.vertex_colors[t[2]])
                                 : uniform;
          for (Layer* layer : layers) {
            if (layer->wins(i, z)) {
              layer->depth[i] = z;
              layer->normal[i] = n;
              layer->color[i] = c;
            }
          }
        }
      }
    }
  });
}

void rasterizeSpheres(const RenderScene& scene, const Camera& camera, int width, int height,
                      Layer& layer) {
  for (const RenderSphere& sphere : scene.spheres) {
    const Vec c = camera.project(sphere.center);
    const double r = std::max(sphere.radius * camera.scale, 0.5);
    const Vec color = toColor(sphere.color);
    const int x0 = std::max(0, static_cast<int>(std::floor(c[0] - r)));
    const int x1 = std::min(width - 1, static_cast<int>(std::ceil(c[0] + r)));
    const int y0 = std::max(0, static_cast<int>(std::floor(c[1] - r)));
    const int y1 = std::min(height - 1, static_cast<int>(std::ceil(c[1] + r)));
    for (int y = y0; y <= y1; ++y) {
      for (int x = x0; x <= x1; ++x) {
        const double dx = (x + 0.5 - c[0]) / r;
        const double dy = (c[1] - (y + 0.5)) / r;
        const double q = 1.0 - dx * dx - dy * dy;
        if (q < 0.0) {
          continue;
        }
        const double dz = std::sqrt(q);
        const double z = c[2] - dz * r / camera.scale;
        const auto i = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                       static_cast<std::size_t>(x);
        if (!layer.wins(i, z)) {
          continue;
        }
        layer.depth[i] = z;
        layer.normal[i] = dx * camera.right + dy * camera.up + dz * camera.towards;
        layer.color[i] = color;
      }
    }
  }
}

/// Screen-space ambient occlusion of the nearest opaque layer: the fraction of nearby samples
/// that lie clearly in front of this one.
std::vector<double> ambientOcclusion(const Layer& layer, int width, int height, double extent) {
  std::vector<double> ao(layer.depth.size(), 1.0);
  const double radius = 0.018 * std::min(width, height);
  constexpr int kSamples = 16;
  std::array<std::array<double, 2>, kSamples> offsets{};
  for (int k = 0; k < kSamples; ++k) {
    const double angle = k * 2.399963;  // golden angle
    const double distance = radius * std::sqrt((k + 0.5) / kSamples);
    offsets[static_cast<std::size_t>(k)] = {distance * std::cos(angle), distance * std::sin(angle)};
  }
  const double bias = 0.002 * extent;
  const double range = 0.06 * extent;
  tbb::parallel_for(tbb::blocked_range<int>(0, height), [&](const tbb::blocked_range<int>& rows) {
    for (int y = rows.begin(); y < rows.end(); ++y) {
      for (int x = 0; x < width; ++x) {
        const auto i = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                       static_cast<std::size_t>(x);
        if (!layer.covered(i)) {
          continue;
        }
        double occluded = 0.0;
        for (const auto& o : offsets) {
          const int sx = x + static_cast<int>(std::lround(o[0]));
          const int sy = y + static_cast<int>(std::lround(o[1]));
          if (sx < 0 || sy < 0 || sx >= width || sy >= height) {
            continue;
          }
          const double d =
              layer.depth[static_cast<std::size_t>(sy) * static_cast<std::size_t>(width) +
                          static_cast<std::size_t>(sx)];
          const double closer = layer.depth[i] - d;
          if (closer > bias && closer < range) {
            occluded += 1.0 - closer / range;
          }
        }
        ao[i] = 1.0 - 0.75 * occluded / kSamples;
      }
    }
  });
  return ao;
}

struct Lighting {
  Vec key;
  Vec fill;
  Vec half;
  Vec towards;
};

Vec shade(const Lighting& light, Vec n, const Vec& albedo, double ao, double specular) {
  if (dot(n, light.towards) < 0.0) {
    n = -1.0 * n;  // back faces seen through openings
  }
  const double ambient = (0.40 + 0.08 * n[2]) * ao;
  const double diffuse =
      0.62 * std::max(0.0, dot(n, light.key)) + 0.26 * std::max(0.0, dot(n, light.fill));
  const double gloss = specular * std::pow(std::max(0.0, dot(n, light.half)), 48.0) * ao;
  return {std::min(1.0, albedo[0] * (ambient + diffuse) + gloss),
          std::min(1.0, albedo[1] * (ambient + diffuse) + gloss),
          std::min(1.0, albedo[2] * (ambient + diffuse) + gloss)};
}

Vec background(double fy, double fx) {
  // Light soft backdrop: brighter towards the top, slightly darker at the corners.
  const double t = std::clamp(fy, 0.0, 1.0);
  const double vignette = 1.0 - 0.05 * ((fx - 0.5) * (fx - 0.5) + (fy - 0.4) * (fy - 0.4));
  return {(0.985 - 0.10 * t) * vignette, (0.987 - 0.095 * t) * vignette,
          (0.992 - 0.085 * t) * vignette};
}

}  // namespace

RenderImage render(const RenderScene& scene, const RenderView& view) {
  if (view.width <= 0 || view.height <= 0 || view.width > 8192 || view.height > 8192) {
    throw std::invalid_argument("Image size must be 1 to 8192 pixels");
  }
  const int ss = std::clamp(view.supersampling, 1, 4);
  const int width = view.width * ss;
  const int height = view.height * ss;
  const auto samples = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  const Camera camera = makeCamera(scene, view, width, height);
  const double extent =
      std::max(static_cast<double>(width), static_cast<double>(height)) / camera.scale;

  const bool glass = scene.surface_opacity < 1.0;
  Layer opaque(samples, false);
  Layer front(glass ? samples : 0, false);
  Layer back(glass ? samples : 0, true);
  const bool has_mesh = scene.mesh != nullptr && !scene.mesh->triangles.empty();
  if (has_mesh) {
    if (glass) {
      rasterizeMesh(scene, camera, width, height, {&front, &back});
    } else {
      rasterizeMesh(scene, camera, width, height, {&opaque});
    }
  }
  rasterizeSpheres(scene, camera, width, height, opaque);
  const std::vector<double> ao = ambientOcclusion(opaque, width, height, extent);

  Lighting light;
  light.towards = camera.towards;
  light.key = normalized(-0.45 * camera.right + 0.75 * camera.up + 0.55 * camera.towards);
  light.fill = normalized(0.8 * camera.right - 0.1 * camera.up + 0.6 * camera.towards);
  light.half = normalized(light.key + camera.towards);
  const double opacity = std::clamp(scene.surface_opacity, 0.0, 1.0);
  const double edge_jump = 0.015 * extent;

  RenderImage image;
  image.width = view.width;
  image.height = view.height;
  image.channels = view.transparent ? 4 : 3;
  image.pixels.assign(static_cast<std::size_t>(view.width) * static_cast<std::size_t>(view.height) *
                          static_cast<std::size_t>(image.channels),
                      0);
  tbb::parallel_for(tbb::blocked_range<int>(0, view.height), [&](const tbb::blocked_range<int>&
                                                                     rows) {
    for (int py = rows.begin(); py < rows.end(); ++py) {
      for (int px = 0; px < view.width; ++px) {
        std::array<double, 4> sum{};  // premultiplied RGBA
        for (int sy = 0; sy < ss; ++sy) {
          for (int sx = 0; sx < ss; ++sx) {
            const int x = px * ss + sx;
            const int y = py * ss + sy;
            const auto i = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                           static_cast<std::size_t>(x);
            // Back to front: backdrop, far glass wall, opaque geometry, near glass wall.
            Vec color = background(static_cast<double>(y) / height, static_cast<double>(x) / width);
            double alpha = view.transparent ? 0.0 : 1.0;
            if (view.transparent) {
              color = {0.0, 0.0, 0.0};
            }
            const auto over = [&](const Vec& c, double a) {
              color = (1.0 - a) * color + a * c;
              alpha = alpha + a * (1.0 - alpha);
            };
            const double opaque_depth = opaque.depth[i];
            if (glass && back.covered(i) && back.depth[i] > opaque_depth) {
              // Opaque geometry hides the far wall.
            } else if (glass && back.covered(i)) {
              over(shade(light, back.normal[i], back.color[i], 1.0, 0.0), 0.6 * opacity);
            }
            if (opaque.covered(i)) {
              Vec c = shade(light, opaque.normal[i], opaque.color[i], ao[i], 0.22);
              // Dark outline where the depth jumps: silhouettes and steps.
              bool edge = false;
              for (const auto& [dx, dy] :
                   {std::pair{ss, 0}, std::pair{-ss, 0}, std::pair{0, ss}, std::pair{0, -ss}}) {
                const int nx = x + dx;
                const int ny = y + dy;
                if (nx < 0 || ny < 0 || nx >= width || ny >= height) {
                  continue;
                }
                const double d =
                    opaque.depth[static_cast<std::size_t>(ny) * static_cast<std::size_t>(width) +
                                 static_cast<std::size_t>(nx)];
                edge = edge || d - opaque_depth > edge_jump;
              }
              if (edge) {
                c = 0.55 * c;
              }
              over(c, 1.0);
            }
            if (glass && front.covered(i) && front.depth[i] <= opaque_depth) {
              const Vec n = front.normal[i];
              const double facing = std::abs(dot(n, camera.towards));
              // Tinted glass that darkens towards the rims, so the outline stays readable on the
              // light backdrop, with a highlight from the key light.
              const double fresnel = std::pow(1.0 - facing, 2.0);
              const double a = std::min(0.9, opacity + (0.8 - opacity) * fresnel);
              const double gloss =
                  0.5 * std::pow(std::max(0.0, std::abs(dot(n, light.half))), 64.0);
              const Vec tint = (0.55 + 0.45 * facing) * front.color[i];
              over({std::min(1.0, tint[0] + gloss), std::min(1.0, tint[1] + gloss),
                    std::min(1.0, tint[2] + gloss)},
                   a);
            }
            if (view.transparent) {
              // `color` is premultiplied here; `over` blended against black.
              sum[0] += color[0];
              sum[1] += color[1];
              sum[2] += color[2];
            } else {
              sum[0] += color[0] * alpha;
              sum[1] += color[1] * alpha;
              sum[2] += color[2] * alpha;
            }
            sum[3] += alpha;
          }
        }
        const auto n = static_cast<double>(ss * ss);
        const auto out = (static_cast<std::size_t>(py) * static_cast<std::size_t>(view.width) +
                          static_cast<std::size_t>(px)) *
                         static_cast<std::size_t>(image.channels);
        const double a = sum[3] / n;
        for (std::size_t c = 0; c < 3; ++c) {
          double value = sum[c] / n;
          if (view.transparent && a > 0.0) {
            value /= a;
          }
          image.pixels[out + c] =
              static_cast<std::uint8_t>(std::lround(255.0 * std::clamp(value, 0.0, 1.0)));
        }
        if (view.transparent) {
          image.pixels[out + 3] =
              static_cast<std::uint8_t>(std::lround(255.0 * std::clamp(a, 0.0, 1.0)));
        }
      }
    }
  });
  return image;
}

void writePng(const RenderImage& image, const std::filesystem::path& file) {
  const auto png =
      detail::encodePng(static_cast<std::uint32_t>(image.width),
                        static_cast<std::uint32_t>(image.height), image.channels, image.pixels);
  std::ofstream out(file, std::ios::binary);
  if (!out) {
    throw std::runtime_error("Cannot write " + file.string());
  }
  out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
  if (!out) {
    throw std::runtime_error("Cannot write " + file.string());
  }
}

void drawBalloon(RenderImage& image, double x, double y, double anchor_x, double anchor_y,
                 const std::string& text, int scale) {
  constexpr std::array<std::uint8_t, 3> kInk{29, 29, 31};
  constexpr std::array<std::uint8_t, 3> kPaper{255, 255, 255};
  const int s = std::max(scale, 1);
  const double glyph_w = 6.0 * s;  // 5 pixels and a gap
  const double text_w = glyph_w * static_cast<double>(text.size()) - s;
  const double text_h = 7.0 * s;
  const double radius = 0.5 * std::max(text_h, text_w) + 2.5 * s;
  if (std::hypot(anchor_x - x, anchor_y - y) > radius) {
    drawLine(image, x, y, anchor_x, anchor_y, 0.6 * s, kInk);
    // A dot at the anchor.
    for (int py = static_cast<int>(anchor_y - 2 * s); py <= static_cast<int>(anchor_y + 2 * s);
         ++py) {
      for (int px = static_cast<int>(anchor_x - 2 * s); px <= static_cast<int>(anchor_x + 2 * s);
           ++px) {
        blendPixel(image, px, py, kInk,
                   1.5 * s + 0.5 - std::hypot(px + 0.5 - anchor_x, py + 0.5 - anchor_y));
      }
    }
  }
  // Circle with an outline, wide enough for the text.
  const double half_w = std::max(radius, 0.5 * text_w + 3.0 * s);
  const auto inside = [&](double px, double py) {
    // Rounded box: a circle stretched horizontally for long labels.
    const double ex = std::max(std::abs(px - x) - (half_w - radius), 0.0);
    return std::hypot(ex, py - y);
  };
  for (int py = static_cast<int>(y - radius - 2); py <= static_cast<int>(y + radius + 2); ++py) {
    for (int px = static_cast<int>(x - half_w - 2); px <= static_cast<int>(x + half_w + 2); ++px) {
      const double d = inside(px + 0.5, py + 0.5);
      blendPixel(image, px, py, kInk, radius + 0.5 - d);
      blendPixel(image, px, py, kPaper, radius - 0.6 * s + 0.5 - d);
    }
  }
  const double left = x - 0.5 * text_w;
  const double top = y - 0.5 * text_h;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const Glyph g = glyph(text[i]);
    for (int row = 0; row < 7; ++row) {
      for (int col = 0; col < 5; ++col) {
        if ((g[static_cast<std::size_t>(row)] >> (4 - col) & 1U) == 0U) {
          continue;
        }
        const int x0 =
            static_cast<int>(std::lround(left + glyph_w * static_cast<double>(i))) + col * s;
        const int y0 = static_cast<int>(std::lround(top)) + row * s;
        for (int dy = 0; dy < s; ++dy) {
          for (int dx = 0; dx < s; ++dx) {
            blendPixel(image, x0 + dx, y0 + dy, kInk, 1.0);
          }
        }
      }
    }
  }
}

std::vector<std::array<double, 2>> projectToImage(const RenderScene& scene, const RenderView& view,
                                                  std::span<const std::array<double, 3>> points) {
  const Camera camera = makeCamera(scene, view, view.width, view.height);
  std::vector<std::array<double, 2>> out;
  out.reserve(points.size());
  for (const auto& p : points) {
    const Vec q = camera.project(p);
    out.push_back({q[0], q[1]});
  }
  return out;
}

}  // namespace voxelsieve
