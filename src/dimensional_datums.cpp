// Alignment of the measured elements on their datums.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "detail/dimensional.hpp"

namespace voxelsieve::detail::dimensional {
namespace {

/// Equations that bring the direction `d` (measured) onto `n` (nominal).
void directionEquations(const Vec& d, const Vec& n, std::vector<Equation>& out) {
  const auto [u, v] = basis(n);
  for (const Vec& b : {u, v}) {
    Equation eq;
    const Vec r = cross(d, b);
    eq.row = {r[0], r[1], r[2], 0.0, 0.0, 0.0};
    eq.rhs = -dot(b, d);
    out.push_back(eq);
  }
}

/// Equation that moves point `p` (measured) along `e` onto the nominal value.
Equation offsetEquation(const Vec& p, const Vec& nominal, const Vec& e, const Vec& origin) {
  Equation eq;
  const Vec r = cross(p - origin, e);
  eq.row = {r[0], r[1], r[2], e[0], e[1], e[2]};
  eq.rhs = -dot(e, p - nominal);
  return eq;
}

}  // namespace

std::vector<Equation> datumEquations(const GeometricElement& nominal,
                                     const GeometricElement& actual, const DatumPlan& datum,
                                     const Vec& origin) {
  std::vector<Equation> eqs;
  switch (nominal.type) {
    case ElementType::kPlane:
      directionEquations(actual.direction, nominal.direction, eqs);
      eqs.push_back(offsetEquation(actual.point, nominal.point, nominal.direction, origin));
      break;
    case ElementType::kCylinder: {
      Vec d = actual.direction;
      if (dot(d, nominal.direction) < 0.0) {
        d = -1.0 * d;
      }
      directionEquations(d, nominal.direction, eqs);
      // The point of the measured axis level with the nominal axis point.
      const Vec p = actual.point + dot(nominal.point - actual.point, d) * d;
      const auto [u, v] = basis(nominal.direction);
      eqs.push_back(offsetEquation(p, nominal.point, u, origin));
      eqs.push_back(offsetEquation(p, nominal.point, v, origin));
      break;
    }
    case ElementType::kSphere:
      for (const Vec& e : {Vec{1.0, 0.0, 0.0}, Vec{0.0, 1.0, 0.0}, Vec{0.0, 0.0, 1.0}}) {
        eqs.push_back(offsetEquation(actual.point, nominal.point, e, origin));
      }
      break;
    case ElementType::kPoint:
      if (datum.directions.empty()) {
        eqs.push_back(offsetEquation(actual.point, nominal.point, nominal.direction, origin));
      }
      for (const Vec& e : datum.directions) {
        eqs.push_back(offsetEquation(actual.point, nominal.point, e, origin));
      }
      break;
  }
  return eqs;
}

/// The small motion that satisfies the equations of the datums in order of precedence: each
/// datum is solved within the freedom the datums before it left; freedom no datum fixes keeps
/// the starting pose. Returns the motion and the number of degrees of freedom fixed.
std::pair<std::array<double, 6>, int> solveDatums(
    const std::vector<std::vector<Equation>>& datums) {
  std::array<double, 6> x{};
  // Columns of the free subspace, 6 x free.
  std::vector<std::array<double, 6>> free;
  for (std::size_t i = 0; i < 6; ++i) {
    std::array<double, 6> column{};
    column[i] = 1.0;
    free.push_back(column);
  }
  for (const auto& eqs : datums) {
    if (free.empty() || eqs.empty()) {
      continue;
    }
    const std::size_t m = free.size();
    // A = J N, b = rhs - J x.
    std::vector<double> ata(m * m, 0.0);
    std::vector<double> atb(m, 0.0);
    double scale = 0.0;
    for (const Equation& eq : eqs) {
      std::vector<double> a(m, 0.0);
      for (std::size_t j = 0; j < m; ++j) {
        for (std::size_t k = 0; k < 6; ++k) {
          a[j] += eq.row[k] * free[j][k];
        }
      }
      double jx = 0.0;
      for (std::size_t k = 0; k < 6; ++k) {
        jx += eq.row[k] * x[k];
        scale += eq.row[k] * eq.row[k];
      }
      const double b = eq.rhs - jx;
      for (std::size_t i = 0; i < m; ++i) {
        for (std::size_t j = 0; j < m; ++j) {
          ata[i * m + j] += a[i] * a[j];
        }
        atb[i] += a[i] * b;
      }
    }
    const auto [values, vectors] = symmetricEigen(ata, m);
    // Constraints the datums before already fixed leave only tiny components; they fix nothing.
    const double threshold = 1e-6 * std::max(scale, 1e-300);
    std::vector<std::array<double, 6>> remaining;
    for (std::size_t j = 0; j < m; ++j) {
      std::array<double, 6> direction{};
      for (std::size_t i = 0; i < m; ++i) {
        for (std::size_t k = 0; k < 6; ++k) {
          direction[k] += vectors[i * m + j] * free[i][k];
        }
      }
      if (values[j] <= threshold) {
        remaining.push_back(direction);
        continue;
      }
      double projection = 0.0;
      for (std::size_t i = 0; i < m; ++i) {
        projection += vectors[i * m + j] * atb[i];
      }
      for (std::size_t k = 0; k < 6; ++k) {
        x[k] += direction[k] * projection / values[j];
      }
    }
    free = std::move(remaining);
  }
  return {x, static_cast<int>(6 - free.size())};
}

RigidTransform smallMotion(const std::array<double, 6>& x, const Vec& origin) {
  const Vec w{x[0], x[1], x[2]};
  const double angle = norm(w);
  RigidTransform rotation;
  if (angle > 0.0) {
    rotation = RigidTransform::fromAxisAngle(w, degrees(angle));
  }
  // p' = R (p - o) + o + t
  rotation.translation = origin - rotation.rotate(origin) + Vec{x[3], x[4], x[5]};
  return rotation;
}

GeometricElement moved(GeometricElement e, const RigidTransform& t) {
  e.point = t.apply(e.point);
  e.direction = t.rotate(e.direction);
  return e;
}

}  // namespace voxelsieve::detail::dimensional
