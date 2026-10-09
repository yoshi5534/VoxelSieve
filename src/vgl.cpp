#include "voxelsieve/vgl.hpp"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <locale>
#include <pugixml.hpp>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

namespace voxelsieve {
namespace {

std::string lower(std::string text) {
  std::ranges::transform(text, text.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

/// The XML of a project. VGStudio writes it gzip-compressed with a few bytes after the gzip
/// stream, which are ignored.
std::string projectXml(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    throw std::runtime_error("Cannot open " + file.string());
  }
  std::string bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  if (bytes.size() < 2 || static_cast<unsigned char>(bytes[0]) != 0x1f ||
      static_cast<unsigned char>(bytes[1]) != 0x8b) {
    return bytes;  // plain XML
  }
  z_stream stream{};
  if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) {
    throw std::runtime_error("Cannot initialise zlib");
  }
  stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(bytes.data()));  // NOLINT
  stream.avail_in = static_cast<uInt>(bytes.size());
  std::string xml;
  std::array<char, 1 << 16> chunk{};
  int status = Z_OK;
  while (status == Z_OK) {
    stream.next_out = reinterpret_cast<Bytef*>(chunk.data());  // NOLINT(*-reinterpret-cast)
    stream.avail_out = static_cast<uInt>(chunk.size());
    status = inflate(&stream, Z_NO_FLUSH);
    xml.append(chunk.data(), chunk.size() - stream.avail_out);
  }
  inflateEnd(&stream);
  if (status != Z_STREAM_END) {
    throw std::runtime_error(file.string() + " is not a readable VGStudio project (" +
                             (stream.msg != nullptr ? stream.msg : "truncated") + ")");
  }
  return xml;
}

/// The objects of a project by id, for links written as <objectref id="..."/>.
using ObjectIndex = std::unordered_map<std::string, pugi::xml_node>;

pugi::xml_node property(pugi::xml_node object, std::string_view name) {
  for (const pugi::xml_node child : object.children("property")) {
    if (name == child.attribute("name").value()) {
      return child;
    }
  }
  return {};
}

std::string text(pugi::xml_node object, std::string_view name) {
  return property(object, name).first_child().text().as_string();
}

std::vector<double> numbers(pugi::xml_node object, std::string_view name) {
  std::istringstream in(property(object, name).first_child().text().as_string());
  in.imbue(std::locale::classic());
  return {std::istream_iterator<double>(in), std::istream_iterator<double>()};
}

pugi::xml_node resolve(pugi::xml_node node, const ObjectIndex& index) {
  if (std::string_view(node.name()) == "objectref") {
    const auto it = index.find(node.attribute("id").value());
    return it == index.end() ? pugi::xml_node() : it->second;
  }
  return std::string_view(node.name()) == "object" ? node : pugi::xml_node();
}

/// The objects of an <objectlink>: the linked object and the one that comes with it (such as the
/// transform of a grid); empty nodes where there are none.
std::array<pugi::xml_node, 2> linked(pugi::xml_node link, const ObjectIndex& index) {
  std::array<pugi::xml_node, 2> objects;
  std::size_t i = 0;
  for (const pugi::xml_node child : link.children()) {
    const std::string_view tag = child.name();
    if ((tag == "object" || tag == "objectref") && i < 2) {
      objects[i++] = resolve(child, index);
    }
  }
  return objects;
}

std::array<pugi::xml_node, 2> linked(pugi::xml_node object, std::string_view name,
                                     const ObjectIndex& index) {
  return linked(property(object, name).child("objectlink"), index);
}

/// VGStudio stores matrices with the translation in the last four values (column-major).
RigidTransform transformOf(pugi::xml_node transform) {
  const std::vector<double> m = numbers(transform, "TransformMatrix");
  if (m.size() != 16) {
    return {};
  }
  std::array<double, 16> row_major{};
  for (std::size_t r = 0; r < 4; ++r) {
    for (std::size_t c = 0; c < 4; ++c) {
      row_major[4 * r + c] = m[4 * c + r];
    }
  }
  return RigidTransform::fromMatrix(row_major);
}

/// Finds a referenced file on this machine (see VglFile::path).
class FileFinder {
 public:
  FileFinder(std::filesystem::path project_dir, std::string saved_project)
      : project_dir_(std::move(project_dir)), saved_dir_(directoryOf(std::move(saved_project))) {}

  [[nodiscard]] VglFile find(const std::string& reference) const {
    VglFile file{.reference = reference, .path = std::nullopt};
    std::string path = reference;
    std::ranges::replace(path, '\\', '/');
    std::error_code error;
    const auto exists = [&error](const std::filesystem::path& p) {
      return std::filesystem::is_regular_file(p, error);
    };
    if (const std::filesystem::path as_is(path); as_is.is_absolute() && exists(as_is)) {
      file.path = as_is;
    } else if (!saved_dir_.empty() && lower(path).starts_with(lower(saved_dir_) + "/") &&
               exists(project_dir_ / path.substr(saved_dir_.size() + 1))) {
      file.path = project_dir_ / path.substr(saved_dir_.size() + 1);
    } else if (const auto slash = path.find_last_of('/');
               exists(project_dir_ / path.substr(slash == std::string::npos ? 0 : slash + 1))) {
      file.path = project_dir_ / path.substr(slash == std::string::npos ? 0 : slash + 1);
    }
    if (file.path) {
      file.path = file.path->lexically_normal();
    }
    return file;
  }

