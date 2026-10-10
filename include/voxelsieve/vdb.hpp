#pragma once

#include <openvdb/openvdb.h>

#include <cstdint>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "voxelsieve/volume.hpp"

namespace voxelsieve {

/// Grids of unsigned 16 and 8-bit grey values, with the tree configuration of `FloatGrid`
/// (docs/adr/0021-native-integer-grids.md). OpenVDB knows them only after `initializeVdb()`.
using UInt16Tree = openvdb::tree::Tree4<std::uint16_t, 5, 4, 3>::Type;
using UInt16Grid = openvdb::Grid<UInt16Tree>;
using UInt8Tree = openvdb::tree::Tree4<std::uint8_t, 5, 4, 3>::Type;
using UInt8Grid = openvdb::Grid<UInt8Tree>;

/// `openvdb::initialize()` and the registration of `UInt16Grid` and `UInt8Grid`, without which
/// OpenVDB cannot read them. Call before reading or writing VDB files; calling it again is cheap.
void initializeVdb();

/// Type of the grey values of a grid, dataset or `.vdb` file.
enum class ValueType : std::uint8_t { kFloat, kUInt16, kUInt8 };

/// "float", "uint16", "uint8".
[[nodiscard]] std::string_view valueTypeName(ValueType type);
/// The inverse of `valueTypeName`; throws for other names.
[[nodiscard]] ValueType parseValueType(std::string_view name);

/// A grid of grey values of any of the three value types. Code that walks voxels calls `visit`
/// with a generic lambda, which is then compiled once per grid type.
class GreyGrid {
 public:
  using Variant = std::variant<openvdb::FloatGrid::Ptr, UInt16Grid::Ptr, UInt8Grid::Ptr>;

  /// No grid.
  GreyGrid() = default;
  GreyGrid(openvdb::FloatGrid::Ptr grid);  // NOLINT(*-explicit-*): any grid is a grey grid
  GreyGrid(UInt16Grid::Ptr grid);          // NOLINT(*-explicit-*)
  GreyGrid(UInt8Grid::Ptr grid);           // NOLINT(*-explicit-*)

  /// An empty grid of `type` with background 0.
  [[nodiscard]] static GreyGrid create(ValueType type);
  /// The grid as one of the three types; no grid when it is another type.
  [[nodiscard]] static GreyGrid fromBase(const openvdb::GridBase::Ptr& grid);

  explicit operator bool() const { return base_ != nullptr; }
  [[nodiscard]] ValueType valueType() const { return static_cast<ValueType>(grid_.index()); }
  [[nodiscard]] openvdb::GridBase& base() const { return *base_; }
  [[nodiscard]] const openvdb::GridBase::Ptr& basePtr() const { return base_; }

  /// Calls `function(grid)` with the typed grid, const or not as this grey grid.
  template <class Function>
  decltype(auto) visit(Function&& function) const {
    return std::visit(
        [&](const auto& grid) -> decltype(auto) {
          return std::forward<Function>(function)(std::as_const(*grid));
        },
        grid_);
  }
  template <class Function>
  decltype(auto) visit(Function&& function) {
    return std::visit(
        [&](const auto& grid) -> decltype(auto) { return std::forward<Function>(function)(*grid); },
        grid_);
  }

  /// The grey value at `coord` as float, which is exact for every value type; false when the
  /// voxel is inactive.
  bool probeValue(const openvdb::Coord& coord, float& value) const;

 private:
  Variant grid_;
  openvdb::GridBase::Ptr base_;
};

/// The grid with its grey values in `type`, with the same topology, transform, name, class and
/// metadata. Throws when a value is not exact in `type` (a fraction, or out of range): grey
/// values are never quantised silently.
[[nodiscard]] GreyGrid convertGrid(const openvdb::FloatGrid& grid, ValueType type);

/// The grid with float grey values, which are exact for every value type, with the same
/// topology, transform and metadata: for viewers that read only float grids. A float grid is
/// returned as it is, not copied.
[[nodiscard]] openvdb::FloatGrid::Ptr toFloatGrid(const GreyGrid& grid);

/// Copies every voxel into an OpenVDB float grid without removing any air. This is the naive
/// baseline that the sieve is benchmarked against. Float is lossless for 16-bit grey values
/// (see docs/adr/0002-float-voxels.md). Voxel (i, j, k) of the volume maps to VDB index (i, j, k);
/// the grid transform carries the voxel size.
[[nodiscard]] openvdb::FloatGrid::Ptr toDenseFloatGrid(const Volume16& volume);

}  // namespace voxelsieve
