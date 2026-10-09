#include <gtest/gtest.h>
#include <zlib.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "dicom_writer.hpp"
#include "voxelsieve/dicom.hpp"
#include "voxelsieve/studio.hpp"
#include "voxelsieve/vgl.hpp"

namespace voxelsieve {
namespace {

using testing::DicomSliceSpec;
using testing::writeDicomSlice;

// A synthetic VGStudio project: the elements and classes of the format as VGStudio writes them,
// with made-up names and paths. The project was saved in D:/archive/part on another machine.
constexpr const char* kSavedDir = "D:/archive/part";
constexpr std::array<std::int64_t, 3> kDims{16, 12, 8};
constexpr std::array<double, 3> kPitch{0.3, 0.3, 0.5};
// The grid lies rotated by 90 degrees about z and shifted; VGStudio writes the matrix
// column by column, the translation last.
constexpr std::array<double, 16> kGridMatrix{0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 5, -7, 2, 1};

std::string property(const std::string& type, const std::string& name, const std::string& body) {
  return R"(<property type=")" + type + R"(" name=")" + name + R"(">)" + body + "</property>\n";
}

std::string link(const std::string& object, const std::string& role,
                 const std::string& second = "<object/>") {
  return "<objectlink>" + object + "<enum>" + role + "</enum>" + second +
         "<enum>Unknown</enum></objectlink>";
}

std::string dicomIo(int id, const std::string& file) {
  return link(R"(<object class="VGLSampleGridImportDicom" id="IO)" + std::to_string(id) + R"(">)" +
                  property("vector4", "HeaderSkip", "<vector4>0 0 0 0</vector4>") +
                  property("string", "FileName", "<string>" + file + "</string>") + "</object>",
              "FileIOList");
}

std::string volume(const std::string& name, const std::string& import_class,
                   const std::vector<std::string>& files, int first_id) {
  std::string io;
  for (std::size_t i = 0; i < files.size(); ++i) {
    io += dicomIo(first_id + static_cast<int>(i), files[i]);
  }
  std::string matrix;
  for (const double value : kGridMatrix) {
    matrix += (matrix.empty() ? "" : " ") + std::to_string(value);
  }
  const std::string settings =
      R"(<object class=")" + import_class + R"(" id="S)" + std::to_string(first_id) + R"(">)" +
      property("index", "AxisMirror", "<index>0</index>") +
      property("index", "AxisSwap", "<index>66051</index>") +
      property("enum", "ResamplingMode", "<enum>False</enum>") +
      R"(<property minIndex="0" maxIndex=")" + std::to_string(files.size() - 1) +
      R"(" type="objectlinkarray" name="FileIOList">)" + io + "</property></object>";
  const std::string grid =
      R"(<object class="VGLSampleGridData" id="G)" + std::to_string(first_id) + R"(">)" +
      property("objectlink", "ImportSettings", link(settings, "ImportSettings")) +
      property("vector4", "GridSize",
               "<vector4>" + std::to_string(kDims[0]) + " " + std::to_string(kDims[1]) + " " +
                   std::to_string(kDims[2]) + " 1</vector4>") +
      property("typeinfo", "SampleDataType", "<typeinfo>Int16</typeinfo>") +
      property("vector3", "SamplingDistance", "<vector3>0.3 0.3 0.5</vector3>") + "</object>";
  const std::string transform =
      R"(<object class="VGLTransform" id="T)" + std::to_string(first_id) + R"(">)" +
      property("matrix4", "TransformMatrix", "<matrix4>" + matrix + "</matrix4>") + "</object>";
  return link(R"(<object class="VGLVolumeRenderObject" id="V)" + std::to_string(first_id) +
                  R"(">)" + property("string", "Name", "<string>" + name + "</string>") +
                  property("objectlink", "SampleGrid", link(grid, "SampleGrid", transform)) +
                  "</object>",
              "VolumeRenderObjectList");
}

std::string projectXml(const std::vector<std::string>& files) {
  const std::string mask =
      R"(<object class="VGLSampleMaskIORLE" id="M1">)" +
      property("string", "FileName",
               std::string("<string>") + kSavedDir + "/part.data/mask.vgm</string>") +
      "</object>";
  return std::string(R"(<?xml version="1.0" encoding="UTF-8"?><!DOCTYPE Project [
<!ELEMENT Project (version, units, vgl)>
<!ATTLIST object id ID #IMPLIED class CDATA #IMPLIED>
]>
<Project>
<version identifier="1.0" appname="VGStudio MAX" appversion="9.9.0">
  <file_location><filename>)") +
         kSavedDir + R"(/part.vgl</filename></file_location>
