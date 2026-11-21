#pragma once

// Internals of the battery cell inspection (battery.cpp and battery_*.cpp, ADR 0029): read
// access to the cell, sections and profiles, the cell types and the images.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <numbers>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "voxelsieve/battery.hpp"
#include "voxelsieve/dataset.hpp"

namespace voxelsieve::detail::battery {

using Index3 = std::array<std::int64_t, 3>;
using Vec3 = std::array<double, 3>;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kPi = std::numbers::pi;
/// Bright or dark voxels collected in the electrode stack at most; more means the threshold
/// catches the layers themselves.
constexpr std::size_t kMaxFindingVoxels = 20'000'000;

inline std::size_t at(std::int64_t i) { return static_cast<std::size_t>(i); }

inline double median(std::vector<double> values) {
  values.erase(std::remove_if(values.begin(), values.end(), [](double v) { return std::isnan(v); }),
               values.end());
  if (values.empty()) {
    return kNaN;
  }
  const auto mid = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
  std::nth_element(values.begin(), mid, values.end());
  return *mid;
}

inline int medianInt(const std::vector<int>& values) {
  if (values.empty()) {
    return 0;
  }
  const std::vector<double> v(values.begin(), values.end());
  return static_cast<int>(std::lround(median(v)));
}

inline double nanMin(double a, double b) {
  if (std::isnan(a)) {
    return b;
  }
  return std::isnan(b) ? a : std::min(a, b);
}

inline double nanMax(double a, double b) {
  if (std::isnan(a)) {
    return b;
  }
  return std::isnan(b) ? a : std::max(a, b);
}

// ---------------------------------------------------------------------------------------------
// Read access, planes and profiles

struct VolumeInfo {
  Index3 dims{};
  VoxelSize voxel_size;
  float air_level = 0.0F;
  float threshold = 0.0F;
  /// Indices along z of the planes that hold measured values, when not all do (cross-sections
  /// given as images; the others repeat the nearest one). Empty: all.
  std::vector<std::int64_t> planes;
};

/// Level 0 of a volume: the regions the analysis reads, with `fill` outside the volume.
class VolumeView {
 public:
  using Reader = std::function<void(const Box&, std::span<float>, float)>;
  VolumeView(VolumeInfo info, Reader reader) : info_(std::move(info)), reader_(std::move(reader)) {}

  [[nodiscard]] const VolumeInfo& info() const { return info_; }
  void readRegion(const Box& box, std::span<float> out, float fill) const {
    reader_(box, out, fill);
  }

 private:
  VolumeInfo info_;
  Reader reader_;
};

/// A slice of level 0 normal to `axis`, over a range of the two other axes (u < v).
struct Plane {
  int u_axis = 0;
  int v_axis = 1;
  std::int64_t u0 = 0;
  std::int64_t v0 = 0;
  std::int64_t nu = 0;
  std::int64_t nv = 0;
  double pu = 1.0;  // pitch in mm
  double pv = 1.0;
  float fill = 0.0F;
  std::vector<float> values;  // u fastest

  [[nodiscard]] float voxel(std::int64_t i, std::int64_t j) const {
    if (i < 0 || j < 0 || i >= nu || j >= nv) {
      return fill;
    }
    return values[at(i + nu * j)];
  }

