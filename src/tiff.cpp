#include "voxelsieve/tiff.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <utility>

#include "detail/tiff.hpp"
#include "detail/zip.hpp"

namespace voxelsieve {
namespace {

std::string lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

bool hasExtension(const std::string& name, std::initializer_list<const char*> extensions) {
  const std::string l = lower(name);
  return std::any_of(extensions.begin(), extensions.end(), [&l](const char* e) {
    return l.size() > std::string_view(e).size() && l.ends_with(e);
  });
}

bool isTiffName(const std::string& name) { return hasExtension(name, {".tif", ".tiff"}); }

/// Whether a folder name looks like a label or mask volume rather than grey values.
bool looksLikeLabels(const std::string& folder) {
  const std::string name = lower(std::filesystem::path(folder).filename().string());
  std::string token;
  const auto check = [](const std::string& t) {
    return t == "gt" || t == "target" || t == "targets" || t.starts_with("label") ||
           t.starts_with("mask") || t.starts_with("seg") || t == "annotation" || t == "annotations";
  };
  for (const char c : name + " ") {
    if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
      token += c;
    } else {
      if (check(token)) {
        return true;
      }
      token.clear();
    }
  }
  return false;
}

std::string parentOf(const std::string& name) {
  const auto slash = name.find_last_of('/');
  return slash == std::string::npos ? std::string() : name.substr(0, slash);
}

}  // namespace

bool naturalLess(const std::string& a, const std::string& b) {
  std::size_t i = 0;
  std::size_t j = 0;
  while (i < a.size() && j < b.size()) {
    const bool da = std::isdigit(static_cast<unsigned char>(a[i])) != 0;
    const bool db = std::isdigit(static_cast<unsigned char>(b[j])) != 0;
    if (da && db) {
      std::size_t ei = i;
      std::size_t ej = j;
      while (ei < a.size() && std::isdigit(static_cast<unsigned char>(a[ei])) != 0) {
        ++ei;
      }
      while (ej < b.size() && std::isdigit(static_cast<unsigned char>(b[ej])) != 0) {
        ++ej;
      }
      // Compare by value: without leading zeros, longer is larger.
      std::string_view na(a.data() + i, ei - i);
      std::string_view nb(b.data() + j, ej - j);
      na.remove_prefix(std::min(na.find_first_not_of('0'), na.size()));
      nb.remove_prefix(std::min(nb.find_first_not_of('0'), nb.size()));
      if (na.size() != nb.size()) {
        return na.size() < nb.size();
      }
      if (na != nb) {
        return na < nb;
      }
      i = ei;
      j = ej;
    } else {
      if (a[i] != b[j]) {
        return a[i] < b[j];
      }
      ++i;
      ++j;
    }
  }
  return a.size() - i < b.size() - j;
}

bool isTiffStackPath(const std::filesystem::path& path) {
  return std::filesystem::is_directory(path) ||
         hasExtension(path.filename().string(), {".tif", ".tiff", ".zip"});
}

struct TiffStackSource::Impl {
  struct Slice {
    std::filesystem::path file;                        // file on disk, or
    const detail::ZipArchive::Entry* entry = nullptr;  // entry of the archive
    std::size_t page = 0;                              // image in a multi-page file
  };

  std::unique_ptr<detail::ZipArchive> zip;
  std::unique_ptr<detail::FileBytes> multi_page;  // one file holding all slices
  std::vector<detail::TiffPage> multi_pages;
  std::vector<Slice> slices;
  std::string folder;
  std::vector<std::string> other_folders;
  detail::TiffPage first;
  std::array<std::int64_t, 3> dims{};
  std::optional<VoxelSize> voxel_size;
  std::size_t cache_bytes = 0;
  bool is_float = false;
  std::array<double, 2> value_range{0.0, 65535.0};
  ValueMapping mapping;

  using Chunk = std::shared_ptr<const std::vector<std::uint16_t>>;
  using Key = std::pair<std::size_t, std::size_t>;  // slice, chunk
  mutable std::mutex mutex;
  mutable std::vector<std::shared_ptr<const detail::TiffPage>> pages;  // null until read
  mutable std::list<std::pair<Key, Chunk>> lru;                        // most recent first
  mutable std::map<Key, std::list<std::pair<Key, Chunk>>::iterator> cached;
  mutable std::size_t cached_bytes = 0;
  mutable std::set<Key> clip_counted;  // chunks whose clipped values are counted
  mutable std::uint64_t clipped = 0;