 private:
  static std::string directoryOf(std::string path) {
    std::ranges::replace(path, '\\', '/');
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
  }

  std::filesystem::path project_dir_;
  std::string saved_dir_;
};

std::string describeReference(const std::string& owner_class, const std::string& reference) {
  const std::string extension = lower(std::filesystem::path(reference).extension().string());
  if (owner_class.starts_with("VGLSampleMaskIO") || extension == ".vgm") {
    return "mask or region of interest in VGStudio's own format (.vgm)";
  }
  return "file of " + owner_class;
}

void readVolume(pugi::xml_node render_object, const ObjectIndex& index, const FileFinder& finder,
                VglProject& project, std::set<const void*>& imported_io) {
  VglVolume volume;
  volume.name = text(render_object, "Name");
  const auto [grid, grid_transform] = linked(render_object, "SampleGrid", index);
  if (!grid) {
    return;
  }
  const std::vector<double> size = numbers(grid, "GridSize");
  for (std::size_t axis = 0; axis < 3 && axis < size.size(); ++axis) {
    volume.dims[axis] = static_cast<std::int64_t>(size[axis]);
  }
  if (const std::vector<double> d = numbers(grid, "SamplingDistance"); d.size() == 3) {
    volume.voxel_size = VoxelSize(d[0], d[1], d[2]);
  }
  volume.sample_type = text(grid, "SampleDataType");
  // VGStudio's grid coordinates have voxel corners at integer multiples of the pitch, VoxelSieve's
  // voxel centres: shift by half a voxel.
  RigidTransform half_voxel;
  for (std::size_t axis = 0; axis < 3; ++axis) {
    half_voxel.translation[axis] = 0.5 * volume.voxel_size[axis];
  }
  volume.pose = (grid_transform ? transformOf(grid_transform) : RigidTransform{}).after(half_voxel);

  const pugi::xml_node settings = linked(grid, "ImportSettings", index)[0];
  if (settings) {
    volume.import_class = settings.attribute("class").value();
    for (const pugi::xml_node link : property(settings, "FileIOList").children("objectlink")) {
      const pugi::xml_node io = linked(link, index)[0];
      if (!io) {
        continue;
      }
      imported_io.insert(io.internal_object());
      if (const std::string reference = text(io, "FileName"); !reference.empty()) {
        volume.files.push_back(finder.find(reference));
      }
    }
    if (volume.import_class == "VGLDicomImportSettings") {
      volume.format = "dicom";
      if (const auto swap = numbers(settings, "AxisSwap"); !swap.empty() && swap[0] != 0x010203) {
        volume.notes.emplace_back("axes were swapped on import (AxisSwap " +
                                  std::to_string(static_cast<long>(swap[0])) + ")");
      }
      if (const auto mirror = numbers(settings, "AxisMirror"); !mirror.empty() && mirror[0] != 0) {
        volume.notes.emplace_back("axes were mirrored on import (AxisMirror " +
                                  std::to_string(static_cast<long>(mirror[0])) + ")");
      }
      if (lower(text(settings, "ResamplingMode")) == "true") {
        volume.notes.emplace_back("the volume was resampled on import");
      }
    }
  }
  project.volumes.push_back(std::move(volume));
}

}  // namespace

VglProject readVglProject(const std::filesystem::path& file) {
  const std::string xml = projectXml(file);
  pugi::xml_document document;
  if (const pugi::xml_parse_result parsed = document.load_buffer(xml.data(), xml.size()); !parsed) {
    throw std::runtime_error(file.string() + " is not a VGStudio project: " + parsed.description());
  }
  const pugi::xml_node root = document.child("Project");
  if (!root) {
    throw std::runtime_error(file.string() + " is not a VGStudio project (no <Project>)");
  }
  VglProject project;
  const pugi::xml_node version = root.child("version");
  project.app_name = version.attribute("appname").value();
  project.app_version = version.attribute("appversion").value();
  const FileFinder finder(std::filesystem::absolute(file).parent_path(),
                          version.child("file_location").child("filename").text().as_string());

  ObjectIndex index;
  for (const pugi::xpath_node node : root.select_nodes("//object[@id]")) {
    index.emplace(node.node().attribute("id").value(), node.node());
  }
  std::set<const void*> imported_io;
  for (const pugi::xpath_node node :
       root.select_nodes("//object[@class='VGLVolumeRenderObject']")) {
    readVolume(node.node(), index, finder, project, imported_io);
  }
  for (const pugi::xpath_node node : root.select_nodes("//property[@name='FileName']")) {
    const pugi::xml_node owner = node.node().parent();
    const std::string reference = node.node().first_child().text().as_string();
    if (reference.empty() || imported_io.contains(owner.internal_object())) {
      continue;
    }
    const std::string owner_class = owner.attribute("class").value();
    project.other_files.push_back({.owner_class = owner_class,
                                   .description = describeReference(owner_class, reference),
                                   .file = finder.find(reference)});
  }
  return project;
}

bool isVglFile(const std::filesystem::path& path) {
  return lower(path.extension().string()) == ".vgl";
}

}  // namespace voxelsieve