  /// Bilinear value at (u, v) in mm (voxel index times pitch).
  [[nodiscard]] double sample(double u_mm, double v_mm) const {
    const double fu = u_mm / pu - static_cast<double>(u0);
    const double fv = v_mm / pv - static_cast<double>(v0);
    const double iu = std::floor(fu);
    const double iv = std::floor(fv);
    const double tu = fu - iu;
    const double tv = fv - iv;
    const auto i = static_cast<std::int64_t>(iu);
    const auto j = static_cast<std::int64_t>(iv);
    return (1.0 - tv) * ((1.0 - tu) * voxel(i, j) + tu * voxel(i + 1, j)) +
           tv * ((1.0 - tu) * voxel(i, j + 1) + tu * voxel(i + 1, j + 1));
  }
};

struct Peak {
  double position = 0.0;  // sample index, sub-sample
  double value = 0.0;     // smoothed peak value
  double prominence = 0.0;
  bool cathode = false;
};

/// Two classes of layer brightness by the split of least variance.
struct Classes {
  double split = 0.0;
  double low = 0.0;
  double high = 0.0;
  bool distinct = false;
};

/// The first bright band from the start of a profile (a wall seen from outside): its outer and
/// inner edge at half height, in samples, and its peak. NaN inner edge when nothing darker
/// follows it.
struct Band {
  double outer = kNaN;
  double inner = kNaN;
  double peak = kNaN;
};

/// The entry of the ascending `planes` nearest to `index`.
std::int64_t nearestPlane(const std::vector<std::int64_t>& planes, std::int64_t index);
VolumeView viewOf(const Dataset& dataset);
std::pair<int, int> planeAxes(int axis);
/// Reads a plane into `plane`, reusing its memory.
void readPlaneInto(Plane& plane, const VolumeView& volume, int axis, std::int64_t index,
                   const Index3& lo, const Index3& hi, float fill);
Plane readPlane(const VolumeView& volume, int axis, std::int64_t index, const Index3& lo,
                const Index3& hi, float fill);
/// Radial profile in `plane` from `center` along `angle`, `count` samples of `step` mm from
/// radius 0, averaged over 2 * average + 1 parallel rays one step apart. `noise` receives the
/// noise of the averaged profile from the spread between the rays, which see the same layers.
std::vector<double> rayProfile(const Plane& plane, const std::array<double, 2>& center,
                               double angle, double step, std::int64_t count, int average,
                               double& noise);
std::vector<double> smooth3(std::span<const double> p);
double quantile(std::span<const double> p, double q);
/// Layers along a profile within [from, to): maxima of the smoothed profile with at least
/// `min_prominence`, located at the centroid of their upper half.
std::vector<Peak> findPeaks(std::span<const double> profile, std::size_t from, std::size_t to,
                            double min_prominence);
/// Smallest prominence of a layer in a profile with `noise`: well above the noise and a small
/// share of the contrast.
double minProminence(std::span<const double> profile, double noise);
/// Mean of the minima between neighbouring peaks: the level of the gaps between the layers.
void collectValleys(std::span<const double> profile, const std::vector<Peak>& peaks,
                    std::vector<double>& valleys);
Classes classify(std::vector<double> values);
void label(std::vector<Peak>& peaks, const Classes& classes, bool cathode_brighter);
Band firstBand(std::span<const double> p, double threshold, double air, std::size_t window);
/// Least-squares circle through points (Kåsa): centre and radius.
std::pair<std::array<double, 2>, double> fitCircle(const std::vector<std::array<double, 2>>& pts);

// ---------------------------------------------------------------------------------------------
// Sections across an edge and the layer tracks in them

/// Grey values of a section across an edge: `across` samples across the layers (radius, or
/// position along the stack normal) by `along` samples towards the edge, row by row from the
/// inside outwards, across fastest.
struct SectionImage {
  std::int64_t across = 0;
  std::int64_t along = 0;
  double across_from_mm = 0.0;
  double across_step_mm = 1.0;
  double along_from_mm = 0.0;  // position along the axis of row 0
  double along_step_mm = 1.0;  // signed: towards the edge
  std::vector<float> values;
  /// Rows are valid up to this across sample (the wall of a cylindrical can).
  std::int64_t valid_across = 0;

  [[nodiscard]] double value(std::int64_t a, std::int64_t s) const {
    return values[at(a + across * s)];
  }
};

/// Overhang samples of one section across an edge, with the anode as its own layers where the
/// electrodes differ in brightness, else between the cathodes.
void sectionOverhang(const SectionImage& image, const BatteryLevels& levels,
                     const BatteryOptions& options, int edge, double section,
                     std::vector<OverhangSample>& samples, bool& unfinished, bool& hidden);
/// Leaves out the layer ends of one edge that lie far from the others: a layer lost by the
/// tracking, a tab or the start of a strip, not the edge of the electrode stack. Returns how many.
std::int64_t dropStrayEnds(std::vector<std::vector<OverhangSample>>& per_section, double limit);
void warnStrayEnds(BatteryResult& result, int edge, std::int64_t dropped, double limit);
/// Warnings about layer ends that were not measured.
void warnOverhang(BatteryResult& result, bool unfinished, const std::vector<int>& hidden_edges);
void summarizeEdges(BatteryResult& result);
std::string edgeName(int axis, int side);

// ---------------------------------------------------------------------------------------------
// Overview

struct Overview {
  Index3 lo{};  // level-0 voxels, inclusive
  Index3 hi{};  // exclusive
  CellType type = CellType::kCylindrical;
  int axis = 2;
};

// ---------------------------------------------------------------------------------------------
// Particles, voids and layer counts

/// Whether a voxel lies where particles (`bright`) or voids are looked for.
using InRegion = std::function<bool(const Index3&, bool bright)>;

/// Counts, pitch and gaps of the layers of one profile.
struct ProfileLayers {
  int cathodes = 0;
  int anodes = 0;
  std::vector<double> cathode_spacing;  // samples
  std::vector<double> spacing;          // between neighbouring layers, samples
  double first = kNaN;                  // position of the first and last layer, samples
  double last = kNaN;
};

std::vector<CellFinding> components(std::vector<std::pair<std::uint64_t, float>> voxels,
                                    const Index3& dims, const VoxelSize& pitch,
                                    std::int64_t min_voxels, bool bright);
/// Moves bright components too large for particles out of the particles: tabs, foil ends and
/// welds reach into the electrode stack. Numbers the rest from 1.
void separateStructures(BatteryResult& result, const BatteryOptions& options);
void findParticlesAndVoids(const Dataset& dataset, const InRegion& in_region, BatteryResult& result,
                           const BatteryOptions& options);
void setThresholds(BatteryResult& result, const BatteryOptions& options);
void summarizeSections(BatteryResult& result);
ProfileLayers countLayers(const std::vector<Peak>& peaks);
/// Whether the space between two neighbouring layers is a gap: much wider than usual and dark
/// in its middle, not a layer the peak search missed.
bool isGap(std::span<const double> profile, const Peak& a, const Peak& b, double usual,
           double factor, const BatteryLevels& levels);

// ---------------------------------------------------------------------------------------------
// Cylindrical cells

/// Layer positions of the middle section, for the image.
struct LayerPoint {
  std::array<double, 2> mm{};
  bool cathode = false;
};

struct CylinderData {
  int axis = 2;
  int u = 0;
  int v = 1;
  Overview box;
  double step = 0.0;
  std::int64_t ray_samples = 0;
  /// Per section and angle: core radius and inner radius of the can wall.
  std::vector<std::vector<double>> core;
  std::vector<std::vector<double>> inner;
  /// Per section and angle: radius of the innermost and outermost layer, NaN without layers.
  std::vector<std::vector<double>> first_layer;
  std::vector<std::vector<double>> last_layer;
  double pitch = 0.0;               // between neighbouring layers, mm
  std::vector<double> section_pos;  // mm along the axis
  /// Centre line: centre = c0 + c1 * position along the axis.
  std::array<double, 2> c0{};
  std::array<double, 2> c1{};
  std::vector<LayerPoint> points;
  /// Along the axis: the ends of the anodes (particles are looked for up to them) and of the
  /// cathodes (voids).
  double electrode_low = kNaN;
  double electrode_high = kNaN;
  double cathode_low = kNaN;
  double cathode_high = kNaN;