  void check(const detail::TiffPage& page, std::size_t z) const {
    detail::checkSupported(page);
    if (page.width != first.width || page.height != first.height || page.bits != first.bits) {
      throw std::runtime_error("Slice " + std::to_string(z) + " has " + std::to_string(page.width) +
                               "x" + std::to_string(page.height) + " pixels of " +
                               std::to_string(page.bits) + " bit, slice 0 " +
                               std::to_string(first.width) + "x" + std::to_string(first.height) +
                               " of " + std::to_string(first.bits) + " bit");
    }
  }

  void insert(const Key& key, Chunk chunk) const {
    if (cached.contains(key)) {
      return;
    }
    cached_bytes += chunk->size() * sizeof(std::uint16_t);
    lru.emplace_front(key, std::move(chunk));
    cached[key] = lru.begin();
    while (cached_bytes > cache_bytes && lru.size() > 1) {
      cached_bytes -= lru.back().second->size() * sizeof(std::uint16_t);
      cached.erase(lru.back().first);
      lru.pop_back();
    }
  }

  /// Strip or tile `index` of slice `z` as 16-bit grey values; float values are mapped.
  std::vector<std::uint16_t> decode(const detail::ByteSource& bytes, const detail::TiffPage& page,
                                    std::size_t z, std::size_t index) const {
    if (!is_float) {
      return detail::decodeTiffChunk(bytes, page, index);
    }
    const std::vector<double> values = detail::decodeTiffFloatChunk(bytes, page, index);
    std::vector<std::uint16_t> grey(values.size());
    // Rows and columns of the chunk that lie in the image; tile padding is not counted.
    const std::size_t x0 = (index % page.chunks_across) * page.chunk_width;
    const std::size_t y0 = (index / page.chunks_across) * page.chunk_height;
    std::uint64_t outside = 0;
    for (std::size_t y = 0; y < page.chunk_height; ++y) {
      for (std::size_t x = 0; x < page.chunk_width; ++x) {
        const std::size_t i = y * page.chunk_width + x;
        const double g = std::round(mapping.toGrey(values[i]));
        if (!(g >= 0.0 && g <= 65535.0) && x0 + x < page.width && y0 + y < page.height) {
          ++outside;  // also not a number
        }
        grey[i] = std::isnan(g) ? 0 : static_cast<std::uint16_t>(std::clamp(g, 0.0, 65535.0));
      }
    }
    const std::lock_guard lock(mutex);
    if (clip_counted.insert({z, index}).second) {
      clipped += outside;
    }
    return grey;
  }

  /// Smallest and largest finite float value of slice `z`, read without the cache.
  std::array<double, 2> floatRange(std::size_t z) const {
    const Slice& slice = slices[z];
    std::unique_ptr<detail::ByteSource> owned;
    const detail::ByteSource* bytes = multi_page.get();
    detail::TiffPage page;
    if (multi_page) {
      page = multi_pages[slice.page];
    } else {
      if (slice.entry != nullptr) {
        owned = std::make_unique<detail::MemoryBytes>(zip->read(*slice.entry));
      } else {
        owned = std::make_unique<detail::FileBytes>(slice.file);
      }
      bytes = owned.get();
      page = detail::readTiffPages(*bytes, 1).front();
    }
    check(page, z);
    std::array<double, 2> range{std::numeric_limits<double>::infinity(),
                                -std::numeric_limits<double>::infinity()};
    for (std::size_t index = 0; index < page.offsets.size(); ++index) {
      const std::vector<double> values = detail::decodeTiffFloatChunk(*bytes, page, index);
      const std::size_t x0 = (index % page.chunks_across) * page.chunk_width;
      const std::size_t y0 = (index / page.chunks_across) * page.chunk_height;
      for (std::size_t y = 0; y < page.chunk_height && y0 + y < page.height; ++y) {
        for (std::size_t x = 0; x < page.chunk_width && x0 + x < page.width; ++x) {
          const double v = values[y * page.chunk_width + x];
          if (std::isfinite(v)) {
            range[0] = std::min(range[0], v);
            range[1] = std::max(range[1], v);
          }
        }
      }
    }
    return range;
  }