</version>
<units/>
<vgl><object class="VGLScene" id="SC">
<property minIndex="0" maxIndex="1" type="objectlinkarray" name="VolumeRenderObjectList">)" +
         volume("Test part", "VGLDicomImportSettings", files, 100) +
         volume("Raw part", "SomeOtherImportSettings", {"E:/elsewhere/raw.vol"}, 200) +
         "</property>" + property("objectlink", "SampleMaskIO", link(mask, "SampleMaskIO")) +
         "</object></vgl></Project>\n";
}

/// gzip as VGStudio writes it, with a few bytes after the gzip stream.
std::string gzip(const std::string& text) {
  z_stream stream{};
  EXPECT_EQ(
      deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY),
      Z_OK);
  std::string out(deflateBound(&stream, static_cast<uLong>(text.size())), '\0');
  stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(text.data()));  // NOLINT
  stream.avail_in = static_cast<uInt>(text.size());
  stream.next_out = reinterpret_cast<Bytef*>(out.data());  // NOLINT(*-reinterpret-cast)
  stream.avail_out = static_cast<uInt>(out.size());
  EXPECT_EQ(deflate(&stream, Z_FINISH), Z_STREAM_END);
  out.resize(stream.total_out);
  deflateEnd(&stream);
  return out + std::string(64, '\x5a');
}

std::int32_t storedValue(std::int64_t x, std::int64_t y, std::int64_t z) {
  const bool inside = x >= 4 && x < 12 && y >= 3 && y < 9 && z >= 2 && z < 6;
  return static_cast<std::int32_t>(inside ? 1200 + x + y + z : -1000 + (x + y + z) % 20);
}

class VglTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("voxelsieve_vgl_" +
            std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_ / "part");
    // The slices lie in a sub-folder of the project's folder, as when it was saved; the last
    // one was moved next to the project.
    DicomSliceSpec spec;
    spec.rows = static_cast<std::uint16_t>(kDims[1]);
    spec.columns = static_cast<std::uint16_t>(kDims[0]);
    spec.pixel_spacing = {kPitch[1], kPitch[0]};
    for (std::int64_t z = 0; z < kDims[2]; ++z) {
      spec.position = std::array<double, 3>{0.0, 0.0, kPitch[2] * static_cast<double>(z)};
      std::vector<std::int32_t> values;
      for (std::int64_t y = 0; y < kDims[1]; ++y) {
        for (std::int64_t x = 0; x < kDims[0]; ++x) {
          values.push_back(storedValue(x, y, z));
        }
      }
      const std::string name = "s" + std::to_string(z) + ".dcm";
      const bool last = z + 1 == kDims[2];
      writeDicomSlice(dir_ / "part" / (last ? "" : "slices") / name, spec, values);
      files_.push_back(std::string(kSavedDir) + (last ? "/old/" : "/slices/") + name);
    }
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  std::filesystem::path writeProject(bool compressed = true) {
    const auto path = dir_ / "part" / "part.vgl";
    const std::string xml = projectXml(files_);
    std::ofstream(path, std::ios::binary) << (compressed ? gzip(xml) : xml);
    return path;
  }

  /// The pose VGStudio's grid matrix gives, for voxel centres.
  static RigidTransform expectedPose() {
    RigidTransform pose;
    pose.rotation = {0, -1, 0, 1, 0, 0, 0, 0, 1};
    pose.translation = {5.0 - 0.5 * kPitch[1], -7.0 + 0.5 * kPitch[0], 2.0 + 0.5 * kPitch[2]};
    return pose;
  }

  std::filesystem::path dir_;
  std::vector<std::string> files_;
};

