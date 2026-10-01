#pragma once

#include <array>
#include <span>

namespace voxelsieve {

/// Rigid transform x' = rotation * x + translation, in mm; rotation row-major. Poses of the objects
/// of a project (ADR 0018) and the alignment of the nominal-actual comparison (ADR 0010) use it;
/// the implementation lives in compare.cpp next to the other 3x3 helpers.
struct RigidTransform {
  std::array<double, 9> rotation{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
  std::array<double, 3> translation{0.0, 0.0, 0.0};

  [[nodiscard]] std::array<double, 3> apply(const std::array<double, 3>& p) const;
  [[nodiscard]] std::array<double, 3> rotate(const std::array<double, 3>& v) const;
  [[nodiscard]] RigidTransform inverse() const;
  /// The transform that applies `first`, then this one.
  [[nodiscard]] RigidTransform after(const RigidTransform& first) const;
  /// Rotation angle in degrees.
  [[nodiscard]] double angleDegrees() const;
  /// Row-major 4x4 matrix.
  [[nodiscard]] std::array<double, 16> matrix() const;

  /// From a row-major 4x4 or 3x4 matrix; throws unless the rotation part is orthonormal with
  /// determinant +1 (to 1e-4).
  [[nodiscard]] static RigidTransform fromMatrix(std::span<const double> values);
  /// Rotation by `degrees` about `axis` through the origin, then the translation.
  [[nodiscard]] static RigidTransform fromAxisAngle(const std::array<double, 3>& axis,
                                                    double degrees,
                                                    const std::array<double, 3>& translation = {});
};

}  // namespace voxelsieve