  /// Image description of slice `z`, read on first use.
  detail::TiffPage pageOf(std::size_t z) const {
    {
      const std::lock_guard lock(mutex);
      if (pages[z] != nullptr) {
        return *pages[z];
      }
    }
    const Slice& slice = slices[z];
    detail::TiffPage page;
    if (multi_page) {
      page = multi_pages[slice.page];
    } else if (slice.entry != nullptr) {
      (void)chunk(z, 0);  // inflates the entry and records its page
      const std::lock_guard lock(mutex);
      return *pages[z];
    } else {
      page = detail::readTiffPages(detail::FileBytes(slice.file), 1).front();
    }
    check(page, z);
    const std::lock_guard lock(mutex);
    pages[z] = std::make_shared<const detail::TiffPage>(page);
    return page;
  }

  /// Decoded strip or tile `index` of slice `z`.
  Chunk chunk(std::size_t z, std::size_t index) const {
    {
      const std::lock_guard lock(mutex);
      if (const auto found = cached.find({z, index}); found != cached.end()) {
        lru.splice(lru.begin(), lru, found->second);
        return found->second->second;
      }
    }
    const Slice& slice = slices[z];
    if (multi_page) {
      const detail::TiffPage& page = multi_pages[slice.page];
      check(page, z);
      auto decoded =
          std::make_shared<const std::vector<std::uint16_t>>(decode(*multi_page, page, z, index));
      const std::lock_guard lock(mutex);
      pages[z] = std::make_shared<const detail::TiffPage>(page);
      insert({z, index}, decoded);
      return decoded;
    }
    std::unique_ptr<detail::ByteSource> bytes;
    if (slice.entry != nullptr) {
      bytes = std::make_unique<detail::MemoryBytes>(zip->read(*slice.entry));
    } else {
      bytes = std::make_unique<detail::FileBytes>(slice.file);
    }
    const detail::TiffPage page = detail::readTiffPages(*bytes, 1).front();
    check(page, z);
    // An archive entry is inflated as a whole, so decode all of its chunks at once.
    std::vector<std::size_t> wanted{index};
    if (slice.entry != nullptr) {
      wanted.clear();
      for (std::size_t i = 0; i < page.offsets.size(); ++i) {
        wanted.push_back(i);
      }
    }
    Chunk result;
    std::vector<std::pair<std::size_t, Chunk>> decoded;
    for (const std::size_t i : wanted) {
      decoded.emplace_back(
          i, std::make_shared<const std::vector<std::uint16_t>>(decode(*bytes, page, z, i)));
      if (i == index) {
        result = decoded.back().second;
      }
    }
    const std::lock_guard lock(mutex);
    pages[z] = std::make_shared<const detail::TiffPage>(page);
    for (auto& [i, data] : decoded) {
      insert({z, i}, std::move(data));
    }
    return result;
  }
};