TEST_F(VglTest, ReadsTheVolumesAndFindsTheirFiles) {
  for (const bool compressed : {true, false}) {
    const VglProject project = readVglProject(writeProject(compressed));
    EXPECT_EQ(project.app_name, "VGStudio MAX");
    EXPECT_EQ(project.app_version, "9.9.0");
    ASSERT_EQ(project.volumes.size(), 2U);

    const VglVolume& volume = project.volumes[0];
    EXPECT_EQ(volume.name, "Test part");
    EXPECT_EQ(volume.format, "dicom");
    EXPECT_EQ(volume.import_class, "VGLDicomImportSettings");
    EXPECT_EQ(volume.dims, kDims);
    EXPECT_EQ(volume.voxel_size, VoxelSize(kPitch[0], kPitch[1], kPitch[2]));
    EXPECT_EQ(volume.sample_type, "Int16");
    EXPECT_TRUE(volume.notes.empty());
    const auto pose = volume.pose.matrix();
    const auto expected = expectedPose().matrix();
    for (std::size_t i = 0; i < 16; ++i) {
      EXPECT_NEAR(pose[i], expected[i], 1e-9) << i;
    }
    ASSERT_EQ(volume.files.size(), static_cast<std::size_t>(kDims[2]));
    EXPECT_EQ(volume.files.front().reference, files_.front());
    ASSERT_TRUE(volume.files.front().path);
    EXPECT_EQ(*volume.files.front().path, (dir_ / "part" / "slices" / "s0.dcm").lexically_normal());
    ASSERT_TRUE(volume.files.back().path);  // found by its name next to the project
    EXPECT_EQ(*volume.files.back().path, (dir_ / "part" / "s7.dcm").lexically_normal());

    EXPECT_EQ(project.volumes[1].format, "");
    EXPECT_EQ(project.volumes[1].import_class, "SomeOtherImportSettings");
    ASSERT_EQ(project.volumes[1].files.size(), 1U);
    EXPECT_FALSE(project.volumes[1].files.front().path);

    ASSERT_EQ(project.other_files.size(), 1U);
    EXPECT_EQ(project.other_files.front().owner_class, "VGLSampleMaskIORLE");
    EXPECT_NE(project.other_files.front().description.find(".vgm"), std::string::npos);
    EXPECT_FALSE(project.other_files.front().file.path);
  }
  std::ofstream(dir_ / "broken.vgl", std::ios::binary) << gzip("<Project>").substr(0, 12);
  EXPECT_ANY_THROW((void)readVglProject(dir_ / "broken.vgl"));
  std::ofstream(dir_ / "other.vgl") << "<NotAProject/>";
  EXPECT_ANY_THROW((void)readVglProject(dir_ / "other.vgl"));
}

TEST_F(VglTest, StudioImportsTheVolumeWhereTheProjectPlacesIt) {
  const auto path = writeProject();
  Studio studio({});
  const auto listing = studio.call("browse", {{"path", (dir_ / "part").string()}});
  std::map<std::string, std::string> kinds;
  for (const auto& entry : listing.at("entries")) {
    kinds[entry.at("name")] = entry.at("kind");
  }
  EXPECT_EQ(kinds["part.vgl"], "vgl");
  EXPECT_EQ(kinds["slices"], "dicom");

  studio.call("project_create", {{"path", (dir_ / "project").string()}});
  const auto imported =
      studio.call("run_import_vgl", {{"path", path.string()}, {"brick_size", 16}});
  ASSERT_EQ(imported.at("status"), "done") << imported.dump();
  EXPECT_EQ(imported.at("summary").at("volume"), "Test part");
  EXPECT_EQ(imported.at("summary").at("dims"), nlohmann::json(kDims));
  EXPECT_EQ(imported.at("summary").at("not_imported"), 1);
  const std::string log = imported.at("messages").dump();
  EXPECT_NE(log.find("Not imported"), std::string::npos) << log;
  EXPECT_NE(log.find("Raw part"), std::string::npos) << log;

  const auto objects = studio.call("objects", {}).at("objects");
  ASSERT_EQ(objects.size(), 1U);
  const auto pose = objects.front().at("pose").get<std::vector<double>>();
  const auto expected = expectedPose().matrix();
  for (std::size_t i = 0; i < 16; ++i) {
    EXPECT_NEAR(pose[i], expected[i], 1e-9) << i;
  }

  // The second volume was imported in a way VoxelSieve does not read, and a missing slice stops
  // the import.
  EXPECT_ANY_THROW(studio.call("run_import_vgl", {{"path", path.string()}, {"volume", 1}}));
  std::filesystem::remove(dir_ / "part" / "slices" / "s3.dcm");
  EXPECT_ANY_THROW(studio.call("run_import_vgl", {{"path", path.string()}}));
}

}  // namespace
}  // namespace voxelsieve