  [[nodiscard]] std::array<double, 2> centre(double pos) const {
    return {c0[0] + c1[0] * pos, c0[1] + c1[1] * pos};
  }
};

void cylinderSections(const VolumeView& volume, const BatteryOptions& options, CylinderData& data,
                      BatteryResult& result);
/// Overhang samples of the sections across one edge of a cylindrical cell, one per angle (in
/// degrees); keeps the section of the smallest overhang in `worst`.
void edgeOverhang(std::vector<SectionImage>& images, const std::vector<double>& angles, int edge,
                  const BatteryOptions& options, BatteryResult& result, bool& unfinished,
                  std::vector<int>& hidden_edges, std::vector<SectionImage>& worst);
/// Sections across both ends of a cylindrical cell, one per angle, from a quarter of the length
/// inside the cell outwards to the end of the can.
void cylinderOverhang(const VolumeView& volume, const BatteryOptions& options, CylinderData& data,
                      BatteryResult& result, std::vector<SectionImage>& worst);
/// Takes the measured ends of the electrodes along the axis, or well inside the can, clear of
/// the tabs and the cap, where they were not measured.
void cylinderEnds(CylinderData& data, const BatteryResult& result);
/// Where particles and voids are looked for in a cylindrical cell: in the winding, between the
/// electrode ends along the axis.
InRegion cylinderRegion(const CylinderData& data, const VoxelSize& pitch);
/// Radius and angle (cylindrical) or position along the normal (stacked) of the findings.
void placeFindings(BatteryResult& result);

// ---------------------------------------------------------------------------------------------
// Stacked cells

struct StackData {
  int n = 2;  // normal
  int u = 0;  // longer in-plane axis
  int v = 1;
  Overview box;
  double first_layer = kNaN;  // mm along the normal
  double last_layer = kNaN;
  double half_pitch = 0.0;
  std::vector<LayerPoint> points;  // in the plane (u, n) through the middle of v
};

void stackSections(const VolumeView& volume, const BatteryOptions& options, StackData& data,
                   BatteryResult& result);
/// Sections across the four edges of a stack, from the middle of the cell outwards.
void stackOverhang(const VolumeView& volume, const BatteryOptions& options, StackData& data,
                   BatteryResult& result, std::vector<SectionImage>& worst);
/// Outer size of the cell along `axis` on a line through `through` (level-0 voxel), averaged
/// over a small column: between the outer half-height edges of the housing at both ends.
double outerExtentMm(const VolumeView& volume, const Overview& box, int axis, const Index3& through,
                     int w);
/// Outer size of a cylindrical cell across (from `cross`) and along the axis through the
/// winding, between the core and the wall (from `along`).
void cylinderOuterSize(const VolumeView& cross, const VolumeView& along, const CylinderData& data,
                       BatteryResult& result);

// ---------------------------------------------------------------------------------------------
// Images

/// What the analysis keeps for the images.
struct Extras {
  std::vector<LayerPoint> points;
  std::vector<SectionImage> worst;
  int cross_axis = 2;  // the cross-section is normal to this axis
  std::int64_t cross_index = 0;
  int long_axis = 0;  // the longitudinal section is normal to this one
  std::int64_t long_index = 0;
  Overview box;
};

/// Renders the images: the cross-section from `cross`, the longitudinal section from `along`.
void renderImages(const VolumeView& cross_view, const VolumeView& along, const Extras& extras,
                  BatteryResult& result);

// ---------------------------------------------------------------------------------------------
// Cylindrical cells given as section images

BatteryResult analyzeImages(const CellImages& images, const BatteryOptions& options);

}  // namespace voxelsieve::detail::battery
