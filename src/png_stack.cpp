// PNG image stacks (ADR 0044), decoded with libpng slice by slice.

#include "voxelsieve/png_stack.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <list>
#include <map>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "detail/png.hpp"
#include "detail/zip.hpp"
#include "voxelsieve/tiff.hpp"

namespace voxelsieve {
namespace {

std::string lower(std::string text) {
  std::ranges::transform(text, text.begin(), [](unsigned char c) {
    return static_cast<char>(c < 0x80 ? std::tolower(c) : c);
  });
  return text;
}

bool isPngName(const std::string& name) {
  const std::string base = std::filesystem::path(name).filename().string();
  return lower(base).ends_with(".png") && !base.starts_with("._");
}

std::string parentOf(const std::string& name) {
  const auto slash = name.find_last_of('/');
  return slash == std::string::npos ? std::string() : name.substr(0, slash);
}

std::vector<std::uint8_t> readFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("Cannot read " + path.string());
  }
  in.seekg(0, std::ios::end);
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(in.tellg()));
  in.seekg(0);
  in.read(
      reinterpret_cast<char*>(bytes.data()),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
      static_cast<std::streamsize>(bytes.size()));
  if (!in) {
    throw std::runtime_error("Cannot read " + path.string());
  }
  return bytes;
}

}  // namespace

struct PngStackSource::Impl {
  std::unique_ptr<detail::ZipArchive> zip;
  std::vector<std::filesystem::path> files;               // directory
  std::vector<const detail::ZipArchive::Entry*> entries;  // archive
  std::string folder;
  std::vector<std::string> other_folders;
  std::array<std::int64_t, 3> dims{};
  VoxelSize voxel_size{1.0};
  bool voxel_size_known = false;
  int bit_depth = 8;
  std::string identity;
  std::size_t cache_bytes = 0;

  using Slice = std::shared_ptr<const std::vector<std::uint16_t>>;
  mutable std::mutex mutex;
  mutable std::list<std::pair<std::size_t, Slice>> lru;  // most recent first
  mutable std::map<std::size_t, std::list<std::pair<std::size_t, Slice>>::iterator> cached;

  [[nodiscard]] std::size_t sliceCount() const { return zip ? entries.size() : files.size(); }

  [[nodiscard]] std::string nameOf(std::size_t z) const {
    return zip ? entries[z]->name : files[z].string();
  }

  [[nodiscard]] detail::GreyImage decode(std::size_t z) const {
    const std::vector<std::uint8_t> bytes = zip ? zip->read(*entries[z]) : readFile(files[z]);
    try {
      return detail::decodeGreyPng(bytes);
    } catch (const std::exception& error) {
      throw std::runtime_error(nameOf(z) + ": " + error.what());
    }
  }

  Slice slice(std::size_t z) const {
    {
      const std::scoped_lock lock(mutex);
      if (const auto it = cached.find(z); it != cached.end()) {
        lru.splice(lru.begin(), lru, it->second);
        return it->second->second;
      }
    }
    detail::GreyImage image = decode(z);
    if (static_cast<std::int64_t>(image.width) != dims[0] ||
        static_cast<std::int64_t>(image.height) != dims[1]) {
      throw std::runtime_error("Slice " + std::to_string(z) + " (" + nameOf(z) + ") has " +
                               std::to_string(image.width) + "x" + std::to_string(image.height) +
                               " pixels, slice 0 " + std::to_string(dims[0]) + "x" +
                               std::to_string(dims[1]));
    }
    auto decoded = std::make_shared<const std::vector<std::uint16_t>>(std::move(image.values));
    const std::scoped_lock lock(mutex);
    if (const auto it = cached.find(z); it != cached.end()) {
      return it->second->second;
    }
    lru.emplace_front(z, decoded);
    cached[z] = lru.begin();
    const std::size_t slice_bytes = decoded->size() * sizeof(std::uint16_t);
    while (lru.size() > 1 && lru.size() * slice_bytes > cache_bytes) {
      cached.erase(lru.back().first);
      lru.pop_back();
    }
    return decoded;
  }
};

