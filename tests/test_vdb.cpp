#include <gtest/gtest.h>

#include "voxelsieve/phantom.hpp"
#include "voxelsieve/vdb.hpp"

namespace voxelsieve {
namespace {

TEST(Vdb, DenseGridKeepsEveryVoxelLossless) {
  PhantomSpec spec = defaultPhantomSpec();
  spec.dims = {40, 40, 40};
  spec.noise_sigma = 300.0;
  const Volume16 volume = generatePhantom(spec);

  const auto grid = toDenseFloatGrid(volume);
  EXPECT_EQ(grid->activeVoxelCount(), volume.voxelCount());
  EXPECT_DOUBLE_EQ(grid->voxelSize()[0], spec.voxel_size[0]);

  const auto accessor = grid->getConstAccessor();
  for (const auto& [x, y, z] : {std::array<int, 3>{0, 0, 0}, {5, 17, 33}, {39, 39, 39}}) {
    EXPECT_EQ(accessor.getValue(openvdb::Coord(x, y, z)), static_cast<float>(volume.at(x, y, z)));
  }
}

}  // namespace
}  // namespace voxelsieve
