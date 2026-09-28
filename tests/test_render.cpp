#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>

#include "voxelsieve/mesh.hpp"
#include "voxelsieve/render.hpp"
#include "voxelsieve/surface.hpp"

namespace voxelsieve {
namespace {

/// The triangle soup of an STL mesh as an indexed mesh, without sharing vertices.
IndexedMesh indexed(const Mesh& mesh) {
  IndexedMesh out;
  for (const auto& t : mesh.triangles) {
    const auto first = static_cast<std::uint32_t>(out.points.size());
    out.points.insert(out.points.end(), t.begin(), t.end());
    out.triangles.push_back({first, first + 1, first + 2});
  }
  return out;
}

std::array<int, 4> pixel(const RenderImage& image, int x, int y) {
  const auto i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) +
                  static_cast<std::size_t>(x)) *
                 static_cast<std::size_t>(image.channels);
  return {image.pixels[i], image.pixels[i + 1], image.pixels[i + 2],
          image.channels == 4 ? image.pixels[i + 3] : 255};
}

bool reddish(const std::array<int, 4>& p) { return p[0] > p[1] + 40 && p[0] > p[2] + 40; }
bool grey(const std::array<int, 4>& p) {
  return std::abs(p[0] - p[1]) < 20 && std::abs(p[1] - p[2]) < 30;
}

RenderView smallView() {
  RenderView view;
  view.width = 200;
  view.height = 150;
  view.supersampling = 2;
  return view;
}

TEST(RenderTest, SphereFillsTheImageCentreOnTheBackdrop) {
  RenderScene scene;
  scene.spheres.push_back({{0.0, 0.0, 0.0}, 1.0, {220, 40, 40}});
  RenderView view = smallView();
  view.azimuth_degrees = 0.0;
  view.elevation_degrees = 0.0;
  const RenderImage image = render(scene, view);
  ASSERT_EQ(image.width, 200);
  ASSERT_EQ(image.height, 150);
  ASSERT_EQ(image.channels, 3);
  ASSERT_EQ(image.pixels.size(), 200U * 150U * 3U);
  // The sphere is fitted to the height with a margin of 6 %: radius 66 pixels.
  EXPECT_TRUE(reddish(pixel(image, 100, 75)));
  EXPECT_TRUE(reddish(pixel(image, 100 + 60, 75)));
  const auto backdrop = pixel(image, 100 + 72, 75);
  EXPECT_TRUE(grey(backdrop));
  EXPECT_GT(backdrop[0], 200);
  EXPECT_TRUE(grey(pixel(image, 2, 2)));
}

TEST(RenderTest, CameraFollowsAzimuthAndElevation) {
  RenderScene scene;
  scene.spheres.push_back({{0.0, 3.0, 0.0}, 1.0, {220, 40, 40}});   // red at +y
  scene.spheres.push_back({{0.0, -3.0, 0.0}, 1.0, {40, 40, 220}});  // blue at -y
  RenderView view = smallView();
  view.azimuth_degrees = 0.0;  // looking along -x: +y is to the right
  view.elevation_degrees = 0.0;
  RenderImage image = render(scene, view);
  EXPECT_TRUE(reddish(pixel(image, 160, 75)));
  EXPECT_GT(pixel(image, 40, 75)[2], pixel(image, 40, 75)[0] + 40);

  view.elevation_degrees = 90.0;  // from above: +x to the right, +y up
  image = render(scene, view);
  EXPECT_TRUE(reddish(pixel(image, 100, 30)));
  EXPECT_GT(pixel(image, 100, 120)[2], pixel(image, 100, 120)[0] + 40);
}

TEST(RenderTest, GlassShowsWhatIsInsideThePart) {
  const IndexedMesh box = indexed(boxMesh({4.0, 4.0, 4.0}));
  RenderScene scene;
  scene.mesh = &box;
  scene.spheres.push_back({{0.0, 0.0, 0.0}, 1.0, {220, 40, 40}});
  const RenderView view = smallView();
  const auto centre = [&] { return pixel(render(scene, view), 100, 75); };
  EXPECT_TRUE(grey(centre()));  // opaque: the box hides the pore
  scene.surface_opacity = 0.2;
  EXPECT_TRUE(reddish(centre()));
}

TEST(RenderTest, TransparentBackgroundHasAlpha) {
  RenderScene scene;
  scene.spheres.push_back({{0.0, 0.0, 0.0}, 1.0, {220, 40, 40}});
  RenderView view = smallView();
  view.transparent = true;
  const RenderImage image = render(scene, view);
  ASSERT_EQ(image.channels, 4);
  EXPECT_EQ(pixel(image, 2, 2)[3], 0);
  EXPECT_EQ(pixel(image, 100, 75)[3], 255);
  EXPECT_TRUE(reddish(pixel(image, 100, 75)));
}

TEST(RenderTest, WritesPngAndRejectsInvalidSizes) {
  const auto file = std::filesystem::temp_directory_path() / "voxelsieve_render_test.png";
  RenderScene scene;
  scene.spheres.push_back({{0.0, 0.0, 0.0}, 1.0});
  writePng(render(scene, smallView()), file);
  EXPECT_GT(std::filesystem::file_size(file), 100U);
  std::filesystem::remove(file);
  RenderView view;
  view.width = 0;
  EXPECT_THROW((void)render(scene, view), std::invalid_argument);
}

}  // namespace
}  // namespace voxelsieve