PngStackSource::PngStackSource(const std::filesystem::path& path, const PngStackOptions& options)
    : impl_(std::make_unique<Impl>()) {
  Impl& impl = *impl_;
  impl.cache_bytes = options.cache_bytes;
  if (!std::filesystem::exists(path)) {
    throw std::runtime_error("No such file or directory: " + path.string());
  }
  impl.identity = inputIdentity(path);
  std::map<std::string, std::vector<std::string>> folders;
  std::map<std::string, const detail::ZipArchive::Entry*> by_name;
  const bool is_zip = !std::filesystem::is_directory(path);
  if (is_zip) {
    impl.zip = std::make_unique<detail::ZipArchive>(path);
    for (const auto& entry : impl.zip->entries()) {
      if (entry.name.ends_with('/') || !isPngName(entry.name) ||
          entry.name.starts_with("__MACOSX/")) {
        continue;
      }
      folders[parentOf(entry.name)].push_back(entry.name);
      by_name[entry.name] = &entry;
    }
  } else {
    for (auto it = std::filesystem::recursive_directory_iterator(path);
         it != std::filesystem::recursive_directory_iterator(); ++it) {
      if (it.depth() > 3) {
        it.disable_recursion_pending();
      }
      if (it->is_regular_file() && isPngName(it->path().filename().string())) {
        const std::string relative = std::filesystem::relative(it->path(), path).generic_string();
        folders[parentOf(relative)].push_back(relative);
      }
    }
  }
  if (folders.empty()) {
    throw std::runtime_error("No PNG files in " + path.string());
  }
  std::string chosen;
  if (!options.folder.empty()) {
    std::string wanted = options.folder;
    while (wanted.ends_with('/')) {
      wanted.pop_back();
    }
    std::vector<std::string> matches;
    std::string known;
    for (const auto& [name, files] : folders) {
      if (name == wanted || std::filesystem::path(name).filename().string() == wanted) {
        matches.push_back(name);
      }
      known += (known.empty() ? "" : ", ") + (name.empty() ? std::string("(top level)") : name);
    }
    if (matches.size() != 1) {
      throw std::invalid_argument("Folder '" + options.folder + "' " +
                                  (matches.empty() ? "not found" : "is ambiguous") +
                                  "; folders with PNG files: " + known);
    }
    chosen = matches.front();
  } else {
    chosen = folders.begin()->first;
    for (const auto& [name, files] : folders) {
      const bool la = looksLikeLabelFolder(name);
      const bool lb = looksLikeLabelFolder(chosen);
      const std::size_t na = files.size();
      const std::size_t nb = folders.at(chosen).size();
      if (la != lb ? !la : na != nb ? na > nb : name < chosen) {
        chosen = name;
      }
    }
  }
  impl.folder = chosen;
  for (const auto& [name, files] : folders) {
    if (name != chosen) {
      impl.other_folders.push_back(name);
    }
  }
  std::vector<std::string> names = folders.at(chosen);
  std::ranges::sort(names, naturalLess);
  for (const std::string& name : names) {
    if (is_zip) {
      impl.entries.push_back(by_name.at(name));
    } else {
      impl.files.push_back(path / name);
    }
  }
  const detail::GreyImage first = impl.decode(0);
  impl.dims = {first.width, first.height, static_cast<std::int64_t>(impl.sliceCount())};
  checkVolumeDims(impl.dims, path.filename().string());
  impl.bit_depth = first.bit_depth;
  if (options.voxel_size) {
    impl.voxel_size = *options.voxel_size;
    impl.voxel_size_known = true;
  } else {
    // The name of the folder of the slices, else of the directory or archive.
    const std::filesystem::path named =
        chosen.empty() ? path / "slice.png" : path / chosen / "slice.png";
    std::optional<VoxelSize> size = voxelSizeFromName(named);
    if (!size && !chosen.empty()) {
      size = voxelSizeFromName(path / "slice.png");
    }
    if (!size) {
      size = voxelSizeFromName(path);
    }
    if (size) {
      impl.voxel_size = *size;
      impl.voxel_size_known = true;
    }
  }
  impl.voxel_size.validate();
}

