#include "voxelsieve/vdb.hpp"

#include <cmath>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>

#include "detail/transform.hpp"

namespace voxelsieve {
namespace {

/// Grid of `Target` with the topology, transform and metadata of `source` and the values
/// `convert(value, coord)`.
template <class Target, class Source, class Convert>
typename Target::Ptr convertValues(const Source& source, Convert convert) {
  using Value = typename Target::ValueType;
  auto target = Target::create(std::make_shared<typename Target::TreeType>(
      source.tree(), Value{0}, openvdb::TopologyCopy()));
  // Voxels leaf by leaf, as both trees have the same leaves; then the active tiles above them.
  for (auto leaf = source.tree().cbeginLeaf(); leaf; ++leaf) {
    auto* target_leaf = target->tree().probeLeaf(leaf->origin());
    for (auto it = leaf->cbeginValueOn(); it; ++it) {
      target_leaf->setValueOnly(it.pos(), convert(*it, it.getCoord()));
    }
  }
  auto tile = source.tree().cbeginValueOn();
  tile.setMaxDepth(Source::TreeType::ValueOnCIter::LEAF_DEPTH - 1);
  for (; tile; ++tile) {
    openvdb::CoordBBox box;
    tile.getBoundingBox(box);
    target->tree().fill(box, convert(*tile, tile.getCoord()), /*active=*/true);
  }
  target->setTransform(source.transform().copy());
  for (auto meta = source.beginMeta(); meta != source.endMeta(); ++meta) {
    target->insertMeta(meta->first, *meta->second);  // name and class are metadata, too
  }
  return target;
}

template <class Grid>
GreyGrid convertTo(const openvdb::FloatGrid& grid, ValueType type) {
  using Value = typename Grid::ValueType;
  constexpr auto kMax = static_cast<float>(std::numeric_limits<Value>::max());
  return convertValues<Grid>(grid, [&](float value, const openvdb::Coord& c) {
    if (!(value >= 0.0F && value <= kMax) || value != std::round(value)) {
      throw std::runtime_error("Grey value " + std::to_string(value) + " at (" +
                               std::to_string(c.x()) + ", " + std::to_string(c.y()) + ", " +
                               std::to_string(c.z()) + ") is not exact as " +
                               std::string(valueTypeName(type)));
    }
    return static_cast<Value>(value);
  });
}

}  // namespace

void initializeVdb() {
  static std::once_flag once;
  std::call_once(once, [] {
    openvdb::initialize();
    if (!UInt16Grid::isRegistered()) {
      UInt16Grid::registerGrid();
    }
    if (!UInt8Grid::isRegistered()) {
      UInt8Grid::registerGrid();
    }
  });
}

std::string_view valueTypeName(ValueType type) {
  switch (type) {
    case ValueType::kFloat:
      return "float";
    case ValueType::kUInt16:
      return "uint16";
    case ValueType::kUInt8:
      return "uint8";
  }
  return "float";
}

ValueType parseValueType(std::string_view name) {
  for (const ValueType type : {ValueType::kFloat, ValueType::kUInt16, ValueType::kUInt8}) {
    if (name == valueTypeName(type)) {
      return type;
    }
  }
  throw std::invalid_argument("Unknown value type '" + std::string(name) +
                              "' (float, uint16 or uint8)");
}

GreyGrid::GreyGrid(openvdb::FloatGrid::Ptr grid) : grid_(grid), base_(std::move(grid)) {}
GreyGrid::GreyGrid(UInt16Grid::Ptr grid) : grid_(grid), base_(std::move(grid)) {}
GreyGrid::GreyGrid(UInt8Grid::Ptr grid) : grid_(grid), base_(std::move(grid)) {}

GreyGrid GreyGrid::create(ValueType type) {
  switch (type) {
    case ValueType::kUInt16:
      return UInt16Grid::create(0);
    case ValueType::kUInt8:
      return UInt8Grid::create(0);
    case ValueType::kFloat:
      break;
  }
  return openvdb::FloatGrid::create(0.0F);
}

GreyGrid GreyGrid::fromBase(const openvdb::GridBase::Ptr& grid) {
  if (auto typed = openvdb::gridPtrCast<openvdb::FloatGrid>(grid)) {
    return typed;
  }
  if (auto typed = openvdb::gridPtrCast<UInt16Grid>(grid)) {
    return typed;
  }
  if (auto typed = openvdb::gridPtrCast<UInt8Grid>(grid)) {
    return typed;
  }
  return {};
}

bool GreyGrid::probeValue(const openvdb::Coord& coord, float& value) const {
  return visit([&](const auto& grid) {
    typename std::decay_t<decltype(grid)>::ValueType typed{};
    const bool on = grid.tree().probeValue(coord, typed);
    value = static_cast<float>(typed);
    return on;
  });
}

GreyGrid convertGrid(const openvdb::FloatGrid& grid, ValueType type) {
  switch (type) {
    case ValueType::kUInt16:
      return convertTo<UInt16Grid>(grid, type);
    case ValueType::kUInt8:
      return convertTo<UInt8Grid>(grid, type);
    case ValueType::kFloat:
      break;
  }
  return grid.deepCopy();
}

openvdb::FloatGrid::Ptr toFloatGrid(const GreyGrid& grid) {
  if (auto float_grid = openvdb::gridPtrCast<openvdb::FloatGrid>(grid.basePtr())) {
    return float_grid;
  }
  return grid.visit([](const auto& typed) {
    return convertValues<openvdb::FloatGrid>(
        typed, [](auto value, const openvdb::Coord&) { return static_cast<float>(value); });
  });
}

openvdb::FloatGrid::Ptr toDenseFloatGrid(const Volume16& volume) {
  initializeVdb();
  auto grid = openvdb::FloatGrid::create(0.0F);
  grid->setTransform(detail::voxelTransform(volume.voxel_size));
  grid->setGridClass(openvdb::GRID_FOG_VOLUME);
  grid->setName("density");

  auto accessor = grid->getAccessor();
  for (std::int64_t z = 0; z < volume.dims[2]; ++z) {
    for (std::int64_t y = 0; y < volume.dims[1]; ++y) {
      for (std::int64_t x = 0; x < volume.dims[0]; ++x) {
        const openvdb::Coord coord(static_cast<openvdb::Int32>(x), static_cast<openvdb::Int32>(y),
                                   static_cast<openvdb::Int32>(z));
        accessor.setValue(coord, static_cast<float>(volume.at(x, y, z)));
      }
    }
  }
  return grid;
}

}  // namespace voxelsieve