TiffStackSource::TiffStackSource(const std::filesystem::path& path, const TiffStackOptions& options)
    : impl_(std::make_unique<Impl>()) {
  Impl& impl = *impl_;
  impl.voxel_size = options.voxel_size;
  impl.cache_bytes = options.cache_bytes;
  if (impl.voxel_size) {
    impl.voxel_size->validate();
  }
  if (!std::filesystem::exists(path)) {
    throw std::runtime_error("No such file or directory: " + path.string());
  }

  // Candidate slices by folder: relative folder -> names.
  std::map<std::string, std::vector<std::string>> folders;
  std::map<std::string, const detail::ZipArchive::Entry*> entries;
  const bool is_zip =
      !std::filesystem::is_directory(path) && hasExtension(path.filename().string(), {".zip"});
  if (is_zip) {
    impl.zip = std::make_unique<detail::ZipArchive>(path);
    for (const auto& entry : impl.zip->entries()) {
      const std::string base = std::filesystem::path(entry.name).filename().string();
      if (entry.name.ends_with('/') || !isTiffName(entry.name) ||
          entry.name.starts_with("__MACOSX/") || base.starts_with("._")) {
        continue;
      }
      folders[parentOf(entry.name)].push_back(entry.name);
      entries[entry.name] = &entry;
    }
  } else if (std::filesystem::is_directory(path)) {
    for (auto it = std::filesystem::recursive_directory_iterator(path);
         it != std::filesystem::recursive_directory_iterator(); ++it) {
      if (it.depth() > 3) {
        it.disable_recursion_pending();
      }
      const std::string base = it->path().filename().string();
      if (it->is_regular_file() && isTiffName(base) && !base.starts_with("._")) {
        const std::string relative = std::filesystem::relative(it->path(), path).generic_string();
        folders[parentOf(relative)].push_back(relative);
      }
    }
  } else {
    folders[""].push_back(path.filename().string());
  }
  if (folders.empty()) {
    throw std::runtime_error("No TIFF files in " + path.string());
  }

  std::string chosen;
  if (!options.folder.empty()) {
    std::string wanted = options.folder;
    while (wanted.ends_with('/')) {
      wanted.pop_back();
    }
    std::vector<std::string> matches;
    for (const auto& [name, files] : folders) {
      if (name == wanted || std::filesystem::path(name).filename().string() == wanted) {
        matches.push_back(name);
      }
    }
    if (matches.size() != 1) {
      std::string known;
      for (const auto& [name, files] : folders) {
        known += (known.empty() ? "" : ", ") + (name.empty() ? std::string("(top level)") : name);
      }
      throw std::invalid_argument("Folder '" + options.folder + "' " +
                                  (matches.empty() ? "not found" : "is ambiguous") +
                                  "; folders with TIFF files: " + known);
    }
    chosen = matches.front();
  } else {
    // Grey values before labels, then the most slices, then the name.
    const auto better = [&folders](const std::string& a, const std::string& b) {
      const bool la = looksLikeLabels(a);
      const bool lb = looksLikeLabels(b);
      if (la != lb) {
        return !la;
      }
      if (folders.at(a).size() != folders.at(b).size()) {
        return folders.at(a).size() > folders.at(b).size();
      }
      return a < b;
    };
    chosen = folders.begin()->first;
    for (const auto& [name, files] : folders) {
      if (better(name, chosen)) {
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
  std::sort(names.begin(), names.end(), naturalLess);

  if (names.size() == 1 && !is_zip) {
    // One file: every image in it is a slice.
    const std::filesystem::path file =
        std::filesystem::is_directory(path) ? path / names.front() : path;
    impl.multi_page = std::make_unique<detail::FileBytes>(file);
    impl.multi_pages = detail::readTiffPages(*impl.multi_page);
    for (std::size_t i = 0; i < impl.multi_pages.size(); ++i) {
      impl.slices.push_back({file, nullptr, i});
    }
    impl.first = impl.multi_pages.front();
  } else {
    for (const std::string& name : names) {
      if (is_zip) {
        impl.slices.push_back({{}, entries.at(name), 0});
      } else {
        impl.slices.push_back({path / name, nullptr, 0});
      }
    }
    const Impl::Slice& s = impl.slices.front();
    if (s.entry != nullptr) {
      const detail::MemoryBytes bytes(impl.zip->read(*s.entry));
      impl.first = detail::readTiffPages(bytes, 1).front();
    } else {
      const detail::FileBytes bytes(s.file);
      impl.first = detail::readTiffPages(bytes, 1).front();
    }
  }
  detail::checkSupported(impl.first);
  impl.pages.resize(impl.slices.size());
  impl.dims = {impl.first.width, impl.first.height, static_cast<std::int64_t>(impl.slices.size())};

  impl.is_float = impl.first.sample_format == 3;
  if (impl.is_float) {
    if (options.value_range) {
      impl.value_range = *options.value_range;
    } else {
      // A few slices spread over the stack, first and last included.
      constexpr std::size_t kSampleSlices = 9;
      const std::size_t count = std::min(kSampleSlices, impl.slices.size());
      std::array<double, 2> found{std::numeric_limits<double>::infinity(),
                                  -std::numeric_limits<double>::infinity()};
      for (std::size_t i = 0; i < count; ++i) {
        const std::size_t z = count == 1 ? 0 : i * (impl.slices.size() - 1) / (count - 1);
        const auto range = impl.floatRange(z);
        found = {std::min(found[0], range[0]), std::max(found[1], range[1])};
      }
      if (!(found[0] <= found[1])) {
        throw std::runtime_error("The float slices hold no finite values");
      }
      const double margin = found[1] > found[0] ? 0.05 * (found[1] - found[0]) : 0.5;
      impl.value_range = {found[0] - margin, found[1] + margin};
    }
    if (!(std::isfinite(impl.value_range[0]) && std::isfinite(impl.value_range[1]) &&
          impl.value_range[0] < impl.value_range[1])) {
      throw std::invalid_argument("The value range must be two finite numbers, low before high");
    }
    impl.mapping = {impl.value_range[0], (impl.value_range[1] - impl.value_range[0]) / 65535.0};
  } else if (options.value_range) {
    throw std::invalid_argument("A value range applies only to float slices");
  }
}

TiffStackSource::~TiffStackSource() = default;

std::array<std::int64_t, 3> TiffStackSource::dims() const { return impl_->dims; }

VoxelSize TiffStackSource::voxelSize() const {
  return impl_->voxel_size.value_or(fileVoxelSize().value_or(VoxelSize(1.0)));
}

std::optional<VoxelSize> TiffStackSource::fileVoxelSize() const {
  const detail::TiffPage& page = impl_->first;
  if (!(page.pixel_size_mm > 0.0)) {
    return std::nullopt;
  }
  const double height = page.pixel_height_mm > 0.0 ? page.pixel_height_mm : page.pixel_size_mm;
  const double spacing = page.slice_spacing_mm > 0.0 ? page.slice_spacing_mm : page.pixel_size_mm;
  return VoxelSize(page.pixel_size_mm, height, spacing);
}

int TiffStackSource::bitsPerSample() const { return impl_->first.bits; }

bool TiffStackSource::isFloat() const { return impl_->is_float; }

ValueMapping TiffStackSource::valueMapping() const { return impl_->mapping; }

std::array<double, 2> TiffStackSource::valueRange() const { return impl_->value_range; }

std::uint64_t TiffStackSource::clippedValues() const {
  const std::lock_guard lock(impl_->mutex);
  return impl_->clipped;
}

const std::string& TiffStackSource::folder() const { return impl_->folder; }

const std::vector<std::string>& TiffStackSource::otherFolders() const {
  return impl_->other_folders;
}

void TiffStackSource::readRegion(const Box& box, std::span<std::uint16_t> out) const {
  for (std::size_t axis = 0; axis < 3; ++axis) {
    if (box.min[axis] < 0 || box.max[axis] > impl_->dims[axis] || box.min[axis] > box.max[axis]) {
      throw std::out_of_range("Region outside the volume");
    }
  }
  if (static_cast<std::int64_t>(out.size()) != box.voxelCount()) {
    throw std::invalid_argument("Output size does not match the region");
  }
  const std::int64_t nx = box.size(0);
  const std::int64_t ny = box.size(1);
  for (std::int64_t z = box.min[2]; z < box.max[2]; ++z) {
    const auto slice = static_cast<std::size_t>(z);
    const detail::TiffPage page = impl_->pageOf(slice);
    const std::int64_t cw = page.chunk_width;
    const std::int64_t ch = page.chunk_height;
    for (std::int64_t y = box.min[1]; y < box.max[1];) {
      const std::int64_t row = y / ch;
      const std::int64_t y_end = std::min(box.max[1], (row + 1) * ch);
      for (std::int64_t x = box.min[0]; x < box.max[0];) {
        const std::int64_t column = x / cw;
        const std::int64_t x_end = std::min(box.max[0], (column + 1) * cw);
        const auto data =
            impl_->chunk(slice, static_cast<std::size_t>(row * page.chunks_across + column));
        for (std::int64_t yy = y; yy < y_end; ++yy) {
          std::copy_n(
              data->data() + (yy - row * ch) * cw + (x - column * cw), x_end - x,
              out.data() + ((z - box.min[2]) * ny + (yy - box.min[1])) * nx + (x - box.min[0]));
        }
        x = x_end;
      }
      y = y_end;
    }
  }
}

}  // namespace voxelsieve
