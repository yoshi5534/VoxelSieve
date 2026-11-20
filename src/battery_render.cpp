// The images of a battery cell inspection.

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#include "detail/battery.hpp"
#include "voxelsieve/telemetry.hpp"

namespace voxelsieve::detail::battery {
namespace {

std::array<std::uint8_t, 3> greyPixel(double value, double lo, double hi) {
  const double t = std::clamp((value - lo) / std::max(1e-9, hi - lo), 0.0, 1.0);
  const auto g = static_cast<std::uint8_t>(std::lround(255.0 * t));
  return {g, g, g};
}

/// Drawing on a BatteryImage.
struct Canvas {
  BatteryImage& image;

  void set(std::int64_t x, std::int64_t y, const std::array<std::uint8_t, 3>& c) const {
    if (x < 0 || y < 0 || x >= image.width || y >= image.height) {
      return;
    }
    for (std::size_t k = 0; k < 3; ++k) {
      image.rgb[at(3 * (x + image.width * y)) + k] = c[k];
    }
  }
  void dot(double x, double y, int radius, const std::array<std::uint8_t, 3>& c) const {
    const auto cx = static_cast<std::int64_t>(std::lround(x));
    const auto cy = static_cast<std::int64_t>(std::lround(y));
    for (int dy = -radius; dy <= radius; ++dy) {
      for (int dx = -radius; dx <= radius; ++dx) {
        if (dx * dx + dy * dy <= radius * radius) {
          set(cx + dx, cy + dy, c);
        }
      }
    }
  }
  void ring(double x, double y, double radius, const std::array<std::uint8_t, 3>& c) const {
    const int steps = std::max(16, static_cast<int>(radius * 8.0));
    for (int k = 0; k < steps; ++k) {
      const double t = 2.0 * kPi * k / steps;
      set(std::lround(x + radius * std::cos(t)), std::lround(y + radius * std::sin(t)), c);
    }
  }
};

constexpr std::array<std::uint8_t, 3> kCathodeColor{230, 60, 50};
constexpr std::array<std::uint8_t, 3> kAnodeColor{60, 140, 240};
constexpr std::array<std::uint8_t, 3> kParticleColor{255, 210, 0};
constexpr std::array<std::uint8_t, 3> kVoidColor{230, 40, 230};
/// Longest side of an image of a plane; larger planes are shown every n-th voxel.
constexpr std::int64_t kMaxImageSide = 2048;

double displayHigh(const BatteryLevels& l) {
  return std::max({l.cathode, l.anode, std::isnan(l.housing) ? 0.0 : l.housing});
}

/// A plane of the dataset as an image, u to the right, v downwards, every `stride`-th voxel.
BatteryImage planeImage(const std::string& name, const Plane& plane, const BatteryLevels& levels,
                        std::int64_t& stride) {
  stride =
      std::max<std::int64_t>(1, (std::max(plane.nu, plane.nv) + kMaxImageSide - 1) / kMaxImageSide);
  BatteryImage image;
  image.name = name;
  image.width = (plane.nu + stride - 1) / stride;
  image.height = (plane.nv + stride - 1) / stride;
  image.rgb.assign(at(3 * image.width * image.height), 0);
  image.pixel_width_mm = plane.pu * static_cast<double>(stride);
  image.pixel_height_mm = plane.pv * static_cast<double>(stride);
  const double high = displayHigh(levels);
  const Canvas canvas{image};
  for (std::int64_t j = 0; j < image.height; ++j) {
    for (std::int64_t i = 0; i < image.width; ++i) {
      canvas.set(i, j, greyPixel(plane.voxel(i * stride, j * stride), levels.air, high));
    }
  }
  return image;
}

/// Pixel of a position in mm in an image of `plane`.
std::array<double, 2> planePixel(const Plane& plane, std::int64_t stride, double u_mm,
                                 double v_mm) {
  return {(u_mm / plane.pu - static_cast<double>(plane.u0)) / static_cast<double>(stride),
          (v_mm / plane.pv - static_cast<double>(plane.v0)) / static_cast<double>(stride)};
}

void markFindings(BatteryImage& image, const BatteryResult& result, const Plane& plane,
                  std::int64_t stride) {
  const Canvas canvas{image};
  for (const auto* list : {&result.particles, &result.voids}) {
    const auto color = list == &result.particles ? kParticleColor : kVoidColor;
    for (const CellFinding& f : *list) {
      const auto p =
          planePixel(plane, stride, f.center_mm[at(plane.u_axis)], f.center_mm[at(plane.v_axis)]);
      const double r = std::max(4.0, 0.5 * f.size_mm / image.pixel_width_mm + 3.0);
      canvas.ring(p[0], p[1], r, color);
      canvas.ring(p[0], p[1], r + 1.0, color);
    }
  }
}

/// Image of a section across an edge: across to the right, the edge at the top, with the ends of
/// the electrode layers of that section.
BatteryImage sectionImage(const SectionImage& section, const BatteryResult& result, int edge) {
  BatteryImage image;
  const BatteryEdge& e = result.edges[at(edge)];
  image.name = "overhang_" + std::string(1, "xyz"[e.axis]) + (e.side == 0 ? "_min" : "_max");
  image.width = section.across;
  image.height = section.along;
  image.rgb.assign(at(3 * section.across * section.along), 0);
  image.pixel_width_mm = section.across_step_mm;
  image.pixel_height_mm = std::abs(section.along_step_mm);
  const Canvas canvas{image};
  const double high = displayHigh(result.levels);
  for (std::int64_t s = 0; s < section.along; ++s) {
    for (std::int64_t a = 0; a < section.across; ++a) {
      canvas.set(a, section.along - 1 - s, greyPixel(section.value(a, s), result.levels.air, high));
    }
  }
  const OverhangSample& worst = result.overhang[at(e.worst)];
  for (const OverhangSample& sample : result.overhang) {
    if (sample.edge != edge || sample.section != worst.section) {
      continue;
    }
    const double x = (sample.across_mm - section.across_from_mm) / section.across_step_mm;
    const double xa = (sample.anode_across_mm - section.across_from_mm) / section.across_step_mm;
    const auto yc = static_cast<double>(section.along - 1) -
                    (sample.cathode_end_mm - section.along_from_mm) / section.along_step_mm;
    const auto ya = static_cast<double>(section.along - 1) -
                    (sample.anode_end_mm - section.along_from_mm) / section.along_step_mm;
    const int r = sample.layer == worst.layer ? 2 : 1;
    canvas.dot(x, yc, r, kCathodeColor);
    canvas.dot(xa, ya, r, kAnodeColor);
  }
  return image;
}

}  // namespace

/// Renders the images: the cross-section from `cross`, the longitudinal section from `along`.
void renderImages(const VolumeView& cross_view, const VolumeView& along, const Extras& extras,
                  BatteryResult& result) {
  const TelemetryPhase phase("images");
  const float air = cross_view.info().air_level;
  std::int64_t stride = 1;
  const Plane cross = readPlane(cross_view, extras.cross_axis, extras.cross_index, extras.box.lo,
                                extras.box.hi, air);
  BatteryImage image = planeImage("cross_section", cross, result.levels, stride);
  {
    const Canvas canvas{image};
    for (const LayerPoint& point : extras.points) {
      const auto p = planePixel(cross, stride, point.mm[0], point.mm[1]);
      canvas.set(std::lround(p[0]), std::lround(p[1]), point.cathode ? kCathodeColor : kAnodeColor);
    }
  }
  markFindings(image, result, cross, stride);
  result.images.push_back(std::move(image));
  const Plane longitudinal =
      readPlane(along, extras.long_axis, extras.long_index, extras.box.lo, extras.box.hi, air);
  image = planeImage("longitudinal", longitudinal, result.levels, stride);
  markFindings(image, result, longitudinal, stride);
  result.images.push_back(std::move(image));
  for (std::size_t e = 0; e < extras.worst.size() && e < result.edges.size(); ++e) {
    if (!extras.worst[e].values.empty() && result.edges[e].worst >= 0) {
      result.images.push_back(sectionImage(extras.worst[e], result, static_cast<int>(e)));
    }
  }
}

}  // namespace voxelsieve::detail::battery