PngStackSource::~PngStackSource() = default;

std::array<std::int64_t, 3> PngStackSource::dims() const { return impl_->dims; }
VoxelSize PngStackSource::voxelSize() const { return impl_->voxel_size; }
SampleType PngStackSource::sampleType() const {
  return impl_->bit_depth == 8 ? SampleType::kUInt8 : SampleType::kUInt16;
}
bool PngStackSource::voxelSizeKnown() const { return impl_->voxel_size_known; }
int PngStackSource::bitDepth() const { return impl_->bit_depth; }
const std::string& PngStackSource::folder() const { return impl_->folder; }
const std::vector<std::string>& PngStackSource::otherFolders() const {
  return impl_->other_folders;
}
std::string PngStackSource::identity() const { return impl_->identity; }

void PngStackSource::releaseMemory() const {
  {
    const std::scoped_lock lock(impl_->mutex);
    impl_->lru.clear();
    impl_->cached.clear();
  }
  if (impl_->zip) {
    impl_->zip->releasePages();
  }
}

void PngStackSource::readRegion(const Box& box, std::span<std::uint16_t> out) const {
  const auto& dims = impl_->dims;
  for (std::size_t axis = 0; axis < 3; ++axis) {
    if (box.min[axis] < 0 || box.max[axis] > dims[axis] || box.min[axis] > box.max[axis]) {
      throw std::out_of_range("Region outside the volume");
    }
  }
  if (static_cast<std::int64_t>(out.size()) != box.voxelCount()) {
    throw std::invalid_argument("Output buffer size does not match the region");
  }
  const auto row = static_cast<std::size_t>(box.size(0));
  std::size_t offset = 0;
  for (std::int64_t z = box.min[2]; z < box.max[2]; ++z) {
    const auto slice = impl_->slice(static_cast<std::size_t>(z));
    for (std::int64_t y = box.min[1]; y < box.max[1]; ++y) {
      const auto source = static_cast<std::size_t>(y * dims[0] + box.min[0]);
      std::memcpy(&out[offset], slice->data() + source, row * sizeof(std::uint16_t));
      offset += row;
    }
  }
}

bool containsPngSlices(const std::filesystem::path& path) {
  std::error_code error;
  if (!std::filesystem::is_directory(path, error)) {
    if (lower(path.extension().string()) != ".zip") {
      return false;
    }
    try {
      const detail::ZipArchive archive(path, false);
      std::size_t png = 0;
      std::size_t tiff = 0;
      for (const auto& entry : archive.entries()) {
        if (isPngName(entry.name)) {
          ++png;
        } else if (lower(entry.name).ends_with(".tif") || lower(entry.name).ends_with(".tiff")) {
          ++tiff;
        }
      }
      return png >= 2 && png > tiff;
    } catch (const std::exception&) {
      return false;
    }
  }
  constexpr int kLookedAt = 64;
  int looked_at = 0;
  int found = 0;
  for (auto it = std::filesystem::directory_iterator(
           path, std::filesystem::directory_options::skip_permission_denied, error);
       !error && it != std::filesystem::directory_iterator() && looked_at < kLookedAt;
       it.increment(error), ++looked_at) {
    std::error_code ignored;
    if (it->is_directory(ignored)) {
      for (auto inner = std::filesystem::directory_iterator(
               it->path(), std::filesystem::directory_options::skip_permission_denied, ignored);
           !ignored && inner != std::filesystem::directory_iterator() && looked_at < kLookedAt;
           inner.increment(ignored), ++looked_at) {
        found += isPngName(inner->path().filename().string()) ? 1 : 0;
      }
    } else if (isPngName(it->path().filename().string())) {
      ++found;
    }
    if (found >= 2) {
      return true;
    }
  }
  return false;
}

}  // namespace voxelsieve
