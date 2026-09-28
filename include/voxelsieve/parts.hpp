#pragma once

#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "voxelsieve/mesh.hpp"

namespace voxelsieve {

/// Sample parts that resemble real castings: thin walls, ribs, bosses, bores and fillets. They are
/// defined as signed distance functions and meshed, so their surface is known analytically.
struct SamplePartInfo {
  std::string name;
  std::string description;
  /// Bounding box of the part at scale 1 in mm.
  Bounds bounds;
};

/// The available parts: "housing" (open gearbox housing with flange, ribs, a boss with a cross
/// bore and inner domes with blind holes), "bracket" (angle bracket with gussets, slots and
/// holes) and "hub" (wheel hub with spokes, rim, bore and bolt circle).
[[nodiscard]] const std::vector<SamplePartInfo>& samplePartInfos();

struct SamplePartOptions {
  /// Uniform scale; the parts are 30 to 50 mm long at scale 1.
  double scale = 1.0;
  /// Edge length of the meshing grid in mm; 0 picks 1/200 of the longest extent.
  double resolution_mm = 0.0;
};

/// Signed distance of `point_mm` to the part in mm, negative inside. Exact near flat faces and
/// cylinders; fillets make it an approximation that keeps the sign and the zero set.
[[nodiscard]] double samplePartDistance(std::string_view name,
                                        const std::array<double, 3>& point_mm, double scale = 1.0);

/// Closed, outward-oriented triangle mesh of a sample part. Throws std::invalid_argument for an
/// unknown name.
[[nodiscard]] Mesh samplePartMesh(std::string_view name, const SamplePartOptions& options = {});

}  // namespace voxelsieve
