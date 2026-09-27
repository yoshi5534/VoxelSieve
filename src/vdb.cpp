#include "voxelsieve/vdb.hpp"

namespace voxelsieve {

openvdb::FloatGrid::Ptr toDenseFloatGrid(const Volume16& volume) {
  openvdb::initialize();
  auto grid = openvdb::FloatGrid::create(0.0F);
  grid->setTransform(openvdb::math::Transform::createLinearTransform(volume.voxel_size_mm));
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
